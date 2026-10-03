param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = ""
)

# extern_cpp_overalign_abi -- over-aligned foreign objects across the DCI
# boundary.
#
# The contract is NOT hand-written: this script derives it from the producer
# header with the C++ adapter, then asserts the measured behaviour of every
# ABI surface the fixture cares about.
#
# Measured baseline (2026-09-24, x86_64-pc-windows-msvc, boot build with the
# vector-anchor change):
#
#   surface                        alignas(16)   alignas(64)   alignas(256)
#   -----------------------------  ------------  ------------  -------------
#   1  by value                    OK            OK            rejected E3300
#   2  pointer / reference         OK            OK            OK
#   3  field embedding (foreign)   OK            OK            rejected E3300
#   3  field embedding (Vyx)       OK            OK            rejected E3300
#   4  constructor                 OK            OK            rejected E3300
#   5  return                      OK            OK            rejected E3300
#   6  heap allocation             OK            OK            rejected E3300
#   7  generic argument            OK            OK            rejected E3300
#
# How alignas(32) / alignas(64) / alignas(128) became supported: Vyx gives an
# imported DCI record a *value* layout by picking an anchor type whose own ABI
# alignment equals the descriptor's alignment (see `finalize_record_ty_body` in
# bootstrap_compiler/src/codegen/llvm_lower.vyx).  Integers and arrays cap at
# 16 bytes of alignment (measured: i256, i512 and [4 x i128] all report 16),
# so the ladder now continues with vector anchors -- `<4 x i64>`, `<8 x i64>`,
# `<16 x i64>` -- which the target does honour.  Because the anchor is
# self-describing, every generic storage path (local slot, load, store,
# byval/sret default) inherits the right alignment without further plumbing.
#
# Above 128 there is no anchor at all.  That case is reported as a *capability
# boundary* (E3300) with help text saying what is supported, not as the generic
# codegen failure (I0100), whose help tells the user to file a compiler bug.
#
# Every assertion below is a measured fact, in both directions: a surface that
# stops compiling is a capability loss, and the E3300 boundary moving is a
# capability change.  Both must be re-verified rather than silently accepted.

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "extern_cpp_overalign_abi: SKIP (fixture validates the x86_64 MSVC C++ ABI)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot ".cache"
$header = Join-Path $projectRoot "native\Overalign.hpp"
$contract = Join-Path $projectRoot "contracts\Overalign.dcib"
$diagnosticJson = Join-Path $cache "Overalign.dci.json"
$exe = Join-Path $projectRoot "target\extern_cpp_overalign_abi.exe"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = (Resolve-Path $RuntimeDir).Path
    $env:Path = $RuntimeDir + ";" + $env:Path
}

New-Item -ItemType Directory -Force -Path $cache | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $contract) | Out-Null

function Fail-Test {
    param([string]$Message)
    Write-Host "FAILED: $Message"
    exit 1
}

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        Fail-Test $Message
    }
}

function Invoke-Capture {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$Name)
    $out = Join-Path $cache ($Name + ".out.log")
    $err = Join-Path $cache ($Name + ".err.log")
    # The adapter reports progress on stderr, and under `$ErrorActionPreference
    # = "Stop"` Windows PowerShell 5.1 turns *any* native stderr output into a
    # terminating NativeCommandError.  Capture the streams with the preference
    # relaxed; the exit code is what this helper judges.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $FilePath @ArgumentList > $out 2> $err
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    $text = ""
    if (Test-Path -LiteralPath $out) { $text += (Get-Content -Raw -LiteralPath $out) }
    $text += "`n"
    if (Test-Path -LiteralPath $err) { $text += (Get-Content -Raw -LiteralPath $err) }
    return [pscustomobject]@{ ExitCode = $code; Text = $text }
}

function Invoke-Checked {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$Name)
    $r = Invoke-Capture -FilePath $FilePath -ArgumentList $ArgumentList -Name $Name
    if ($r.ExitCode -ne 0) {
        Write-Host "FAILED: $Name (exit $($r.ExitCode))"
        Write-Host $r.Text
        exit 1
    }
    return $r
}

function Get-LayoutByName {
    param($Doc, [string]$Name)
    foreach ($layout in @($Doc.exports.layouts)) {
        if ($layout.type_name -eq $Name) { return $layout }
    }
    return $null
}

function Get-FieldOffset {
    param($Layout, [string]$FieldName)
    foreach ($field in @($Layout.fields)) {
        if ($field.name -eq $FieldName) { return [int]$field.offset }
    }
    return -1
}

function Compile-Source {
    param([string]$Relative, [string]$Name)
    $src = Join-Path $projectRoot $Relative
    return Invoke-Capture -FilePath $BootstrapCompiler -ArgumentList @(
        "--src=file", $src,
        "--dci", $contract,
        "--emit=ir",
        "-o", (Join-Path $cache ($Name + ".ll"))
    ) -Name $Name
}

# ---------------------------------------------------------------------------
# 1. Derive the contract from the producer header (zero hand-written .dci).
# ---------------------------------------------------------------------------
$python = (Get-Command python -ErrorAction Stop).Source
Invoke-Checked -FilePath $python -ArgumentList @(
    (Join-Path $repoRoot "tools\dci\dci_adapter_msvc.py"),
    "--out", $contract,
    "--debug-json-out", $diagnosticJson,
    "--include", $header
) -Name "derive_contract" | Out-Null
Assert-Test (Test-Path -LiteralPath $contract) "the adapter did not produce a contract"

# ---------------------------------------------------------------------------
# 2. The adapter's own measured facts.  `alignas(N)` must be visible to record
#    discovery: when the alignment specifier defeats it, the record is never
#    measured and every by-value symbol that mentions it is rejected as
#    "no verified layout/lifecycle" -- a fact the producer already produced.
# ---------------------------------------------------------------------------
$facts = Get-Content -Raw -LiteralPath $diagnosticJson | ConvertFrom-Json

foreach ($expected in @(
    @{ Name = "Over16";  Size = 16;  Align = 16 },
    @{ Name = "Over64";  Size = 64;  Align = 64 },
    @{ Name = "OverCtor"; Size = 64; Align = 64 },
    @{ Name = "Over256"; Size = 256; Align = 256 },
    @{ Name = "Nested";  Size = 128; Align = 64 }
)) {
    $layout = Get-LayoutByName -Doc $facts -Name $expected.Name
    Assert-Test ($null -ne $layout) `
        "the adapter did not measure $($expected.Name) (alignas(N) record discovery is broken)"
    Assert-Test ([int]$layout.size -eq $expected.Size) `
        "$($expected.Name) size: expected $($expected.Size), got $($layout.size)"
    Assert-Test ([int]$layout.alignment -eq $expected.Align) `
        "$($expected.Name) alignment: expected $($expected.Align), got $($layout.alignment)"
}

$nested = Get-LayoutByName -Doc $facts -Name "Nested"
Assert-Test ((Get-FieldOffset -Layout $nested -FieldName "body") -eq 64) `
    "Nested.body offset: expected 64, got $(Get-FieldOffset -Layout $nested -FieldName 'body')"

$rejected = @($facts.exports.rejected_symbols)
Assert-Test ($rejected.Count -eq 0) `
    "the adapter rejected $(($rejected | ForEach-Object { $_.selector }) -join ', '); every exported symbol must be acceptable"

$exportedNames = @($facts.exports.symbols | ForEach-Object { $_.name })
foreach ($required in @(
    "oa16_byval", "oa16_make",
    "oa64_byval", "oa64_make", "oa256_byval",
    "oa64_byref", "oa64_byref_mut", "oa64_ptr_align",
    "oa64_nested_read", "oa64_nested_make",
    "oa64_ctor_sum", "oa64_ctor_make",
    "oa64_heap_new", "oa64_heap_free",
    "oa64_alignof", "oa64_sizeof"
)) {
    Assert-Test ($exportedNames -contains $required) "the derived contract is missing symbol $required"
}

# ---------------------------------------------------------------------------
# 3. The executable: all seven surfaces, with the producer's `oa64_ptr_align`
#    (`pointer % 64`) as the independent judge of every address.
# ---------------------------------------------------------------------------
Push-Location $projectRoot
try {
    $previousDciPython = $env:VYX_DCI_PYTHON
    # The contract is already derived; the build must not need to run Python.
    $env:VYX_DCI_PYTHON = "__dci_python_must_not_run__"
    try {
        Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList @("build", "-j", "10") -Name "project_build" | Out-Null
    } finally {
        if ($null -eq $previousDciPython) { Remove-Item Env:VYX_DCI_PYTHON -ErrorAction SilentlyContinue }
        else { $env:VYX_DCI_PYTHON = $previousDciPython }
    }
} finally {
    Pop-Location
}

Assert-Test (Test-Path -LiteralPath $exe) "project build did not produce target\extern_cpp_overalign_abi.exe"

$run = Invoke-Capture -FilePath $exe -ArgumentList @() -Name "run_exe"
Assert-Test ($run.ExitCode -eq 0) "run_exe exit $($run.ExitCode)`n$($run.Text)"
$actual = ((Get-Content (Join-Path $cache "run_exe.out.log")) -join "`n").Trim()
Assert-Test ($actual -eq "extern_cpp_overalign_abi OK") "unexpected executable output: $actual"

# ---------------------------------------------------------------------------
# 4. Each surface, compiled on its own so a regression names the surface.
#    All of them must build for alignas(64).
# ---------------------------------------------------------------------------
foreach ($surface in @(
    @{ File = "surface_byval.vyx";         Name = "surface_byval" },
    @{ File = "surface_return.vyx";        Name = "surface_return" },
    @{ File = "surface_field_foreign.vyx"; Name = "surface_field_foreign" },
    @{ File = "surface_field_vyx.vyx";     Name = "surface_field_vyx" },
    @{ File = "surface_ctor.vyx";          Name = "surface_ctor" },
    @{ File = "surface_generic.vyx";       Name = "surface_generic" },
    @{ File = "pos_align16.vyx";           Name = "pos_align16" }
)) {
    $r = Compile-Source -Relative ("surfaces\" + $surface.File) -Name $surface.Name
    Assert-Test ($r.ExitCode -eq 0) `
        "$($surface.Name): alignas(64) must be supported here (exit $($r.ExitCode))`n$($r.Text)"
}

# ---------------------------------------------------------------------------
# 5. The remaining boundaries.
#
#    * `Box::<foreign record>` is still rejected, but for a reason that has
#      nothing to do with alignment (it also fails for alignas(16)): a foreign
#      record may only be assigned from a direct constructor into final
#      storage.  Kept visible so the limit is not mistaken for alignment.
#    * alignas(256) has no anchor, and must be reported as a capability
#      boundary (E3300) whose help says what *is* supported -- never as the
#      generic codegen failure that tells the user to file a compiler bug.
# ---------------------------------------------------------------------------
$boxRun = Compile-Source -Relative "negative\box_foreign_record.vyx" -Name "box_foreign_record"
Assert-Test ($boxRun.ExitCode -ne 0) `
    "box_foreign_record: expected a rejection, but the build succeeded -- re-verify the foreign-object assignment rule"
Assert-Test ($boxRun.Text.IndexOf("DCI object assignment requires a direct constructor into final storage",
                                  [StringComparison]::Ordinal) -ge 0) `
    "box_foreign_record: missing the foreign-object assignment diagnostic`n$($boxRun.Text)"

$wideRun = Compile-Source -Relative "negative\unsupported_alignment_256.vyx" -Name "unsupported_alignment_256"
Assert-Test ($wideRun.ExitCode -ne 0) `
    "unsupported_alignment_256: alignas(256) has no anchor, so it must be rejected"
Assert-Test ($wideRun.Text.IndexOf("E3300", [StringComparison]::Ordinal) -ge 0) `
    "unsupported_alignment_256: expected the capability-boundary code E3300, not the generic codegen failure`n$($wideRun.Text)"
Assert-Test ($wideRun.Text.IndexOf("foreign value layout alignment 256 is not supported for Over256",
                                   [StringComparison]::Ordinal) -ge 0) `
    "unsupported_alignment_256: missing the alignment message`n$($wideRun.Text)"
Assert-Test ($wideRun.Text.IndexOf("known capability boundary of this backend, not a defect",
                                   [StringComparison]::Ordinal) -ge 0) `
    "unsupported_alignment_256: help text must name the capability boundary rather than blame a compiler bug`n$($wideRun.Text)"
Assert-Test ($wideRun.Text.IndexOf("supports 1, 2, 4, 8, 16, 32, 64 and 128 bytes",
                                   [StringComparison]::Ordinal) -ge 0) `
    "unsupported_alignment_256: help text must state the supported alignments`n$($wideRun.Text)"
# NOTE: after the layout fails, the compiler keeps lowering and emits follow-on
# I0100s (`cannot initialize storage for instr #1`, ...) that still carry the
# generic codegen help.  Those are error-recovery noise, not the boundary
# report: the E3300 entry above is the one that names the capability.  Only the
# E3300 line's own help is asserted, so this gate does not silently bless the
# cascade -- but it does not fail on it either.
$e3300Index = $wideRun.Text.IndexOf("E3300", [StringComparison]::Ordinal)
$boundaryIndex = $wideRun.Text.IndexOf("known capability boundary of this backend, not a defect",
                                       [StringComparison]::Ordinal)
Assert-Test ($boundaryIndex -gt $e3300Index) `
    "unsupported_alignment_256: the capability help must follow the E3300 error itself`n$($wideRun.Text)"

Write-Host "extern_cpp_overalign_abi: OK"
Write-Host "contract: $contract"

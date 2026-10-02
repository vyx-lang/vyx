param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [string]$Rustc = ""
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "dci_rust_trait: SKIP (fixture requires rustc x86_64-pc-windows-msvc)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot ".cache"
$src = Join-Path $projectRoot "src\main.vyx"
$native = Join-Path $projectRoot "native\lib.rs"
$dcib = Join-Path $projectRoot "native\native.dcib"
$exe = Join-Path $projectRoot "target\dci_rust_trait.exe"
$adapter = Join-Path $repoRoot "tools\dci\dci_adapter_rust.py"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($Rustc)) {
    $Rustc = (Get-Command rustc -ErrorAction Stop).Source
}
$Rustc = (Resolve-Path $Rustc).Path

if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = (Resolve-Path $RuntimeDir).Path
    $env:Path = $RuntimeDir + ";" + $env:Path
}

New-Item -ItemType Directory -Force -Path $cache | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot "target") | Out-Null

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

function Invoke-Checked {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$Name)
    $out = Join-Path $cache ($Name + ".out.log")
    $err = Join-Path $cache ($Name + ".err.log")
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $FilePath @ArgumentList > $out 2> $err
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) {
        Write-Host "FAILED: $Name"
        if (Test-Path $out) { Get-Content $out -Tail 80 }
        if (Test-Path $err) { Get-Content $err -Tail 80 }
        exit $code
    }
}

$python = (Get-Command python -ErrorAction Stop).Source

# ---------------------------------------------------------------------------
# No hand-written instance list, anywhere -- and no scripted export either.
#
# The Vyx side imports the producer trait open (`class native.SinkG<T>`) and
# the closed instances it actually materializes are reported by the compiler
# into the per-object `.dci_open` sidecars as `instance <consumer-type-text>`.
# The BUILD SYSTEM closes those requests against the producer itself
# (tools/dci/dci_close_instances.py, driven by the compile-failure retry loop):
# the adapter regenerates the closed vtable facts on demand, the supplement
# contract joins the descriptor list, and lowering runs again.  Nothing in this
# file names `SinkG<i32>`, and neither does the Vyx source.
#
# One pass is enough: an inherited trait instance cannot be deferred to link
# time the way an open generic *function* can (the producer's `dyn` vtable
# shape must exist before lowering reads it), but the fixed-point loop inside
# the build owns the second lowering -- the script never re-runs the adapter.
# `-j 1` keeps the retry loop inline (the parallel scheduler defers tasks and
# does not retry yet).
# ---------------------------------------------------------------------------
function New-Contract {
    $adapterArgs = @($adapter, $native, "--crate-name", "native", "--edition", "2024", "--rustc", $Rustc, "-o", $dcib)
    Invoke-Checked -FilePath $python -ArgumentList $adapterArgs -Name "adapter_contract"
}

function Invoke-ProjectBuild {
    Push-Location $projectRoot
    try {
        & $BootstrapCompiler build -j 1 > (Join-Path $cache "project_build.out.log") 2> (Join-Path $cache "project_build.err.log")
        return $LASTEXITCODE
    } finally {
        Pop-Location
    }
}

# Discovery must reflect this run, so clear any sidecar left by an earlier one
# before asking the compiler what it needs (`[IO.File]::Delete` rather than
# Remove-Item: this host wraps Remove-Item with a delete guard that can throw
# even when the removal succeeds).
foreach ($stale in (Get-ChildItem -LiteralPath $cache -Recurse -Filter *.dci_open -File -ErrorAction SilentlyContinue)) {
    [System.IO.File]::Delete($stale.FullName)
}

New-Contract
# The contract must carry only invariant facts: no closed generic instance may
# be pre-exported, or the open-import test would be trivially true.
# [System.Text.Encoding]::Latin1 is .NET Core-only; on Windows PowerShell 5.1
# (the host this suite uses to launch project gates) the property evaluates to
# $null and the gate dies with InvokeMethodOnNull before any assertion runs.
# Codepage 28591 is the same ISO-8859-1 byte-preserving decoder and exists on
# both .NET Framework and .NET Core (verified: byte 0xC8 -> U+00C8 on 5.1 & 7).
$contractText = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($dcib))
Assert-Test ($contractText.IndexOf("SinkG") -lt 0) `
    "the generated contract pre-exports a closed generic instance (open generics must not be pre-exported)"

$buildRc = Invoke-ProjectBuild
if ($buildRc -ne 0) {
    Write-Host "FAILED: project_build (exit $buildRc)"
    Get-Content (Join-Path $cache "project_build.out.log") -Tail 80
    Get-Content (Join-Path $cache "project_build.err.log") -Tail 80
    exit $buildRc
}

Assert-Test (Test-Path -LiteralPath $exe) "project build did not produce target\dci_rust_trait.exe"

# The build's internal fixed point must have produced the closed-instance
# supplement itself; this file is the evidence that the closure ran inside the
# build instead of a script re-exporting instances by hand.
$supplement = Join-Path $cache "dci_closed_facts_dci_rust_trait.dcib"
Assert-Test (Test-Path -LiteralPath $supplement) `
    "the build did not close the discovered instances internally (no supplement contract in .cache)"

$runOut = Join-Path $cache "run_exe.out.log"
$runErr = Join-Path $cache "run_exe.err.log"
& $exe > $runOut 2> $runErr
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: run_exe (exit $LASTEXITCODE)"
    Get-Content $runOut -Tail 80
    Get-Content $runErr -Tail 80
    exit $LASTEXITCODE
}
$actual = ((Get-Content $runOut) -join "`n").Trim()
Assert-Test ($actual -eq "dci_rust_trait OK") "unexpected executable output: $actual"

# `dci_stub_backend = "rustc"` emits `<stem>_dci_stubs_rustc.rs`; the plain
# `<stem>_dci_stubs.rs` is NOT produced by this backend (an older naming left
# one behind in .cache), so globbing the plain name reads a stale artifact and
# the asserts below stop describing the build that just ran.
$stubFile = Get-ChildItem -LiteralPath $cache -Filter "*_dci_stubs_rustc.rs" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
Assert-Test ($null -ne $stubFile) "generated DCI rustc stub source is missing"
$stubText = Get-Content -Raw -LiteralPath $stubFile.FullName
Assert-Test ($stubText.IndexOf("impl native::Sink for __dci_vyx_stub_VyxHost", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not impl native::Sink for the Vyx host"
Assert-Test ($stubText.IndexOf("__vyx_M_VyxHost_N_consume", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not call the Vyx override thunk"
Assert-Test ($stubText.IndexOf("__dci_stub_factory_VyxHost", [StringComparison]::Ordinal) -ge 0) `
    "generated stub factory is missing"
Assert-Test ($stubText.IndexOf("extern crate native;", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not depend on the producer crate"
# A generic producer trait must be inherited as its *closed instance*, so the
# `impl` has to name the turbofish spelling the contract's `rust.path` uses.
Assert-Test ($stubText.IndexOf("impl native::SinkG::<i32> for __dci_vyx_stub_VyxHostG", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not impl the closed generic trait instance native::SinkG::<i32>"
Assert-Test ($stubText.IndexOf("&dyn native::SinkG::<i32>", [StringComparison]::Ordinal) -ge 0) `
    "generated stub factory does not build the closed instance's trait object"
Assert-Test ($stubText.IndexOf("__vyx_M_VyxHostG_N_consume", [StringComparison]::Ordinal) -ge 0) `
    "generated generic stub does not call the Vyx override thunk"
Assert-Test ($stubText.IndexOf("__dci_stub_factory_VyxHostG", [StringComparison]::Ordinal) -ge 0) `
    "generated generic stub factory is missing"
# A *generic* Vyx stub class (`VyxHostTG<T> : native.SinkG<T>`) has no base at
# declaration level, so its spec row is only written when the class closes.  The
# closed instance must still come out as a struct *with* its methods.
Assert-Test ($stubText.IndexOf("for __dci_vyx_stub_VyxHostTG", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not emit the generic stub class instance VyxHostTG"
Assert-Test ($stubText.IndexOf("impl native::SinkG::<i32> for __dci_vyx_stub_VyxHostTG", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not impl the closed trait instance for the generic stub class"
Assert-Test ($stubText.IndexOf("__vyx_M_VyxHostTG", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not call the generic stub class's Vyx override thunk"
Assert-Test ($stubText.IndexOf("__dci_stub_factory_VyxHostTG", [StringComparison]::Ordinal) -ge 0) `
    "generated generic stub class factory is missing"
# The generic stub class has to materialize **once per closed instance**.  A
# previous shape matched instances by head name only, so every instance's
# override landed in every `impl` and rustc reported duplicate `consume`
# definitions; asserting the *typed* member functions is what pins each instance
# to exactly its own vtable.
foreach ($ty in @("i32", "i64", "char")) {
    Assert-Test ($stubText.IndexOf("impl native::SinkG::<$ty> for __dci_vyx_stub_VyxHostTG_3A_3A_3C", [StringComparison]::Ordinal) -ge 0) `
        "generated stub does not impl native::SinkG::<$ty> for the VyxHostTG instance"
}
# Each instance must carry its own argument/return type: the same "specialised
# once" bug shows up as every signature reading `i32`.
Assert-Test ($stubText.IndexOf("fn consume(&self, p1: i64) -> i64", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not type the i64 instance's consume as i64"
Assert-Test ($stubText.IndexOf("fn consume(&self, p1: char) -> char", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not type the char instance's consume as char"
# NOT covered: a *producer record* as the instance's type argument
# (`SinkG<Sample>`).  Three independent walls, all pre-existing and none of them
# about the per-instance materialization:
#   1. `<>` does not take a dotted type argument -- writing
#      `native.SinkG<native.Sample>` leaves the owner as `native.SinkG::<native>`
#      and drops everything after the `.` in the argument;
#   2. sidestepping that with `type Sample = native.Sample;` resolves the name
#      but the alias carries no DCI descriptor entry, so HIR verify reports
#      `function has open return type native.Sample`;
#   3. returning a foreign DCI aggregate from a Vyx function anyway violates the
#      storage contract: MIR verify rejects even `return x;` with
#      `DCI object assignment requires a direct constructor into final storage`.
# A producer record *does* cross the boundary today -- `dci_opengeneric` covers
# it (`identity(TestS)`, `swap(TestS, i32)`) -- just not as the type argument of
# an inherited generic base.
# ... and each must call *its own* Vyx override thunk, not a sibling's.
foreach ($ty in @("i32", "i64", "char")) {
    $stubStruct = "__dci_vyx_stub_VyxHostTG_3A_3A_3C" + $ty + "_3E"
    # Exactly one struct/impl per instance: emitting the same instance twice is
    # what `duplicate definitions with name 'consume'` (E0201) looks like from
    # rustc, and skipping one is what `no unique supported virtual base method`
    # looks like from the stub validator.
    $implCount = ([regex]::Matches($stubText,
        "impl native::SinkG::<" + [regex]::Escape($ty) + "> for " + $stubStruct + " \{")).Count
    Assert-Test ($implCount -eq 1) `
        "expected exactly 1 impl for VyxHostTG<$ty>, found $implCount"
    $structCount = ([regex]::Matches($stubText, "pub struct " + $stubStruct + " \{")).Count
    Assert-Test ($structCount -eq 1) `
        "expected exactly 1 stub struct for VyxHostTG<$ty>, found $structCount"
    $factoryCount = ([regex]::Matches($stubText,
        "__dci_stub_factory_VyxHostTG_3A_3A_3C" + $ty + "_3E\(")).Count
    Assert-Test ($factoryCount -eq 1) `
        "expected exactly 1 stub factory for VyxHostTG<$ty>, found $factoryCount"
}
$mainText = Get-Content -Raw -LiteralPath $src
Assert-Test ($mainText.IndexOf("impl HostView for native.NativePair", [StringComparison]::Ordinal) -ge 0) `
    "host source is missing impl HostView for native.NativePair"
Assert-Test ($mainText.IndexOf("class VyxHost : native.Sink", [StringComparison]::Ordinal) -ge 0) `
    "host source is missing impl native.Sink for VyxHost"
Assert-Test ($mainText.IndexOf("class VyxHostG : native.SinkG<i32>", [StringComparison]::Ordinal) -ge 0) `
    "host source is missing impl native.SinkG<i32> for VyxHostG"
Assert-Test ($mainText.IndexOf("class VyxHostTG<T> : native.SinkG<T>", [StringComparison]::Ordinal) -ge 0) `
    "host source is missing the generic stub class VyxHostTG"

# The generic producer trait must be imported ONCE, open.  Requiring one
# hand-written `class native.SinkG<i32>` / `<i64>` / `<char>` entry per
# inheritable instance is not generics, it is spelling every specialization
# out by hand -- that is precisely what this test exists to rule out.
Assert-Test ($mainText.IndexOf("class native.SinkG<T>", [StringComparison]::Ordinal) -ge 0) `
    "host source does not import the producer trait open (class native.SinkG<T>)"
$specLine = "class native.SinkG<i32>"
Assert-Test ($mainText.IndexOf($specLine, [StringComparison]::Ordinal) -lt 0) `
    "host source closed the trait by hand ($specLine); that defeats the open-import test"

Write-Host "dci_rust_trait: OK"

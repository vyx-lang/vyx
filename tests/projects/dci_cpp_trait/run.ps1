param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = ""
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "dci_cpp_trait: SKIP (fixture requires the repo clang toolchain on Windows)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot ".cache"
$src = Join-Path $projectRoot "src\main.vyx"
$overlay = Join-Path $projectRoot "native\sink.hpp"
$dcib = Join-Path $projectRoot "contracts\sink.dcib"
$exe = Join-Path $projectRoot "target\dci_cpp_trait.exe"
$dciPy = Join-Path $repoRoot "tools\dci\dci.py"

$LLVM = Join-Path $repoRoot "clang"
$env:LLVM_ROOT = $LLVM
$env:PATH = "$LLVM\bin;$repoRoot\bootstrap_compiler\out;$env:PATH"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path
$Clangxx = Join-Path $LLVM "bin\clang++.exe"
if (-not (Test-Path $Clangxx)) { throw "clang++ missing: $Clangxx" }

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
# Open generics only.  The adapter derives the contract from the producer
# header; `SinkG<T>` is a template and never pre-instantiated, so the contract
# carries the closed, non-generic surface plus the invariant record layouts
# and nothing else.  The closed instances the Vyx consumer inherits are closed
# by the BUILD (compile -> .dci_open requests -> dci_close_instances ->
# re-lower); no script in this repo passes --export-instance for this fixture.
# ---------------------------------------------------------------------------
$adapterArgs = @($dciPy, "adapter", "--language", "cpp", $overlay,
    "-o", $dcib,
    "--triplet", "windows_x64",
    "--std", "c++17",
    "--toolchain", "clang",
    "--clang", $Clangxx,
    "-j", "4",
    "--project-root", (Join-Path $projectRoot "native"))
Invoke-Checked -FilePath $python -ArgumentList $adapterArgs -Name "adapter_contract"

# [System.Text.Encoding]::Latin1 is .NET Core-only; on Windows PowerShell 5.1
# (the host this suite uses to launch project gates) the property evaluates to
# $null and the gate dies with InvokeMethodOnNull before any assertion runs.
# Codepage 28591 is the same ISO-8859-1 byte-preserving decoder and exists on
# both .NET Framework and .NET Core (verified: byte 0xC8 -> U+00C8 on 5.1 & 7).
$contractText = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($dcib))
Assert-Test ($contractText.IndexOf("SinkG") -lt 0) `
    "the generated contract pre-exports a closed generic instance (open generics must not be pre-exported)"

# Discovery must reflect this run; the same delete-guard note as dci_rust_trait.
foreach ($stale in (Get-ChildItem -LiteralPath $cache -Recurse -Filter *.dci_open -File -ErrorAction SilentlyContinue)) {
    [System.IO.File]::Delete($stale.FullName)
}
# A stale supplement from a previous run would (a) join the descriptor list at
# planning time and (b) suppress re-closing its instances; delete it so the
# closure loop starts from the freshly generated open contract.
$staleSupplement = Join-Path $cache "dci_closed_facts_dci_cpp_trait.dcib"
if (Test-Path -LiteralPath $staleSupplement) {
    [System.IO.File]::Delete($staleSupplement)
    [System.IO.File]::Delete("$staleSupplement.log")
}

Push-Location $projectRoot
try {
    & $BootstrapCompiler build -j 1 > (Join-Path $cache "project_build.out.log") 2> (Join-Path $cache "project_build.err.log")
    $buildRc = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($buildRc -ne 0) {
    Write-Host "FAILED: project_build (exit $buildRc)"
    Get-Content (Join-Path $cache "project_build.out.log") -Tail 80
    Get-Content (Join-Path $cache "project_build.err.log") -Tail 80
    exit $buildRc
}

Assert-Test (Test-Path -LiteralPath $exe) "project build did not produce target\dci_cpp_trait.exe"

# Evidence the build closed the discovered instances internally.
$supplement = Join-Path $cache "dci_closed_facts_dci_cpp_trait.dcib"
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
Assert-Test ($actual -eq "dci_cpp_trait OK") "unexpected executable output: $actual"

# A cached object is only reusable with its DCI discovery sidecar and the
# closed-facts contract.  Simulate losing those derived artifacts, then make
# the same build reconstruct all four C++ method specializations.
$stubSource = Join-Path $cache "dci_cpp_trait_src_main_vyx_dci_stubs_clang_cpp.cpp"
$specializationPattern = '(?m)^template <>.* SinkG<[^>]+>::consume\('
Assert-Test (([regex]::Matches((Get-Content -Raw $stubSource), $specializationPattern)).Count -eq 4) `
    "cold build did not emit four closed SinkG::consume specializations"
foreach ($stale in (Get-ChildItem -LiteralPath $cache -Recurse -Filter *.dci_open -File -ErrorAction SilentlyContinue)) {
    [System.IO.File]::Delete($stale.FullName)
}
[System.IO.File]::Delete($supplement)
[System.IO.File]::Delete("$supplement.log")
Push-Location $projectRoot
try {
    & $BootstrapCompiler build -j 1 > (Join-Path $cache "project_build_warm.out.log") 2> (Join-Path $cache "project_build_warm.err.log")
    $warmRc = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($warmRc -ne 0) {
    Write-Host "FAILED: project_build_warm (exit $warmRc)"
    Get-Content (Join-Path $cache "project_build_warm.out.log") -Tail 80
    Get-Content (Join-Path $cache "project_build_warm.err.log") -Tail 80
    exit $warmRc
}
Assert-Test (Test-Path -LiteralPath $supplement) "warm build did not recreate the DCI supplement"
Assert-Test (([regex]::Matches((Get-Content -Raw $stubSource), $specializationPattern)).Count -eq 4) `
    "warm build dropped closed SinkG::consume specializations"
$warmRunOut = Join-Path $cache "run_exe_warm.out.log"
$warmRunErr = Join-Path $cache "run_exe_warm.err.log"
& $exe > $warmRunOut 2> $warmRunErr
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: run_exe_warm (exit $LASTEXITCODE)"
    Get-Content $warmRunOut -Tail 80
    Get-Content $warmRunErr -Tail 80
    exit $LASTEXITCODE
}
Assert-Test (((Get-Content $warmRunOut) -join "`n").Trim() -eq "dci_cpp_trait OK") `
    "warm executable returned unexpected output"

Write-Host "dci_cpp_trait: OK"

param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = ""
)

# DCI + CUDA acceptance test.
#
# 1. nvcc builds the CUDA provider (native/cuda_ops.cu) into a static library.
# 2. The Vyx build consumes contracts/CudaOps.dcib - a contract the cpp adapter
#    derived from native/cuda_ops.hpp - and links that library in.
# 3. The consumer calls CudaVector::fill_and_sum / scale_and_sum, i.e. real
#    <<<>>> kernel launches, through the plain C++ ABI.
#
# SKIP (exit 77) when this is not Windows or no CUDA toolkit/host toolchain is
# installed; a CUDA toolkit that cannot launch a kernel is a real failure.

$ErrorActionPreference = "Stop"

# $IsWindows only exists on PowerShell 6+; $env:OS is absent in stripped
# environments, so fall back to the CLR platform instead of trusting it.
if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    $isWindowsHost = [bool]$IsWindows
} elseif (-not [string]::IsNullOrWhiteSpace($env:OS)) {
    $isWindowsHost = ($env:OS -eq "Windows_NT")
} else {
    $isWindowsHost = ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT)
}
if (-not $isWindowsHost) {
    Write-Host "dci_cuda: SKIP (CUDA provider targets the x86_64 MSVC C++ ABI)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot ".cache"
$prebuilt = Join-Path $projectRoot "native\prebuilt"
$cu = Join-Path $projectRoot "native\cuda_ops.cu"
$contract = Join-Path $projectRoot "contracts\CudaOps.dcib"
$exe = Join-Path $projectRoot "target\dci_cuda.exe"

function Write-Skip {
    param([string]$Reason)
    Write-Host ("dci_cuda: SKIP (" + $Reason + ")")
    exit 77
}

function Fail-Test {
    param([string]$Message)
    Write-Host "FAILED: $Message"
    exit 1
}

function Test-CommandAvailable {
    param([string]$Name)
    return ($null -ne (Get-Command $Name -ErrorAction SilentlyContinue))
}

# --- CUDA toolkit -----------------------------------------------------------

$cudaRoot = ""
if (-not [string]::IsNullOrWhiteSpace($env:CUDA_PATH)) {
    if (Test-Path -LiteralPath (Join-Path $env:CUDA_PATH "bin\nvcc.exe")) {
        $cudaRoot = $env:CUDA_PATH
    }
}
if ($cudaRoot -eq "") {
    $nvccCommand = Get-Command nvcc.exe -ErrorAction SilentlyContinue
    if ($null -ne $nvccCommand) {
        $cudaRoot = Split-Path -Parent (Split-Path -Parent $nvccCommand.Source)
    }
}
if ($cudaRoot -eq "") {
    # Non-standard installs can point the fixture at their toolkit root; the
    # default mirrors the CUDA installer's own location.
    $toolkitRoot = $env:VYX_CUDA_TOOLKIT_ROOT
    if ([string]::IsNullOrWhiteSpace($toolkitRoot)) {
        $toolkitRoot = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA"
    }
    if (Test-Path -LiteralPath $toolkitRoot) {
        foreach ($dir in (Get-ChildItem -LiteralPath $toolkitRoot -Directory | Sort-Object Name -Descending)) {
            if (Test-Path -LiteralPath (Join-Path $dir.FullName "bin\nvcc.exe")) {
                $cudaRoot = $dir.FullName
                break
            }
        }
    }
}
if ($cudaRoot -eq "") { Write-Skip "no CUDA toolkit" }

$nvcc = Join-Path $cudaRoot "bin\nvcc.exe"
$cudart = Join-Path $cudaRoot "lib\x64\cudart.lib"
if (-not (Test-Path -LiteralPath $nvcc -PathType Leaf)) { Write-Skip "nvcc.exe missing under $cudaRoot" }
if (-not (Test-Path -LiteralPath $cudart -PathType Leaf)) { Write-Skip "cudart.lib missing under $cudaRoot" }

# --- MSVC host toolchain (nvcc drives cl.exe for the host pass) --------------

# Some stripped CI/sandbox environments leave ProgramFiles(x86) unset, so fall
# back to the canonical location instead of failing on a null Join-Path.
$programFilesX86 = ${env:ProgramFiles(x86)}
if ([string]::IsNullOrWhiteSpace($programFilesX86)) { $programFilesX86 = "C:\Program Files (x86)" }

$vsWhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vsWhere -PathType Leaf)) { Write-Skip "vswhere.exe missing (no Visual Studio installer)" }

$vsPath = (& $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($vsPath)) { Write-Skip "no MSVC x64 toolset installed" }
$vsPath = $vsPath.Trim()

$msvcRoot = $null
$msvcTools = Join-Path $vsPath "VC\Tools\MSVC"
if (Test-Path -LiteralPath $msvcTools) {
    $msvcRoot = (Get-ChildItem -LiteralPath $msvcTools -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
}
if ([string]::IsNullOrWhiteSpace($msvcRoot)) { Write-Skip "MSVC tools directory not found" }
$clDir = Join-Path $msvcRoot "bin\Hostx64\x64"
if (-not (Test-Path -LiteralPath (Join-Path $clDir "cl.exe"))) { Write-Skip "cl.exe not found under $clDir" }

$sdkRoot = "C:\Program Files (x86)\Windows Kits\10"
$sdkVersion = $null
if (Test-Path -LiteralPath (Join-Path $sdkRoot "Include")) {
    $sdkVersion = (Get-ChildItem -LiteralPath (Join-Path $sdkRoot "Include") -Directory |
        Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1).Name
}
$sdkInclude = @()
$sdkLib = @()
if (-not [string]::IsNullOrWhiteSpace($sdkVersion)) {
    foreach ($part in @("ucrt", "shared", "um")) {
        $sdkInclude += (Join-Path $sdkRoot "Include\$sdkVersion\$part")
    }
    $sdkLib += (Join-Path $sdkRoot "Lib\$sdkVersion\ucrt\x64")
    $sdkLib += (Join-Path $sdkRoot "Lib\$sdkVersion\um\x64")
}

$previousPath = $env:Path
$env:Path = $clDir + ";" + (Join-Path $cudaRoot "bin") + ";" + $env:Path
$savedInclude = $env:INCLUDE
$savedLib = $env:LIB
$env:INCLUDE = ((@((Join-Path $msvcRoot "include")) + $sdkInclude) -join ";")
$env:LIB = ((@((Join-Path $msvcRoot "lib\x64")) + $sdkLib) -join ";")

New-Item -ItemType Directory -Force -Path $cache | Out-Null
New-Item -ItemType Directory -Force -Path $prebuilt | Out-Null

$obj = Join-Path $prebuilt "cuda_ops.obj"
$lib = Join-Path $prebuilt "cuda_ops.lib"

function Invoke-CudaStep {
    param([string[]]$ArgumentList, [string]$Name)
    $out = Join-Path $cache ($Name + ".out.log")
    $err = Join-Path $cache ($Name + ".err.log")
    & $nvcc @ArgumentList > $out 2> $err
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("FAILED: " + $Name)
        if (Test-Path $out) { Get-Content $out -Tail 80 }
        if (Test-Path $err) { Get-Content $err -Tail 80 }
        exit 1
    }
}

Push-Location $projectRoot
try {
    Invoke-CudaStep -ArgumentList @("-std=c++17", "-c", $cu, "-o", $obj) -Name "nvcc_obj"
    Invoke-CudaStep -ArgumentList @("--lib", "-o", $lib, $obj) -Name "nvcc_lib"
} finally {
    Pop-Location
}
Copy-Item -LiteralPath $cudart -Destination $prebuilt -Force

if (-not (Test-Path -LiteralPath $lib -PathType Leaf)) { Fail-Test "nvcc did not produce $lib" }

# --- Vyx build -------------------------------------------------------------

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
    $env:Path = $RuntimeDir + ";" + $env:Path
}
if (-not (Test-Path -LiteralPath $contract -PathType Leaf)) { Fail-Test "contract $contract is missing" }
# NOTE: `*.dcib` is covered by a repo-wide .gitignore rule, so the contract has
# to be force-added (`git add -f contracts/CudaOps.dcib`) - the build itself is
# deliberately forbidden from regenerating it (VYX_DCI_PYTHON below).

Push-Location $projectRoot
try {
    # The checked-in contract must be enough: no Python adapter is allowed to
    # run during the build (same gate dci_complex_abi uses).
    $previousDciPython = $env:VYX_DCI_PYTHON
    $env:VYX_DCI_PYTHON = "__dci_python_must_not_run__"
    try {
        $buildOut = Join-Path $cache "project_build.out.log"
        $buildErr = Join-Path $cache "project_build.err.log"
        & $BootstrapCompiler build -j 10 > $buildOut 2> $buildErr
        $buildRc = $LASTEXITCODE
        if ($buildRc -ne 0) {
            Write-Host ("FAILED: project build (exit " + $buildRc + ")")
            if (Test-Path $buildOut) { Get-Content $buildOut -Tail 100 }
            if (Test-Path $buildErr) { Get-Content $buildErr -Tail 100 }
            exit $buildRc
        }
    } finally {
        if ($null -eq $previousDciPython) { Remove-Item Env:VYX_DCI_PYTHON -ErrorAction SilentlyContinue }
        else { $env:VYX_DCI_PYTHON = $previousDciPython }
    }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Fail-Test "project build did not produce target\dci_cuda.exe" }

# --- run ------------------------------------------------------------------

$runOut = Join-Path $cache "run_exe.out.log"
$runErr = Join-Path $cache "run_exe.err.log"
& $exe > $runOut 2> $runErr
$runRc = $LASTEXITCODE
if ($runRc -ne 0) {
    Write-Host ("FAILED: run_exe (exit " + $runRc + ")")
    if (Test-Path $runOut) { Get-Content $runOut -Tail 40 }
    if (Test-Path $runErr) { Get-Content $runErr -Tail 40 }
    exit 1
}
$actual = ((Get-Content $runOut) -join "`n").Trim()
if ($actual -ne "dci_cuda OK") { Fail-Test "unexpected executable output: $actual" }

# --- evidence the provider really went through the CUDA toolchain ----------

$dumpbin = $null
$dumpbinCommand = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($null -ne $dumpbinCommand) {
    $dumpbin = $dumpbinCommand.Source
} else {
    $dumpbinCandidate = Join-Path $clDir "dumpbin.exe"
    if (Test-Path -LiteralPath $dumpbinCandidate -PathType Leaf) { $dumpbin = $dumpbinCandidate }
}

if ($null -ne $dumpbin) {
    $symbolDump = Join-Path $cache "cuda_ops_symbols.txt"
    & $dumpbin /symbols $lib > $symbolDump 2> $null
    $symbolText = Get-Content -Raw -LiteralPath $symbolDump
    if ($symbolText.IndexOf("fill_and_sum@CudaVector", [StringComparison]::Ordinal) -lt 0) {
        Fail-Test "nvcc-built library does not export CudaVector::fill_and_sum"
    }

    # nvcc embeds the device code as a fatbin; the Vyx build's linker pass must
    # carry those sections into the executable or the kernels cannot launch.
    $headerDump = Join-Path $cache "exe_headers.txt"
    & $dumpbin /headers $exe > $headerDump 2> $null
    $headerText = Get-Content -Raw -LiteralPath $headerDump
    if (($headerText.IndexOf("nv_fatb", [StringComparison]::Ordinal) -lt 0) -and
        ($headerText.IndexOf("nvFatBi", [StringComparison]::Ordinal) -lt 0)) {
        Fail-Test "the linked executable carries no CUDA fatbin section"
    }
}

Write-Host "dci_cuda: OK"
exit 0

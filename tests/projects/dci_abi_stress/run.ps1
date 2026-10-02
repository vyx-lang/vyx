[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [string]$LlvmRoot = "",
    [int]$Jobs = 1,
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = "Stop"

$fixtureRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $fixtureRoot "..\..\..")).Path
. (Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1")
$windowsHost = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) { [bool]$IsWindows } else { $env:OS -eq "Windows_NT" }
if (-not $windowsHost) {
    Write-Host "dci_abi_stress: SKIP (fixture descriptor exercises x86_64 MSVC C++ ABI)"
    exit 77
}
$compilerName = if ($windowsHost) { "boot.exe" } else { "boot" }
$runtimeName = if ($windowsHost) {
    "vyx_compiler_backend.dll"
} elseif ($IsMacOS) {
    "libvyx_compiler_backend.dylib"
} else {
    "libvyx_compiler_backend.so"
}
$exeName = if ($windowsHost) { "dci_abi_stress.exe" } else { "dci_abi_stress" }

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = Split-Path -Parent $BootstrapCompiler
}
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$runtimePath = Join-Path $RuntimeDir $runtimeName
if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
    throw "compiler backend paired with selected compiler is missing: $runtimePath"
}

function Get-Sha256Hex([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}
if ([string]::IsNullOrWhiteSpace($LlvmRoot) -and $windowsHost) {
    $LlvmRoot = Join-Path $repoRoot "clang"
}

$runRoot = Join-Path $repoRoot ("tests/.cache/dci_abi_stress_run_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][System.IO.Directory]::CreateDirectory($runRoot)
Copy-Item -LiteralPath (Join-Path $fixtureRoot "Vyx.toml") -Destination $runRoot
foreach ($dir in @("contracts", "native", "src")) {
    Copy-Item -LiteralPath (Join-Path $fixtureRoot $dir) -Destination $runRoot -Recurse
}

$buildOut = Join-Path $runRoot "build.stdout.log"
$buildErr = Join-Path $runRoot "build.stderr.log"
$runOut = Join-Path $runRoot "run.stdout.log"
$runErr = Join-Path $runRoot "run.stderr.log"
$originalLlvmRoot = $env:LLVM_ROOT
$originalPath = $env:Path
try {
    if (-not [string]::IsNullOrWhiteSpace($LlvmRoot)) {
        $env:LLVM_ROOT = (Resolve-Path -LiteralPath $LlvmRoot).Path
    }
    $pathSeparator = if ($windowsHost) { ";" } else { ":" }
    $env:Path = $RuntimeDir + $pathSeparator + $originalPath

    $buildArgs = @{
        FilePath = $BootstrapCompiler
        ArgumentList = @("build", "-j$Jobs")
        WorkingDirectory = $runRoot
        StdoutLog = $buildOut
        StderrLog = $buildErr
        DialogLog = (Join-Path $runRoot "build.dialog.log")
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $buildArgs.MemoryLimitMB = 8192 }
    $build = Invoke-VyxProcess @buildArgs
    if ($build.ExitCode -ne 0 -or $build.TimedOut -or $build.DialogCaught -or $build.MemoryExceeded) {
        throw "dci_abi_stress build failed: exit=$($build.ExitCode) timeout=$($build.TimedOut) memory=$($build.MemoryExceeded)"
    }

    $exe = Join-Path $runRoot ("target/" + $exeName)
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "dci_abi_stress executable is missing: $exe"
    }
    $runArgs = @{
        FilePath = $exe
        WorkingDirectory = $runRoot
        StdoutLog = $runOut
        StderrLog = $runErr
        DialogLog = (Join-Path $runRoot "run.dialog.log")
        TimeoutSec = 60
    }
    if ($windowsHost) { $runArgs.MemoryLimitMB = 1024 }
    $run = Invoke-VyxProcess @runArgs
    $actual = if (Test-Path -LiteralPath $runOut) { [IO.File]::ReadAllText($runOut).Trim() } else { "" }
    if ($run.ExitCode -ne 0 -or $actual -cne "dci_abi_stress OK") {
        throw "dci_abi_stress execution failed: exit=$($run.ExitCode) output=$actual"
    }

    $evidence = [ordered]@{
        compiler = [ordered]@{
            path = $BootstrapCompiler
            sha256 = Get-Sha256Hex $BootstrapCompiler
        }
        runtime = [ordered]@{
            path = $runtimePath
            sha256 = Get-Sha256Hex $runtimePath
        }
        build_command = "$BootstrapCompiler build -j$Jobs"
        build_exit_code = [int]$build.ExitCode
        build_peak_working_set_mb = $build.PeakWorkingSetMB
        run_command = $exe
        run_exit_code = [int]$run.ExitCode
        run_output = $actual
    }
    $evidence | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot "evidence.json") -Encoding utf8
    Write-Host "dci_abi_stress: OK"
    Write-Host "dci_abi_stress evidence=$runRoot"
} finally {
    $env:LLVM_ROOT = $originalLlvmRoot
    $env:Path = $originalPath
}

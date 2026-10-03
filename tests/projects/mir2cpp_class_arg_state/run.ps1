[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [string]$ClangCxx = "",
    [int]$Jobs = 10,
    [int]$TimeoutSec = 600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# MIR2CPP / `--emit=cpp` is an experimental backend that is not a release or
# fixed-point acceptance gate (docs/TESTING_GUIDE.md, "Backend policy"), and
# this case gates that backend's class-argument lowering only. It is kept for
# manual runs of MIR2CPP and self-skips in the project sweep; set
# VYX_ACCEPT_MIR2CPP=1 to force it. Exit 77 is the sweep's "skipped" sentinel.
if ($env:VYX_ACCEPT_MIR2CPP -ne "1") {
    Write-Host "mir2cpp_class_arg_state: skipped (experimental MIR2CPP backend; set VYX_ACCEPT_MIR2CPP=1 to run)"
    exit 77
}

if ($Jobs -lt 1) { throw "Jobs must be at least 1." }
if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) {
    throw "TimeoutSec must be between 1 and 600."
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "..\..\..")).Path
$processHelper = Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1"
if (-not (Test-Path -LiteralPath $processHelper -PathType Leaf)) {
    throw "Process helper not found: $processHelper"
}
. $processHelper

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
if (-not (Test-Path -LiteralPath $BootstrapCompiler -PathType Leaf)) {
    throw "Bootstrap compiler not found: $BootstrapCompiler"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($ClangCxx)) {
    $clangName = if ($env:OS -eq "Windows_NT") { "clang++.exe" } else { "clang++" }
    $bundledClang = Join-Path $repoRoot ("clang\bin\" + $clangName)
    if (Test-Path -LiteralPath $bundledClang -PathType Leaf) {
        $ClangCxx = $bundledClang
    } else {
        $ClangCxx = (Get-Command $clangName -ErrorAction Stop).Source
    }
}
if (-not (Test-Path -LiteralPath $ClangCxx -PathType Leaf)) {
    throw "Clang C++ compiler not found: $ClangCxx"
}
$ClangCxx = (Resolve-Path -LiteralPath $ClangCxx).Path

$cmakeExe = (Get-Command cmake -ErrorAction Stop).Source
$ninjaExe = (Get-Command ninja -ErrorAction Stop).Source
$runId = "run_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks
$runRoot = Join-Path $projectRoot (".cache\" + $runId)
$logsRoot = Join-Path $runRoot "logs"
$cppOut = Join-Path $runRoot "cpp"
New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null

function Invoke-CheckedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    $stdoutLog = Join-Path $logsRoot ($Name + ".stdout.log")
    $stderrLog = Join-Path $logsRoot ($Name + ".stderr.log")
    $dialogLog = Join-Path $logsRoot ($Name + ".dialog.log")
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $processOutput = @(Invoke-VyxProcess -FilePath $FilePath `
            -ArgumentList $ArgumentList `
            -WorkingDirectory $WorkingDirectory `
            -StdoutLog $stdoutLog `
            -StderrLog $stderrLog `
            -DialogLog $dialogLog `
            -TimeoutSec $TimeoutSec)
    } finally {
        $watch.Stop()
    }
    if ($processOutput.Count -eq 0) {
        throw "Step '$Name' returned no process result. Logs: $logsRoot"
    }
    $result = $processOutput[-1]

    if ($result.TimedOut) {
        throw "Step '$Name' timed out after $TimeoutSec seconds. Logs: $logsRoot"
    }
    if ($result.ExitCode -ne 0) {
        $stdout = if (Test-Path -LiteralPath $stdoutLog) { Get-Content -Raw -LiteralPath $stdoutLog } else { "" }
        $stderr = if (Test-Path -LiteralPath $stderrLog) { Get-Content -Raw -LiteralPath $stderrLog } else { "" }
        throw "Step '$Name' failed with exit code $($result.ExitCode).`nstdout:`n$stdout`nstderr:`n$stderr"
    }
    Write-Host ("[{0}] exit=0 elapsed_ms={1}" -f $Name, [Math]::Round($watch.Elapsed.TotalMilliseconds, 3))
    return [pscustomobject]@{
        Result = $result
        StdoutLog = $stdoutLog
        StderrLog = $stderrLog
    }
}

[void](Invoke-CheckedProcess -Name "emit_cpp" -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=project", $projectRoot, "--emit=cpp", "-j", "$Jobs", "-o", $cppOut) `
    -WorkingDirectory $projectRoot)

[void](Invoke-CheckedProcess -Name "cmake_configure" -FilePath $cmakeExe `
    -ArgumentList @("--preset", "ninja-release",
                    "-DCMAKE_CXX_COMPILER:FILEPATH=$ClangCxx",
                    "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninjaExe") `
    -WorkingDirectory $cppOut)

[void](Invoke-CheckedProcess -Name "cmake_build" -FilePath $cmakeExe `
    -ArgumentList @("--build", "--preset", "ninja-release", "--parallel", "$Jobs") `
    -WorkingDirectory $cppOut)

$exeName = if ($env:OS -eq "Windows_NT") {
    "vyx_mir2cpp_class_arg_state.exe"
} else {
    "vyx_mir2cpp_class_arg_state"
}
$exePath = Join-Path $cppOut ("build\ninja-release\" + $exeName)
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Generated executable not found: $exePath"
}

$run = Invoke-CheckedProcess -Name "run_generated" -FilePath $exePath `
    -WorkingDirectory $cppOut
$actual = (Get-Content -Raw -LiteralPath $run.StdoutLog).Trim()
$expected = "mir2cpp_class_arg_state OK"
if ($actual -ne $expected) {
    throw "Generated executable output mismatch. Expected '$expected', got '$actual'."
}

Write-Host "mir2cpp_class_arg_state: OK"
Write-Host "artifacts: $runRoot"

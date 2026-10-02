[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [int]$Jobs = 10,
    [int]$TimeoutSec = 600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "mir2cpp_raw_callback: SKIP (POSIX full-runtime declarations are not stable in MIR2CPP)"
    exit 77
}

if ($Jobs -lt 1) { throw "Jobs must be at least 1." }
if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) { throw "TimeoutSec must be between 1 and 600." }

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "..\..\..")).Path
. (Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1")
$isWindowsPlatform = $env:OS -eq "Windows_NT"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = Split-Path -Parent $BootstrapCompiler
}
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path

$clangName = if ($isWindowsPlatform) { "clang++.exe" } else { "clang++" }
$bundledClang = Join-Path $repoRoot ("clang\bin\" + $clangName)
$clangCxx = if (Test-Path -LiteralPath $bundledClang -PathType Leaf) {
    (Resolve-Path -LiteralPath $bundledClang).Path
} else {
    (Get-Command $clangName -ErrorAction Stop).Source
}
$cmakeExe = (Get-Command cmake -ErrorAction Stop).Source
$ninjaExe = (Get-Command ninja -ErrorAction Stop).Source
$runRoot = Join-Path $projectRoot (".cache\run_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
$logsRoot = Join-Path $runRoot "logs"
$cppOut = Join-Path $runRoot "cpp"
New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null

function Invoke-TestProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [int]$ExpectedExit = 0,
        [string]$ExpectedText = ""
    )

    $stdoutLog = Join-Path $logsRoot ($Name + ".stdout.log")
    $stderrLog = Join-Path $logsRoot ($Name + ".stderr.log")
    $dialogLog = Join-Path $logsRoot ($Name + ".dialog.log")
    $resultRows = @(Invoke-VyxProcess -FilePath $FilePath `
        -ArgumentList $ArgumentList `
        -WorkingDirectory $WorkingDirectory `
        -StdoutLog $stdoutLog `
        -StderrLog $stderrLog `
        -DialogLog $dialogLog `
        -TimeoutSec $TimeoutSec)
    if ($resultRows.Count -eq 0) { throw "$Name returned no process result" }
    $result = $resultRows[-1]
    $stdout = if (Test-Path -LiteralPath $stdoutLog) { Get-Content -Raw -LiteralPath $stdoutLog } else { "" }
    $stderr = if (Test-Path -LiteralPath $stderrLog) { Get-Content -Raw -LiteralPath $stderrLog } else { "" }
    $combined = $stdout + "`n" + $stderr
    if ($result.TimedOut) { throw "$Name timed out after $TimeoutSec seconds" }
    if ($result.ExitCode -ne $ExpectedExit) {
        throw "$Name expected exit $ExpectedExit, got $($result.ExitCode).`n$combined"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedText) -and
        $combined.IndexOf($ExpectedText, [StringComparison]::Ordinal) -lt 0) {
        throw "$Name did not emit '$ExpectedText'.`n$combined"
    }
    Write-Host "[$Name] exit=$ExpectedExit"
    return [pscustomobject]@{ Result = $result; Stdout = $stdout; Stderr = $stderr }
}

$runtimeArgs = @("-L", $RuntimeDir, "-l", "vyx_runtime")
$mainSource = Join-Path $projectRoot "src\main.vyx"
$nativeExe = Join-Path $runRoot $(if ($isWindowsPlatform) { "native.exe" } else { "native" })
$native = Invoke-TestProcess -Name "native_llvm" -FilePath $BootstrapCompiler `
    -ArgumentList (@("--src=file", $mainSource, "--run=aot", "-o", $nativeExe) + $runtimeArgs) `
    -WorkingDirectory $projectRoot -ExpectedText "mir2cpp_raw_callback OK"

[void](Invoke-TestProcess -Name "emit_cpp" -FilePath $BootstrapCompiler `
    -ArgumentList (@("--src=project", $projectRoot, "--emit=cpp", "-j", "$Jobs", "-o", $cppOut) + $runtimeArgs) `
    -WorkingDirectory $projectRoot)
[void](Invoke-TestProcess -Name "cmake_configure" -FilePath $cmakeExe `
    -ArgumentList @("--preset", "ninja-release",
                    "-DCMAKE_CXX_COMPILER:FILEPATH=$clangCxx",
                    "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninjaExe") `
    -WorkingDirectory $cppOut)
[void](Invoke-TestProcess -Name "cmake_build" -FilePath $cmakeExe `
    -ArgumentList @("--build", "--preset", "ninja-release", "--parallel", "$Jobs") `
    -WorkingDirectory $cppOut)

$generatedName = if ($isWindowsPlatform) { "vyx_mir2cpp_raw_callback.exe" } else { "vyx_mir2cpp_raw_callback" }
$generatedExe = Join-Path $cppOut ("build\ninja-release\" + $generatedName)
if (-not (Test-Path -LiteralPath $generatedExe -PathType Leaf)) {
    throw "Generated executable not found: $generatedExe"
}
$oldPath = $env:Path
try {
    $env:Path = $RuntimeDir + [System.IO.Path]::PathSeparator + $env:Path
    [void](Invoke-TestProcess -Name "run_generated" -FilePath $generatedExe `
        -WorkingDirectory $cppOut -ExpectedText "mir2cpp_raw_callback OK")
} finally {
    $env:Path = $oldPath
}

foreach ($negative in @(
    [pscustomobject]@{
        Name = "capturing";
        Source = (Join-Path $projectRoot "src\capturing_negative.vyx");
        Diagnostic = "cannot expose capturing function"
    },
    [pscustomobject]@{
        Name = "arity";
        Source = (Join-Path $projectRoot "src\arity_negative.vyx");
        Diagnostic = "cannot expose bad_callback as raw callback"
    }
)) {
    [void](Invoke-TestProcess -Name ($negative.Name + "_llvm") -FilePath $BootstrapCompiler `
        -ArgumentList (@("--src=file", $negative.Source, "--emit=ir", "-o",
                         (Join-Path $runRoot ($negative.Name + ".ll"))) + $runtimeArgs) `
        -WorkingDirectory $projectRoot -ExpectedExit 1 -ExpectedText $negative.Diagnostic)
    [void](Invoke-TestProcess -Name ($negative.Name + "_cpp") -FilePath $BootstrapCompiler `
        -ArgumentList (@("--src=file", $negative.Source, "--emit=cpp", "-o",
                         (Join-Path $runRoot ($negative.Name + "_cpp"))) + $runtimeArgs) `
        -WorkingDirectory $projectRoot -ExpectedExit 1 -ExpectedText $negative.Diagnostic)
}

Write-Host "mir2cpp_raw_callback: OK"
exit 0

param(
    [string]$Compiler = ""
)

$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$testsRoot = (Resolve-Path (Join-Path $here "..\..")).Path
$repoRoot = (Resolve-Path (Join-Path $testsRoot "..")).Path
$bootstrapRoot = Join-Path $repoRoot "bootstrap_compiler"
. (Join-Path $bootstrapRoot "scripts\VyxTestProcess.ps1")

$isWindowsHost = ([System.Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
if ([string]::IsNullOrWhiteSpace($Compiler)) {
    $compilerName = $(if ($isWindowsHost) { "boot.exe" } else { "boot" })
    $Compiler = Join-Path (Join-Path $bootstrapRoot "out") $compilerName
}
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
    throw "Compiler not found: $Compiler"
}
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path

$runRoot = Join-Path $repoRoot ("tests\.cache\module_registry_failure_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $runRoot | Out-Null

# The driver writes its module scan list below .cache. Occupying that path
# with a regular file makes the real runtime helper return a write failure.
[System.IO.File]::WriteAllText((Join-Path $runRoot ".cache"), "not a directory")

$stdoutLog = Join-Path $runRoot "stdout.log"
$stderrLog = Join-Path $runRoot "stderr.log"
$invokeArgs = @{
    FilePath = $Compiler
    ArgumentList = @("--stop-after-sema", "--src=file", (Join-Path $here "valid.vyx"))
    WorkingDirectory = $runRoot
    StdoutLog = $stdoutLog
    StderrLog = $stderrLog
    DialogLog = (Join-Path $runRoot "dialog.log")
    TimeoutSec = 10
}
$run = Invoke-VyxProcess @invokeArgs
if ($run.TimedOut -or $run.DialogCaught) {
    throw "module registry failure did not terminate cleanly: exit=$($run.ExitCode) timeout=$($run.TimedOut) dialog=$($run.DialogCaught)"
}
if ($run.ExitCode -ne 1) {
    throw "module registry helper failure returned $($run.ExitCode), expected exit 1"
}

$diagnostics = [System.IO.File]::ReadAllText($stdoutLog) + [System.IO.File]::ReadAllText($stderrLog)
$expected = "failed to scan module registry root"
if ($diagnostics.IndexOf($expected, [StringComparison]::Ordinal) -lt 0) {
    throw "module registry helper failure did not emit expected diagnostic: $expected"
}

Write-Host "module registry failure propagation PASS (exit 1)"

param(
    [string]$Compiler = ""
)

$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$testsRoot = (Resolve-Path (Join-Path $here "..\..")).Path
$repoRoot = (Resolve-Path (Join-Path $testsRoot "..")).Path
$bootstrapRoot = Join-Path $repoRoot "bootstrap_compiler"
. (Join-Path $bootstrapRoot "scripts\VyxTestProcess.ps1")

if ([string]::IsNullOrWhiteSpace($Compiler)) {
    $Compiler = Join-Path $bootstrapRoot "out\boot.exe"
}
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
    throw "Compiler not found: $Compiler"
}
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path

$logDir = Join-Path $repoRoot "tests\.cache\parser_recovery"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$cases = @(
    [pscustomobject]@{
        File = "reserved_var_name.vyx"
        Diagnostics = [string[]]@(
            "expected identifier after 'var', got ``match``",
            "expected identifier after 'let', got ``for``",
            "expected identifier after 'var', got ``while``"
        )
        ForbiddenDiagnostics = [string[]]@()
    },
    [pscustomobject]@{
        File = "match_missing_lbrace.vyx"
        Diagnostics = [string[]]@(
            "expected '{', got ``return``"
        )
        ForbiddenDiagnostics = [string[]]@()
    },
    [pscustomobject]@{
        File = "malformed_match_arm.vyx"
        Diagnostics = [string[]]@(
            "expected primary expression, got ``rawptr``"
        )
        ForbiddenDiagnostics = [string[]]@()
    }
    [pscustomobject]@{
        File = "nested_scope_boundary.vyx"
        Diagnostics = [string[]]@(
            "expected field or method declaration, got ``+``"
        )
        ForbiddenDiagnostics = [string[]]@(
            "expected '}' to close class body"
        )
    }
)

foreach ($case in $cases) {
    $source = Join-Path $here $case.File
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($case.File)
    $stdoutLog = Join-Path $logDir ($stem + ".stdout.log")
    $stderrLog = Join-Path $logDir ($stem + ".stderr.log")
    $invokeArgs = @{
        FilePath = $Compiler
        ArgumentList = @("--stop-after-parse", "--src=file", $source)
        WorkingDirectory = $repoRoot
        StdoutLog = $stdoutLog
        StderrLog = $stderrLog
        DialogLog = (Join-Path $logDir ($stem + ".dialog.log"))
        TimeoutSec = 5
    }
    $isWindowsHost = ([System.Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
    if ($isWindowsHost) {
        $invokeArgs.MemoryLimitMB = 256
    }
    $run = Invoke-VyxProcess @invokeArgs

    $memoryExceeded = $false
    $peakWorkingSetMB = 0.0
    if ($null -ne $run.PSObject.Properties["MemoryExceeded"]) {
        $memoryExceeded = [bool]$run.MemoryExceeded
    }
    if ($null -ne $run.PSObject.Properties["PeakWorkingSetMB"]) {
        $peakWorkingSetMB = [double]$run.PeakWorkingSetMB
    }

    if ($run.TimedOut -or $memoryExceeded -or $run.DialogCaught) {
        throw "$($case.File) did not fail cleanly: exit=$($run.ExitCode) timeout=$($run.TimedOut) memory=$memoryExceeded dialog=$($run.DialogCaught)"
    }
    if ($run.ExitCode -ne 1) {
        throw "$($case.File) returned $($run.ExitCode), expected parser failure exit 1"
    }
    $diagnostics = [System.IO.File]::ReadAllText($stdoutLog) + [System.IO.File]::ReadAllText($stderrLog)
    foreach ($expected in @($case.Diagnostics)) {
        if ($diagnostics.IndexOf($expected, [StringComparison]::Ordinal) -lt 0) {
            throw "$($case.File) did not emit expected diagnostic: $expected"
        }
    }
    foreach ($forbidden in @($case.ForbiddenDiagnostics)) {
        if ($diagnostics.IndexOf($forbidden, [StringComparison]::Ordinal) -ge 0) {
            throw "$($case.File) crossed its enclosing recovery boundary: $forbidden"
        }
    }
    if ($diagnostics.Length -gt 65536) {
        throw "$($case.File) emitted an unexpectedly large diagnostic stream: $($diagnostics.Length) bytes"
    }
    Write-Host "PASS $($case.File) (clean parser failure, peak $peakWorkingSetMB MB)"
}

Write-Host "parser recovery regression PASS"

param()

$ErrorActionPreference = "Stop"

$isWindowsPlatform = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    [bool]$IsWindows
} else {
    $env:OS -eq "Windows_NT"
}
if (-not $isWindowsPlatform) {
    Write-Host "vyx_test_process_memory_job SKIP (Windows only)"
    exit 0
}

. (Join-Path $PSScriptRoot "VyxTestProcess.ps1")

$powerShellPath = (Get-Process -Id $PID).Path
$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vyx-test-process-job-" + [Guid]::NewGuid().ToString("N"))
[void][System.IO.Directory]::CreateDirectory($workRoot)
$passed = $false

function Read-TestLog {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return "" }
    return [System.IO.File]::ReadAllText($Path)
}

try {
    $exitScript = Join-Path $workRoot "exit_and_log.ps1"
    @'
[Console]::Out.WriteLine("suspended launch stdout")
[Console]::Error.WriteLine("suspended launch stderr")
$stdin = [Console]::In.ReadToEnd()
[Console]::Out.WriteLine("stdin=" + $stdin)
exit 7
'@ | Set-Content -LiteralPath $exitScript -Encoding ascii

    $positive = Invoke-VyxProcess `
        -FilePath $powerShellPath `
        -ArgumentList @("-NoLogo", "-NoProfile", "-File", $exitScript) `
        -WorkingDirectory $workRoot `
        -StdoutLog (Join-Path $workRoot "positive.stdout.log") `
        -StderrLog (Join-Path $workRoot "positive.stderr.log") `
        -DialogLog (Join-Path $workRoot "positive.dialog.log") `
        -StdinText "suspended launch stdin" `
        -TimeoutSec 10 `
        -MemoryLimitMB 512

    if ($positive.ExitCode -ne 7 -or $positive.TimedOut -or $positive.DialogCaught -or $positive.MemoryExceeded) {
        throw "positive launch semantics failed: exit=$($positive.ExitCode) timeout=$($positive.TimedOut) dialog=$($positive.DialogCaught) memory=$($positive.MemoryExceeded)"
    }
    if (-not $positive.JobLimited) {
        throw "positive launch was not assigned to the memory Job"
    }
    if (-not (Read-TestLog $positive.StdoutLog).Contains("suspended launch stdout")) {
        throw "positive stdout redirection was lost"
    }
    if (-not (Read-TestLog $positive.StderrLog).Contains("suspended launch stderr")) {
        throw "positive stderr redirection was lost"
    }
    if (-not (Read-TestLog $positive.StdoutLog).Contains("stdin=suspended launch stdin")) {
        throw "suspended stdin redirection was lost"
    }

    $childScript = Join-Path $workRoot "memory_child.ps1"
    @'
param([string]$Marker)

Add-Type -TypeDefinition @"
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

public static class JobMembershipProbe {
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);

    public static bool CurrentProcessIsInJob() {
        bool result;
        if (!IsProcessInJob(Process.GetCurrentProcess().Handle, IntPtr.Zero, out result)) {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        return result;
    }
}
"@

[System.IO.File]::WriteAllText($Marker, "in_job=" + [JobMembershipProbe]::CurrentProcessIsInJob())
$blocks = [System.Collections.ArrayList]::new()
$allocatedMB = 0
while ($allocatedMB -lt 384) {
    $block = [byte[]]::new(1024 * 1024)
    for ($offset = 0; $offset -lt $block.Length; $offset += 4096) {
        $block[$offset] = 1
    }
    [void]$blocks.Add($block)
    $allocatedMB += 1
}
[System.IO.File]::WriteAllText($Marker, "escaped_limit allocated_mb=" + $allocatedMB)
exit 0
'@ | Set-Content -LiteralPath $childScript -Encoding ascii

    $rootScript = Join-Path $workRoot "spawn_memory_child.ps1"
    @'
param(
    [string]$ChildScript,
    [string]$Marker
)

$PowerShellPath = (Get-Process -Id $PID).Path
& $PowerShellPath -NoLogo -NoProfile -File $ChildScript $Marker
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $rootScript -Encoding ascii

    $marker = Join-Path $workRoot "memory_child.marker"
    $negative = Invoke-VyxProcess `
        -FilePath $powerShellPath `
        -ArgumentList @("-NoLogo", "-NoProfile", "-File", $rootScript, $childScript, $marker) `
        -WorkingDirectory $workRoot `
        -StdoutLog (Join-Path $workRoot "negative.stdout.log") `
        -StderrLog (Join-Path $workRoot "negative.stderr.log") `
        -DialogLog (Join-Path $workRoot "negative.dialog.log") `
        -TimeoutSec 15 `
        -MemoryLimitMB 256

    if (-not $negative.JobLimited) {
        throw "negative launch was not assigned to the memory Job"
    }
    if (-not (Test-Path -LiteralPath $marker)) {
        throw "memory child did not start"
    }
    $markerText = [System.IO.File]::ReadAllText($marker)
    if (-not $markerText.Contains("in_job=True")) {
        throw "memory child did not inherit a Job: $markerText"
    }
    if ($markerText.Contains("escaped_limit")) {
        throw "memory child escaped the 256 MB Job limit"
    }
    if (-not $negative.MemoryExceeded -or $negative.ExitCode -ne -905) {
        throw "memory tree limit was not reported: exit=$($negative.ExitCode) memory=$($negative.MemoryExceeded) peak_mb=$($negative.PeakWorkingSetMB)"
    }

    $passed = $true
    Write-Host "vyx_test_process_memory_job PASS peak_mb=$($negative.PeakWorkingSetMB)"
} finally {
    if ($passed -and (Test-Path -LiteralPath $workRoot)) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    } elseif (Test-Path -LiteralPath $workRoot) {
        Write-Host "vyx_test_process_memory_job retained_work_root=$workRoot"
    }
}

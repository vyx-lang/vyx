param(
    [string]$BootstrapCompiler = "",
    [string]$LlvmRoot = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $compilerName = if ($env:OS -eq "Windows_NT") { "boot.exe" } else { "boot" }
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path

$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vyx-build-lock-" + [Guid]::NewGuid().ToString("N"))
[void][System.IO.Directory]::CreateDirectory($workRoot)
$passed = $false
$originalLlvmRoot = $env:LLVM_ROOT
$runs = New-Object System.Collections.Generic.List[object]

if ([string]::IsNullOrWhiteSpace($LlvmRoot) -and $env:OS -eq "Windows_NT") {
    $LlvmRoot = Join-Path $repoRoot "clang"
}

function Start-FixtureBuild {
    param([Parameter(Mandatory=$true)][string]$Name)

    $stdout = Join-Path $workRoot ($Name + ".stdout.log")
    $stderr = Join-Path $workRoot ($Name + ".stderr.log")
    $process = Start-Process `
        -FilePath $BootstrapCompiler `
        -ArgumentList @("build", "-j2") `
        -WorkingDirectory $workRoot `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -PassThru
    $run = [pscustomobject]@{
        Process = $process
        Stdout = $stdout
        Stderr = $stderr
    }
    [void]$runs.Add($run)
    return $run
}

function Wait-FixtureBuild {
    param(
        [Parameter(Mandatory=$true)]$Run,
        [int]$TimeoutSec = 60
    )

    if (-not $Run.Process.WaitForExit($TimeoutSec * 1000)) {
        Stop-FixtureProcessTree -Process $Run.Process
        throw "fixture build timed out: $($Run.Process.Id)"
    }
    $Run.Process.WaitForExit()
    return [int]$Run.Process.ExitCode
}

function Stop-FixtureProcessTree {
    param([Parameter(Mandatory=$true)]$Process)

    if ($env:OS -eq "Windows_NT") {
        try {
            & taskkill.exe /PID $Process.Id /T /F *> $null
            return
        } catch {}
    }
    try { $Process.Kill($true) }
    catch { try { $Process.Kill() } catch {} }
}

function Read-RunLog {
    param([Parameter(Mandatory=$true)]$Run)

    $stdout = if (Test-Path -LiteralPath $Run.Stdout) { [IO.File]::ReadAllText($Run.Stdout) } else { "" }
    $stderr = if (Test-Path -LiteralPath $Run.Stderr) { [IO.File]::ReadAllText($Run.Stderr) } else { "" }
    return $stdout + "`n" + $stderr
}

try {
    Copy-Item -LiteralPath (Join-Path $projectRoot "Vyx.toml") -Destination $workRoot
    Copy-Item -LiteralPath (Join-Path $projectRoot "main.vyx") -Destination $workRoot
    Copy-Item -LiteralPath (Join-Path $projectRoot "hooks") -Destination $workRoot -Recurse
    if (-not [string]::IsNullOrWhiteSpace($LlvmRoot)) {
        $env:LLVM_ROOT = (Resolve-Path -LiteralPath $LlvmRoot).Path
    }

    $lockPath = Join-Path $workRoot ".cache/.vyx_build.lock"
    [void][System.IO.Directory]::CreateDirectory($lockPath)
    [IO.File]::WriteAllText((Join-Path $lockPath "owner.txt"), "pid=2147483647`nlegacy=dead-owner`n")

    $first = Start-FixtureBuild -Name "first"
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 50
        $tracePath = Join-Path $workRoot "lock_trace.txt"
        $trace = if (Test-Path -LiteralPath $tracePath) { @(Get-Content -LiteralPath $tracePath) } else { @() }
    } while (($trace -join ",") -cne "begin" -and [DateTime]::UtcNow -lt $deadline)
    if (($trace -join ",") -cne "begin") {
        throw "first build did not enter the locked prebuild hook"
    }

    $second = Start-FixtureBuild -Name "second"
    Start-Sleep -Seconds 2
    $trace = @(Get-Content -LiteralPath $tracePath)
    if (($trace -join ",") -cne "begin") {
        throw "second prebuild hook overlapped the first before release: trace=$($trace -join ',')"
    }
    [IO.File]::WriteAllText((Join-Path $workRoot "release_hook"), "release`n")

    $firstExit = Wait-FixtureBuild -Run $first
    $secondExit = Wait-FixtureBuild -Run $second
    if ($firstExit -ne 0 -or $secondExit -ne 0) {
        throw "concurrent serialized builds failed: first=$firstExit second=$secondExit"
    }
    $secondLog = Read-RunLog -Run $second
    if ($secondLog -notmatch "waiting for project lock" -or $secondLog -notmatch "acquired project lock") {
        throw "second build did not report waiting and subsequent lock acquisition"
    }
    $trace = @(Get-Content -LiteralPath (Join-Path $workRoot "lock_trace.txt"))
    if (($trace -join ",") -cne "begin,end,begin,end") {
        throw "project build critical sections overlapped: trace=$($trace -join ',')"
    }
    if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
        throw "legacy project lock directory was not migrated to the OS lock file"
    }
    $validSource = [IO.File]::ReadAllText((Join-Path $workRoot "main.vyx"))
    [IO.File]::WriteAllText((Join-Path $workRoot "main.vyx"), "fn main() -> i32 { var match: rawptr = null; return 0; }`n")
    $failed = Start-FixtureBuild -Name "failed"
    $failedExit = Wait-FixtureBuild -Run $failed
    $failedLog = Read-RunLog -Run $failed
    # Windows PowerShell 5 can expose a stale zero ExitCode for a redirected
    # Start-Process child when its own parent is cmd.exe. The compiler's exact
    # failure diagnostic still proves that this build did not hit the cache.
    $reportedCompileFailure = $failedLog -match 'compile failed: main\.vyx \(exit 1\)'
    $allowPs5StaleExit = $env:OS -eq "Windows_NT" -and
        $PSVersionTable.PSVersion.Major -le 5 -and $reportedCompileFailure
    if ($failedExit -eq 0 -and -not $allowPs5StaleExit) {
        throw "malformed source build unexpectedly succeeded"
    }
    [IO.File]::WriteAllText((Join-Path $workRoot "main.vyx"), $validSource)
    $recovered = Start-FixtureBuild -Name "recovered"
    $recoveredExit = Wait-FixtureBuild -Run $recovered
    if ($recoveredExit -ne 0) {
        throw "build did not recover after the failed build: exit=$recoveredExit"
    }
    $outputName = if ($env:OS -eq "Windows_NT") { "build_lock_concurrency.exe" } else { "build_lock_concurrency" }
    if (-not (Test-Path -LiteralPath (Join-Path $workRoot ("out/" + $outputName)) -PathType Leaf)) {
        throw "serialized build output is missing"
    }

    $passed = $true
    Write-Host "build_lock_concurrency PASS"
} finally {
    $env:LLVM_ROOT = $originalLlvmRoot
    foreach ($run in $runs) {
        try {
            if (-not $run.Process.HasExited) {
                Stop-FixtureProcessTree -Process $run.Process
                [void]$run.Process.WaitForExit(5000)
            }
        } catch {}
        try { $run.Process.Dispose() } catch {}
    }
    if ($passed) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "build_lock_concurrency retained_work_root=$workRoot"
    }
}

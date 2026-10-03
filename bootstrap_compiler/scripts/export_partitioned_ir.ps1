[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$Compiler = "",
    [string]$RecordsPath = "",
    [string]$InterfaceRoot = "",
    [string]$OutDir = "",
    [string]$Target = "x86_64-linux-gnu",
    [string]$LlvmLink = "",
    [int]$Jobs = 10,
    [int]$TimeoutSec = 600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Jobs -lt 1) { throw "Jobs must be at least 1." }
if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) {
    throw "TimeoutSec must be between 1 and 600."
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Join-Path $repoRoot "bootstrap_compiler"
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path

if ([string]::IsNullOrWhiteSpace($Compiler)) {
    $Compiler = Join-Path $ProjectRoot "out\boot.exe"
}
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path

if ([string]::IsNullOrWhiteSpace($RecordsPath)) {
    $RecordsPath = Join-Path $ProjectRoot ".cache\records_boot.tmp"
}
$RecordsPath = (Resolve-Path -LiteralPath $RecordsPath).Path

if ([string]::IsNullOrWhiteSpace($InterfaceRoot)) {
    $InterfaceRoot = Join-Path $ProjectRoot "out\.vyx_interfaces\boot"
}
$InterfaceRoot = (Resolve-Path -LiteralPath $InterfaceRoot).Path

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $safeTarget = $Target.Replace('-', '_')
    $OutDir = Join-Path $ProjectRoot ("out\cross\" + $safeTarget + "\llvm")
}
$OutDir = [IO.Path]::GetFullPath($OutDir)
$partsDir = Join-Path $OutDir "parts"
$logsDir = Join-Path $OutDir "logs"
New-Item -ItemType Directory -Force -Path $partsDir, $logsDir | Out-Null

if ([string]::IsNullOrWhiteSpace($LlvmLink)) {
    $LlvmLink = Join-Path $repoRoot "clang\bin\llvm-link.exe"
}
$LlvmLink = (Resolve-Path -LiteralPath $LlvmLink).Path

function Get-QuotedOption {
    param([string]$Command, [string]$Name)
    $pattern = '(?:^|\s)' + [regex]::Escape($Name) + '\s+"([^"]*)"'
    $match = [regex]::Match($Command, $pattern)
    if ($match.Success) { return $match.Groups[1].Value }
    return ""
}

function Quote-ProcessArg {
    param([string]$Value)
    if ($null -eq $Value -or $Value.Length -eq 0) { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + $Value.Replace('"', '\"') + '"'
}

$plans = New-Object System.Collections.ArrayList
foreach ($line in [IO.File]::ReadLines($RecordsPath)) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith("__vyi:")) { continue }
    $fields = $line.Split("`t")
    if ($fields.Count -lt 4) { continue }
    $source = $fields[0]
    if (-not $source.EndsWith(".vyx", [StringComparison]::OrdinalIgnoreCase)) { continue }

    $objectName = [IO.Path]::GetFileNameWithoutExtension($fields[1])
    $partPath = Join-Path $partsDir ($objectName + ".ll")
    $recordCommand = $fields[3]
    $args = New-Object System.Collections.Generic.List[string]
    foreach ($arg in @(
        "--src=file", $source,
        "--emit=ir",
        ("--target=" + $Target),
        "-O0",
        "--interface-root", $InterfaceRoot,
        "--export-top-level-roots",
        "--export-method-roots"
    )) {
        $args.Add($arg)
    }

    foreach ($name in @(
        "--export-root-names-file",
        "--emit-root-names-file",
        "--export-root-names",
        "--unit-sources"
    )) {
        $value = Get-QuotedOption -Command $recordCommand -Name $name
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            $args.Add($name)
            $args.Add($value)
        }
    }
    $args.Add("-o")
    $args.Add($partPath)
    [void]$plans.Add([pscustomobject]@{
        Name = $objectName
        Source = $source
        Output = $partPath
        Args = $args.ToArray()
    })
}

if ($plans.Count -eq 0) {
    throw "No Vyx partition records found in $RecordsPath"
}

Write-Host ("partitioned IR: {0} unit(s), target={1}, jobs={2}" -f $plans.Count, $Target, $Jobs)
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$next = 0
$completed = 0
$running = @()

try {
    while ($next -lt $plans.Count -or $running.Count -gt 0) {
        if ($stopwatch.Elapsed.TotalSeconds -gt $TimeoutSec) {
            throw "partitioned IR export timed out after $TimeoutSec seconds"
        }

        while ($next -lt $plans.Count -and $running.Count -lt $Jobs) {
            $plan = $plans[$next]
            $stdoutPath = Join-Path $logsDir ($plan.Name + ".stdout.log")
            $stderrPath = Join-Path $logsDir ($plan.Name + ".stderr.log")
            $psi = [Diagnostics.ProcessStartInfo]::new()
            $psi.FileName = $Compiler
            $psi.Arguments = ($plan.Args | ForEach-Object { Quote-ProcessArg $_ }) -join ' '
            $psi.WorkingDirectory = $ProjectRoot
            $psi.UseShellExecute = $false
            $psi.CreateNoWindow = $true
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError = $true
            $process = [Diagnostics.Process]::new()
            $process.StartInfo = $psi
            [void]$process.Start()
            $running += [pscustomobject]@{
                Plan = $plan
                Process = $process
                StdoutTask = $process.StandardOutput.ReadToEndAsync()
                StderrTask = $process.StandardError.ReadToEndAsync()
                StdoutPath = $stdoutPath
                StderrPath = $stderrPath
            }
            $next++
        }

        for ($i = $running.Count - 1; $i -ge 0; $i--) {
            $item = $running[$i]
            if (-not $item.Process.HasExited) { continue }
            $stdout = $item.StdoutTask.GetAwaiter().GetResult()
            $stderr = $item.StderrTask.GetAwaiter().GetResult()
            [IO.File]::WriteAllText($item.StdoutPath, $stdout)
            [IO.File]::WriteAllText($item.StderrPath, $stderr)
            $exitCode = $item.Process.ExitCode
            $item.Process.Dispose()
            $running = @($running | Where-Object { $_ -ne $item })
            if ($exitCode -ne 0) {
                throw ("IR partition failed: {0} (exit {1})`n{2}`n{3}" -f
                       $item.Plan.Name, $exitCode, $stdout, $stderr)
            }
            if (-not (Test-Path -LiteralPath $item.Plan.Output) -or
                (Get-Item -LiteralPath $item.Plan.Output).Length -eq 0) {
                throw "IR partition produced no output: $($item.Plan.Name)"
            }
            $completed++
            Write-Progress -Activity "Exporting LLVM IR partitions" `
                -Status "$completed / $($plans.Count)" `
                -PercentComplete ([int](100 * $completed / $plans.Count))
        }
        Start-Sleep -Milliseconds 25
    }
} catch {
    foreach ($item in $running) {
        try { if (-not $item.Process.HasExited) { $item.Process.Kill() } } catch {}
        try { $item.Process.Dispose() } catch {}
    }
    throw
} finally {
    Write-Progress -Activity "Exporting LLVM IR partitions" -Completed
}

$partPaths = @($plans | ForEach-Object { $_.Output })
$bootLl = Join-Path $OutDir "boot.ll"
& $LlvmLink @partPaths -S -o $bootLl
if ($LASTEXITCODE -ne 0) { throw "llvm-link failed with exit $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $bootLl) -or (Get-Item -LiteralPath $bootLl).Length -eq 0) {
    throw "llvm-link produced no boot.ll"
}

$nativeDir = Join-Path $OutDir "native"
New-Item -ItemType Directory -Force -Path $nativeDir | Out-Null
foreach ($name in @("cJSON.c", "cJSON.h")) {
    $source = Join-Path $repoRoot ("third_party\cjson\" + $name)
    [IO.File]::Copy($source, (Join-Path $nativeDir $name), $true)
}

$manifest = @(
    "target=$Target",
    "partitions=$($plans.Count)",
    "llvm_link=$LlvmLink",
    "boot_ll=$bootLl"
) -join "`n"
[IO.File]::WriteAllText((Join-Path $OutDir "manifest.txt"), $manifest + "`n")

$elapsed = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 1)
Write-Host ("partitioned IR complete: {0} ({1} bytes, {2}s)" -f
           $bootLl, (Get-Item -LiteralPath $bootLl).Length, $elapsed)

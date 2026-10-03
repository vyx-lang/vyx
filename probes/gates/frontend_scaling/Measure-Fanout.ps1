param(
    [ValidateRange(1, 64)][int]$Modules = 8,
    [switch]$NoImplicitStd,
    [string]$Compiler = (Join-Path $PSScriptRoot '../../../bootstrap_compiler/out/boot.exe'),
    [ValidateRange(1, 120)][int]$TimeoutSeconds = 30,
    [ValidateRange(128, 8192)][int]$PrivateLimitMiB = 2048
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$runDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-n' + $Modules)
$src = Join-Path $runDir 'src'
$interfaces = Join-Path $runDir 'interfaces'
New-Item -ItemType Directory -Path $src, $interfaces -Force | Out-Null
$imports = [Collections.Generic.List[string]]::new()
$calls = [Collections.Generic.List[string]]::new()
for ($i = 0; $i -lt $Modules; $i++) {
    $suffix = $i.ToString('D3')
    $imports.Add("use FrontendScale.Unit$suffix;")
    $calls.Add("    value = compute$suffix(value);")
    $body = @"
module FrontendScale.Unit$suffix;
public fn compute$suffix(seed: i64) -> i64 {
    return seed + $($i + 1);
}
"@
    [IO.File]::WriteAllText((Join-Path $src "unit$suffix.vyx"), $body)
    [IO.File]::WriteAllText((Join-Path $interfaces "unit$suffix.vyi"), "module FrontendScale.Unit$suffix;`npublic fn compute$suffix(seed: i64) -> i64;`n")
}
$main = @"
module FrontendScale;
$($imports -join "`n")
fn main() -> i32 {
    var value: i64 = 0;
$($calls -join "`n")
    if (value != $(($Modules * ($Modules + 1)) / 2)) { return 1; }
    return 0;
}
"@
$mainPath = Join-Path $src 'main.vyx'
[IO.File]::WriteAllText($mainPath, $main)
$argsList = @('--src=file', $mainPath, '--emit=obj', '-o', (Join-Path $runDir 'main.obj'), '--interface-root', $interfaces)
if ($NoImplicitStd) { $argsList += '--no-implicit-std' }
$start = [Diagnostics.ProcessStartInfo]::new($compilerPath)
$start.WorkingDirectory = $runDir
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
foreach ($item in $argsList) { $start.ArgumentList.Add($item) }
$start.Environment['LLVM_ROOT'] = Join-Path $repo 'clang'
$start.Environment['VYX_BOOTSTRAP_PROFILE'] = '1'
$start.Environment['VYX_DRIVER_PROFILE'] = '1'
$start.Environment['VYX_MIR_TIMING'] = '1'
$start.Environment['VYX_RT_PROFILE'] = '1'
$start.Environment['VYX_RT_MEM_PROFILE'] = '1'
$start.Environment['VYX_CODEGEN_UNITS'] = '1'
$proc = [Diagnostics.Process]::new()
$proc.StartInfo = $start
$timer = [Diagnostics.Stopwatch]::StartNew()
if (-not $proc.Start()) { throw 'Failed to start compiler' }
$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()
$samples = [Collections.Generic.List[object]]::new()
$stopReason = ''
$peakRss = 0L
$peakPrivate = 0L
$cpuObserved = 0.0
while (-not $proc.WaitForExit(100)) {
    $proc.Refresh()
    $peakRss = [math]::Max($peakRss, $proc.PeakWorkingSet64)
    $peakPrivate = [math]::Max($peakPrivate, $proc.PeakPagedMemorySize64)
    $cpuObserved = [math]::Max($cpuObserved, $proc.TotalProcessorTime.TotalSeconds)
    $samples.Add([pscustomobject]@{Milliseconds=$timer.ElapsedMilliseconds;RssBytes=$proc.WorkingSet64;PrivateBytes=$proc.PrivateMemorySize64;CpuMilliseconds=$proc.TotalProcessorTime.TotalMilliseconds})
    if ($timer.Elapsed.TotalSeconds -gt $TimeoutSeconds) { $stopReason = 'timeout' }
    if ($proc.PrivateMemorySize64 -gt ($PrivateLimitMiB * 1MB)) { $stopReason = 'private memory limit' }
    if ($stopReason) { $proc.Kill($true); $proc.WaitForExit(); break }
}
$timer.Stop()
$proc.Refresh()
$stdout = $stdoutTask.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $runDir 'stdout.log'), $stdout)
[IO.File]::WriteAllText((Join-Path $runDir 'stderr.log'), $stderrTask.GetAwaiter().GetResult())
$samples | Export-Csv -LiteralPath (Join-Path $runDir 'samples.csv') -NoTypeInformation
$reportedRss = ([regex]::Matches($stdout, 'peak_working_mb=(\d+)') | ForEach-Object { [long]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum
$reportedPrivate = ([regex]::Matches($stdout, 'peak_private_mb=(\d+)') | ForEach-Object { [long]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum
$result = [ordered]@{
    compiler=$compilerPath; compiler_sha256=(Get-FileHash -LiteralPath $compilerPath).Hash
    modules=$Modules; no_implicit_std=[bool]$NoImplicitStd; argv=$argsList
    wall_seconds=$timer.Elapsed.TotalSeconds; exit_code=$proc.ExitCode; stop_reason=$stopReason
    peak_rss_bytes=$peakRss; peak_private_bytes=$peakPrivate
    compiler_reported_peak_working_mib=$reportedRss; compiler_reported_peak_private_mib=$reportedPrivate
    observed_cpu_seconds=$cpuObserved; profile_flags=@('VYX_BOOTSTRAP_PROFILE','VYX_DRIVER_PROFILE','VYX_MIR_TIMING','VYX_RT_PROFILE','VYX_RT_MEM_PROFILE')
    note='Single direct compiler process, LLVM CGU count 1. Counters retain the largest observed OS lifetime peak; the final <100ms tail may be missed. This measures import/frontend scaling, not full project build throughput.'
}
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDir 'result.json')
$proc.Dispose()
[pscustomobject]@{RunDir=$runDir;Modules=$Modules;NoImplicitStd=[bool]$NoImplicitStd;Wall=$result.wall_seconds;Exit=$result.exit_code;Stop=$stopReason;PeakRssMiB=[math]::Round($result.peak_rss_bytes/1MB,1);PeakPrivateMiB=[math]::Round($result.peak_private_bytes/1MB,1)} | ConvertTo-Json

param(
    [Parameter(Mandatory)][string]$Compiler,
    [ValidateRange(1, 20)][int]$Repetitions = 3,
    [ValidateRange(64, 1024)][int]$SourceCount = 64,
    [int[]]$Jobs = @(1, 20),
    [ValidateRange(1, 10)][int]$NoOpRuns = 2,
    [ValidateRange(50, 10000)][int]$SampleIntervalMs = 100,
    [string]$OutputDir = (Join-Path $PSScriptRoot ('.runs/matrix-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
)

$ErrorActionPreference = 'Stop'
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$matrixPath = [IO.Path]::GetFullPath($OutputDir)
$runsRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.runs'))
if (-not $matrixPath.StartsWith($runsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Matrix fixtures and measurements must be beneath pipeline_perf/.runs/'
}
if (Test-Path -LiteralPath $matrixPath) { throw "Refusing to overwrite a matrix: $matrixPath" }
foreach ($jobCount in $Jobs) {
    if ($jobCount -lt 1 -or $jobCount -gt 1024) { throw "Invalid worker count: $jobCount" }
}
New-Item -ItemType Directory -Path (Join-Path $matrixPath 'measurements') -Force | Out-Null
$pwshPath = (Get-Process -Id $PID).Path
$identity = $null
$matrixExit = 0
[ordered]@{
    started_utc = [DateTime]::UtcNow.ToString('o')
    compiler = $compilerPath
    repetitions = $Repetitions
    source_count = $SourceCount
    jobs = $Jobs
    consecutive_noop_runs = $NoOpRuns
    sample_interval_ms = $SampleIntervalMs
    workload = 'Independent source units, not a Zyn or equivalent-Clang benchmark.'
    cache_policy = 'Fresh fixture for each cold run; subsequent no-op builds change no source. No directories are cleaned.'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $matrixPath 'matrix.json') -Encoding utf8NoBOM

try {
    for ($repetition = 1; $repetition -le $Repetitions; $repetition++) {
        # Alternate which worker count runs first to reduce ordering bias.
        $jobOrder = @($Jobs)
        if (($repetition % 2) -eq 0) { [Array]::Reverse($jobOrder) }
        foreach ($jobCount in $jobOrder) {
            $group = "rep$repetition-j$jobCount"
            $fixture = & (Join-Path $PSScriptRoot 'New-Fixture.ps1') -SourceCount $SourceCount -OutputDir (Join-Path $matrixPath "fixtures/$group")
            for ($pass = 0; $pass -le $NoOpRuns; $pass++) {
                $label = if ($pass -eq 0) { 'cold' } else { "no-op-$pass" }
                $measurementPath = Join-Path $matrixPath "measurements/$group-$label"
                Write-Host "Matrix $group $label"
                & $pwshPath -NoProfile -File (Join-Path $PSScriptRoot 'Measure-Build.ps1') `
                    -Compiler $compilerPath -ProjectDir $fixture -Target pipeline_perf `
                    -Jobs $jobCount -CacheLabel $label -SampleIntervalMs $SampleIntervalMs `
                    -OutputDir $measurementPath
                $measurementExit = $LASTEXITCODE
                if ($measurementExit -ne 0) {
                    $matrixExit = $measurementExit
                    throw "Measurement failed with $measurementExit; artifacts: $measurementPath"
                }
                $result = Get-Content (Join-Path $measurementPath 'result.json') -Raw | ConvertFrom-Json
                $runIdentity = @($result.metadata.compiler_sha256) + @($result.metadata.compiler_directory_artifacts | ForEach-Object { $_.role + ':' + $_.sha256 })
                $identityText = $runIdentity -join '|'
                if ($null -eq $identity) { $identity = $identityText }
                if ($identity -ne $identityText) {
                    throw 'Compiler/native artifacts changed during the measurement matrix; do not aggregate these runs.'
                }
                & (Join-Path $fixture 'target/pipeline_perf.exe')
                if ($LASTEXITCODE -ne 0) {
                    $matrixExit = $LASTEXITCODE
                    throw "Generated fixture result failed with $matrixExit"
                }
                $objects = @(Get-ChildItem -LiteralPath (Join-Path $fixture '.cache') -Filter '*.obj')
                if ($objects.Count -lt ($SourceCount + 1)) {
                    throw "Expected at least $($SourceCount + 1) independent objects; found $($objects.Count)"
                }
            }
        }
    }
} catch {
    if ($matrixExit -eq 0) { $matrixExit = 1 }
    Write-Error $_ -ErrorAction Continue
} finally {
    & (Join-Path $PSScriptRoot 'Summarize-Runs.ps1') -InputDir (Join-Path $matrixPath 'measurements') -OutputDir $matrixPath
}
Write-Host "Matrix artifacts: $matrixPath"
exit $matrixExit

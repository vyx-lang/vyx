param(
    [Parameter(Mandatory)][string]$InputDir,
    [string]$OutputDir = $InputDir
)

$ErrorActionPreference = 'Stop'
$rows = @()
foreach ($file in (Get-ChildItem -LiteralPath $InputDir -Filter result.json -Recurse -File)) {
    $result = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    $logPath = Join-Path $file.DirectoryName 'stdout.log'
    $log = if (Test-Path -LiteralPath $logPath) { @(Get-Content -LiteralPath $logPath) } else { @() }
    $summary = $result.summary
    $rows += [pscustomobject][ordered]@{
        run = $file.Directory.Name
        jobs = $result.metadata.jobs
        cache_label = $result.metadata.cache_label
        exit_code = $summary.exit_code
        wall_seconds = $summary.wall_seconds
        parent_peak_rss_mib = $summary.parent_peak_rss_bytes / 1MB
        parent_peak_private_mib = $summary.parent_peak_private_bytes / 1MB
        tree_peak_rss_mib = $summary.tree_sampled_peak_rss_bytes / 1MB
        tree_peak_private_mib = $summary.tree_sampled_peak_private_bytes / 1MB
        observed_cpu_seconds = $summary.observed_cpu_seconds
        observed_cpu_over_wall = $summary.observed_cpu_over_wall
        peak_vyx_children = $summary.peak_active_vyx_compiler_children
        sample_work_ms = $summary.sample_work_ms
        vyi_compiles = @($log | Where-Object { $_ -match '\] compile .*\.vyi' }).Count
        object_compiles = @($log | Where-Object { $_ -match '\] compile .*\.obj' }).Count
        links = @($log | Where-Object { $_ -match '\] link ' }).Count
        link_cache_hits = @($log | Where-Object { $_ -match '\] cache link ' }).Count
        compiler_sha256 = $result.metadata.compiler_sha256
        result_file = $file.FullName
    }
}
if ($rows.Count -eq 0) {
    Write-Warning 'No completed result.json files to summarize.'
    return
}
$rows = @($rows | Sort-Object jobs, cache_label, run)
$rows | Export-Csv -LiteralPath (Join-Path $OutputDir 'runs.csv') -NoTypeInformation -Encoding utf8NoBOM
$groups = @()
foreach ($group in ($rows | Group-Object jobs, cache_label)) {
    $values = @($group.Group | Where-Object { $_.exit_code -eq 0 } | Select-Object -ExpandProperty wall_seconds | Sort-Object)
    if ($values.Count -eq 0) { continue }
    $middle = [int][Math]::Floor($values.Count / 2)
    $median = if (($values.Count % 2) -eq 0) { ($values[$middle - 1] + $values[$middle]) / 2 } else { $values[$middle] }
    $groups += [pscustomobject][ordered]@{
        jobs = $group.Group[0].jobs
        cache_label = $group.Group[0].cache_label
        successful_runs = $values.Count
        failed_runs = @($group.Group | Where-Object { $_.exit_code -ne 0 }).Count
        wall_min_seconds = $values[0]
        wall_median_seconds = $median
        wall_mean_seconds = ($values | Measure-Object -Average).Average
        wall_max_seconds = $values[-1]
        tree_rss_max_mib = ($group.Group.tree_peak_rss_mib | Measure-Object -Maximum).Maximum
        tree_private_max_mib = ($group.Group.tree_peak_private_mib | Measure-Object -Maximum).Maximum
        peak_vyx_children = ($group.Group.peak_vyx_children | Measure-Object -Maximum).Maximum
    }
}
[ordered]@{ runs = $rows; groups = $groups; note = 'Sampled CPU is a lower bound. This independent-unit fixture does not establish Zyn performance or parity with Clang.' } |
    ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $OutputDir 'summary.json') -Encoding utf8NoBOM
$groups | Export-Csv -LiteralPath (Join-Path $OutputDir 'summary.csv') -NoTypeInformation -Encoding utf8NoBOM
$groups | Format-Table jobs, cache_label, successful_runs, wall_min_seconds, wall_median_seconds, wall_max_seconds, tree_rss_max_mib -AutoSize

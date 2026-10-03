param([Parameter(Mandatory)][string]$InputDir)
$ErrorActionPreference = 'Stop'
$rows = @(Import-Csv -LiteralPath (Join-Path $InputDir 'runs.csv'))
$groups = @()
foreach ($group in ($rows | Group-Object language, stage, jobs)) {
    $ok = @($group.Group | Where-Object {[int]$_.exit_code -eq 0})
    if ($ok.Count -eq 0) { continue }
    $values = @($ok | ForEach-Object {[double]$_.wall_seconds} | Sort-Object)
    $middle = [int][Math]::Floor($values.Count/2)
    $median = if (($values.Count % 2) -eq 0) {($values[$middle-1]+$values[$middle])/2} else {$values[$middle]}
    $groups += [pscustomobject][ordered]@{
        language=$group.Group[0].language; stage=$group.Group[0].stage; jobs=[int]$group.Group[0].jobs
        successful_runs=$ok.Count; failed_runs=$group.Count-$ok.Count
        wall_min_seconds=$values[0]; wall_median_seconds=$median; wall_max_seconds=$values[-1]
        tree_rss_max_mib=($ok | ForEach-Object {[double]$_.tree_peak_rss_mib} | Measure-Object -Maximum).Maximum
        tree_private_max_mib=($ok | ForEach-Object {[double]$_.tree_peak_private_mib} | Measure-Object -Maximum).Maximum
        object_tasks_min=($ok | ForEach-Object {[int]$_.object_tasks} | Measure-Object -Minimum).Minimum
        object_tasks_max=($ok | ForEach-Object {[int]$_.object_tasks} | Measure-Object -Maximum).Maximum
        link_tasks_min=($ok | ForEach-Object {[int]$_.link_tasks} | Measure-Object -Minimum).Minimum
        link_tasks_max=($ok | ForEach-Object {[int]$_.link_tasks} | Measure-Object -Maximum).Maximum
    }
}
$ratios = @()
foreach ($vyx in @($groups | Where-Object {$_.language -eq 'vyx'})) {
    $cpp = @($groups | Where-Object {$_.language -eq 'cpp' -and $_.stage -eq $vyx.stage -and $_.jobs -eq $vyx.jobs})
    if ($cpp.Count -eq 1) {
        $ratios += [pscustomobject]@{stage=$vyx.stage; jobs=$vyx.jobs; vyx_median_seconds=$vyx.wall_median_seconds; cpp_median_seconds=$cpp[0].wall_median_seconds; vyx_over_cpp=$vyx.wall_median_seconds/$cpp[0].wall_median_seconds}
    }
}
$groups | Export-Csv -LiteralPath (Join-Path $InputDir 'summary.csv') -NoTypeInformation -Encoding utf8NoBOM
$ratios | Export-Csv -LiteralPath (Join-Path $InputDir 'ratios.csv') -NoTypeInformation -Encoding utf8NoBOM
[ordered]@{groups=$groups; ratios=$ratios; note='Configure time is separate. CPU/tree memory are sampled; tiny runs can be missed. Ratios characterize only this generated workload.'} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $InputDir 'summary.json') -Encoding utf8NoBOM

param([string]$Compiler = '', [string]$BaselineRevision = '8e44b876', [switch]$SkipBaseline,
      [string[]]$Modes = @('ownership', 'cold', 'history', 'disabled'))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$runDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path (Join-Path $runDir 'fixtures') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $runDir '.cache') -Force | Out-Null
$source = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/build_system.vyx'))
$actionSource = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/build_action.vyx'))
$resourceSource = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/build_resources.vyx'))
$actionModel = [regex]::Match($actionSource, '(?ms)^public class BuildAction\s*\{.*?^\}').Value
if (-not $actionModel) { throw 'Missing production BuildAction model' }
$currentSource = $source + "`n" + $actionSource + "`n" + $resourceSource

function Get-Function([string]$Text, [string]$Name) {
    $match = [regex]::Match($Text, ('(?m)^(?:public )?fn ' + [regex]::Escape($Name) + '\s*\('))
    if (-not $match.Success) { throw "Missing production function $Name" }
    $open = $Text.IndexOf('{', $match.Index)
    $depth = 0
    $quote = [char]0
    $lineComment = $false
    $blockComment = $false
    for ($pos = $open; $pos -lt $Text.Length; $pos++) {
        $ch = $Text[$pos]
        $next = if ($pos + 1 -lt $Text.Length) { $Text[$pos + 1] } else { [char]0 }
        if ($lineComment) { if ($ch -eq "`n") { $lineComment = $false }; continue }
        if ($blockComment) {
            if ($ch -eq '*' -and $next -eq '/') { $blockComment = $false; $pos++ }
            continue
        }
        if ($quote -ne [char]0) {
            if ($ch -eq '\') { $pos++; continue }
            if ($ch -eq $quote) { $quote = [char]0 }
            continue
        }
        if ($ch -eq '/' -and $next -eq '/') { $lineComment = $true; $pos++; continue }
        if ($ch -eq '/' -and $next -eq '*') { $blockComment = $true; $pos++; continue }
        if ($ch -eq '"' -or $ch -eq "'") { $quote = $ch; continue }
        if ($ch -eq '{') { $depth++ }
        if ($ch -eq '}') {
            $depth--
            if ($depth -eq 0) { return $Text.Substring($match.Index, $pos - $match.Index + 1) }
        }
    }
    throw "Unterminated production function $Name"
}

function Write-Unit([string]$Text, [string[]]$Roots, [string]$Probe, [string]$Output, [string]$Model = '') {
    $queue = [Collections.Generic.Queue[string]]::new()
    foreach ($name in $Roots) { $queue.Enqueue($name) }
    $seen = [Collections.Generic.HashSet[string]]::new()
    $bodies = [Collections.Generic.List[string]]::new()
    while ($queue.Count -gt 0) {
        $name = $queue.Dequeue()
        if (-not $seen.Add($name)) { continue }
        $body = Get-Function $Text $name
        $bodies.Add($body)
        foreach ($call in [regex]::Matches($body, '\b(build_[A-Za-z0-9_]+)\s*\(')) {
            if (-not $seen.Contains($call.Groups[1].Value)) { $queue.Enqueue($call.Groups[1].Value) }
        }
    }
    $extern = [regex]::Match($Text, '(?ms)^extern "C" \{.*?^\}').Value
    $header = "module pipeline_scheduler_probe;`nuse std.clone;`nuse std.collections;`nuse std.fs;`nuse std.string;`n"
    [IO.File]::WriteAllText($Output, ($header + $extern + "`n" + $Model + "`n" + ($bodies -join "`n`n") + "`n" + $Probe))
    Write-Host "extracted $($seen.Count) production functions -> $Output"
}

function Write-Fixture([string]$Name, [string]$Content) {
    [IO.File]::WriteAllText((Join-Path $runDir $Name), $Content)
}
Write-Fixture 'fixtures/present.obj' 'exists'
Write-Fixture 'fixtures/long_records.tsv' ("a`tb`t" + ('x' * 96) + "`nb`tc`t" + ('k' * 96) + "`n")
Write-Fixture 'fixtures/long_records_expected.tsv' ("a`tb`tz`nb`tc`t" + ('k' * 96) + "`n")
foreach ($length in @(0, 1, 23, 24, 63, 64)) {
    Write-Fixture "fixtures/length_$length.txt" ('x' * $length)
}
for ($kind = 0; $kind -lt 4; $kind++) { Write-Fixture "fixtures/$kind.vyx" ('x' * (4 + $kind)) }
$tasks = [Collections.Generic.List[string]]::new()
$times = [Collections.Generic.List[string]]::new()
for ($i = 0; $i -lt 96; $i++) {
    $kind = $i % 4
    $tasks.Add("$i`tfixtures/$kind.vyx`titem$i.obj`tcommand$i`n")
    # History prefers the reverse cost order from the input-size estimate.
    $times.Add("item$i.obj`t$(100 - $kind)`n")
}
Write-Fixture 'fixtures/tasks.tsv' ($tasks -join '')
Write-Fixture '.cache/build_task_times.tsv' ($times -join '')
Write-Fixture 'fixtures/expected_disabled.tsv' ($tasks -join '')
$cold = [Collections.Generic.List[string]]::new()
$history = [Collections.Generic.List[string]]::new()
for ($group = 3; $group -ge 0; $group--) {
    for ($i = $group; $i -lt 96; $i += 4) { $cold.Add($tasks[$i]) }
}
for ($group = 0; $group -lt 4; $group++) {
    for ($i = $group; $i -lt 96; $i += 4) { $history.Add($tasks[$i]) }
}
Write-Fixture 'fixtures/expected_cold.tsv' ($cold -join '')
Write-Fixture 'fixtures/expected_history.tsv' ($history -join '')
foreach ($name in @('one', 'llvm_lower', 'mir_builder', 'sema', 'a', 'b', 'c')) {
    Write-Fixture "fixtures/$name.vyx" 'same bytes'
}
Write-Fixture 'fixtures/named.tsv' "0`tfixtures/sema.vyx`tsema.obj`tcmd`n1`tfixtures/mir_builder.vyx`tmir.obj`tcmd`n2`tfixtures/llvm_lower.vyx`tllvm.obj`tcmd`n"
Write-Fixture 'fixtures/renamed.tsv' "0`tfixtures/a.vyx`tsema.obj`tcmd`n1`tfixtures/b.vyx`tmir.obj`tcmd`n2`tfixtures/c.vyx`tllvm.obj`tcmd`n"

$roots = @('build_cache_digest_index', 'build_cache_has_indexed', 'build_cache_replace_many',
           'build_action_launch_order', 'build_action_ready', 'build_task_priority', 'build_field',
           'build_resource_credit', 'build_resource_headroom', 'build_resource_limit', 'build_resource_fits',
           'build_resource_cold_estimate', 'build_resource_measured_estimate', 'build_action_memory_estimate')
$unit = Join-Path $runDir 'scheduler.vyx'
Write-Unit $currentSource $roots ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'probe.vyx'))) $unit $actionModel
if (-not $SkipBaseline) {
    $baselineLines = & git -C $repo show ($BaselineRevision + ':bootstrap_compiler/src/core/build_system.vyx')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read baseline production source' }
    $baseline = $baselineLines -join "`n"
    Write-Unit $baseline @('build_cache_digest_index', 'build_cache_has_indexed') `
        ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'baseline_collision.vyx'))) (Join-Path $runDir 'baseline.vyx')
}
$saved = @{}
foreach ($key in @('LLVM_ROOT', 'PATH', 'VYX_BUILD_USE_TASK_TIMES', 'VYX_BUILD_NO_TASK_PRIORITIZE')) {
    $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
}
try {
    $env:LLVM_ROOT = Join-Path $repo 'clang'
    $env:PATH = (Split-Path $Compiler) + ';' + (Join-Path $env:LLVM_ROOT 'bin') + ';' + $env:PATH
    Push-Location $runDir
    try {
        $env:VYX_BUILD_USE_TASK_TIMES = '0'
        $env:VYX_BUILD_NO_TASK_PRIORITIZE = '0'
        $units = @('scheduler')
        if (-not $SkipBaseline) { $units += 'baseline' }
        foreach ($name in $units) {
            & $Compiler --src=file (Join-Path $runDir "$name.vyx") --emit=exe -o (Join-Path $runDir "$name.exe") `
                -L (Split-Path $Compiler) -l vyx_compiler_backend *> "$name.compile.log"
            if ($LASTEXITCODE -ne 0) {
                Get-Content "$name.compile.log" -Tail 50
                throw "$name compilation failed: $LASTEXITCODE"
            }
        }
        if (-not $SkipBaseline) {
            & (Join-Path $runDir 'baseline.exe')
            if ($LASTEXITCODE -ne 0) { throw "Baseline reproduction failed: $LASTEXITCODE" }
        }
        foreach ($mode in $Modes) {
            $env:VYX_BUILD_USE_TASK_TIMES = if ($mode -eq 'history') { '1' } else { '0' }
            $env:VYX_BUILD_NO_TASK_PRIORITIZE = if ($mode -eq 'disabled') { '1' } else { '0' }
            & (Join-Path $runDir 'scheduler.exe') $mode
            if ($LASTEXITCODE -ne 0) { throw "Scheduler $mode failed: $LASTEXITCODE" }
        }
    } finally { Pop-Location }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
}
Write-Host "artifacts: $runDir"

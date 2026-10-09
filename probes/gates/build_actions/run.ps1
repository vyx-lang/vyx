param(
    [string]$Compiler = '',
    [switch]$PrepareOnly,
    [string]$PreparedDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$runtimeDir = Split-Path $Compiler

function Get-ProductionFunction([string]$Text, [string]$Name) {
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

if ($PreparedDir) {
    $runDir = (Resolve-Path -LiteralPath $PreparedDir).Path
} else {
    $runDir = Join-Path $PSScriptRoot ('.runs/lifecycle-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $systemPath = Join-Path $repo 'bootstrap_compiler/src/core/project/build_system.vyx'
    $actionPath = Join-Path $repo 'bootstrap_compiler/src/core/project/build_action.vyx'
    $resourcePath = Join-Path $repo 'bootstrap_compiler/src/core/project/build_resources.vyx'
    $policyPath = Join-Path $repo 'bootstrap_compiler/src/core/sema/policy.vyx'
    $system = [IO.File]::ReadAllText($systemPath)
    $action = [IO.File]::ReadAllText($actionPath)
    $source = $system + "`n" + $action + "`n" + [IO.File]::ReadAllText($resourcePath) + "`n" + [IO.File]::ReadAllText($policyPath)
    $model = [regex]::Match($action, '(?ms)^public class BuildAction\s*\{.*?^\}').Value
    if (-not $model) { throw 'Missing production BuildAction model' }
    $queue = [Collections.Generic.Queue[string]]::new()
    foreach ($name in @('build_run_action_batch', 'build_parse_int', 'build_quote', 'build_task_tmp_output',
                        'build_action_ready', 'build_action_running', 'build_action_succeeded',
                        'build_action_failed', 'build_action_cancelled')) { $queue.Enqueue($name) }
    $seen = [Collections.Generic.HashSet[string]]::new()
    $bodies = [Collections.Generic.List[string]]::new()
    while ($queue.Count -gt 0) {
        $name = $queue.Dequeue()
        if (-not $seen.Add($name)) { continue }
        $body = Get-ProductionFunction $source $name
        $bodies.Add($body)
        foreach ($call in [regex]::Matches($body, '\b((?:build|policy)_[A-Za-z0-9_]+)\s*\(')) {
            if (-not $seen.Contains($call.Groups[1].Value)) { $queue.Enqueue($call.Groups[1].Value) }
        }
    }
    $joined = $bodies -join "`n`n"
    $globals = [Collections.Generic.List[string]]::new()
    foreach ($global in [regex]::Matches($source, '(?m)^var g_build_\w+[^\r\n]*;')) {
        $name = [regex]::Match($global.Value, '\bg_build_\w+').Value
        if ($joined -match ('\b' + [regex]::Escape($name) + '\b')) { $globals.Add($global.Value) }
    }
    $extern = [regex]::Match($system, '(?ms)^extern "C" \{.*?^\}').Value
    $header = "module build_action_lifecycle_probe;`nuse std.clone;`nuse std.collections;`nuse std.fs;`nuse std.string;`n"
    $unit = Join-Path $runDir 'batch.vyx'
    [IO.File]::WriteAllText($unit, $header + $extern + "`n" + $model + "`n" + ($globals -join "`n") + "`n" + $joined + "`n" + [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'probe.vyx')))
    [ordered]@{
        compiler=$Compiler; compiler_sha256=(Get-FileHash -LiteralPath $Compiler).Hash
        runtime_sha256=(Get-FileHash -LiteralPath (Join-Path $runtimeDir 'vyx_compiler_backend.dll')).Hash
        functions=@($seen); source_files=@($systemPath,$actionPath,$resourcePath,$policyPath)
        source_sha256=@((Get-FileHash -LiteralPath $systemPath).Hash,(Get-FileHash -LiteralPath $actionPath).Hash,(Get-FileHash -LiteralPath $resourcePath).Hash,(Get-FileHash -LiteralPath $policyPath).Hash)
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDir 'extraction.json')
    Write-Host "extracted $($seen.Count) production functions -> $unit"
    & (Join-Path $repo 'clang/bin/clang++.exe') -std=c++20 -O2 (Join-Path $PSScriptRoot 'child.cpp') -o (Join-Path $runDir 'child.exe')
    if ($LASTEXITCODE -ne 0) { throw "Child compiler exited $LASTEXITCODE" }
    $oldPath = $env:PATH
    $oldLlvm = $env:LLVM_ROOT
    try {
        $env:LLVM_ROOT = Join-Path $repo 'clang'
        $env:PATH = $runtimeDir + ';' + $env:PATH
        & $Compiler --src=file $unit --emit=exe -o (Join-Path $runDir 'batch.exe') -L $runtimeDir -l vyx_compiler_backend *> (Join-Path $runDir 'compile.log')
        if ($LASTEXITCODE -ne 0) {
            Get-Content -LiteralPath (Join-Path $runDir 'compile.log') -Tail 80
            throw "Batch gate compilation failed: $LASTEXITCODE"
        }
    } finally { $env:PATH = $oldPath; $env:LLVM_ROOT = $oldLlvm }
}
Write-Host "prepared: $runDir"
if ($PrepareOnly) { return }

$results = [Collections.Generic.List[object]]::new()
foreach ($case in @(@{Mode='success';Jobs=1}, @{Mode='success';Jobs=20}, @{Mode='tree_success';Jobs=20}, @{Mode='failure';Jobs=20})) {
    $caseDir = Join-Path $runDir ($case.Mode + '-j' + $case.Jobs)
    if (Test-Path -LiteralPath $caseDir) { throw "Refusing to reuse lifecycle outputs: $caseDir" }
    New-Item -ItemType Directory -Path $caseDir | Out-Null
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $runDir 'batch.exe'))
    $start.WorkingDirectory = $caseDir
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($arg in @($case.Mode, [string]$case.Jobs, (Join-Path $runDir 'child.exe'))) { $start.ArgumentList.Add($arg) }
    $start.Environment['PATH'] = $runtimeDir + ';' + $env:PATH
    $start.Environment['VYX_BUILD_PROFILE'] = '1'
    $start.Environment['VYX_BUILD_TRACE'] = '1'
    $start.Environment['VYX_BUILD_NO_TASK_PRIORITIZE'] = '0'
    $start.Environment['VYX_BUILD_USE_TASK_TIMES'] = '0'
    $start.Environment['VYX_BOOTSTRAP_DISABLE_JOB_OBJECT'] = '0'
    $start.Environment['VYX_BUILD_MEMORY_RESERVE_MB'] = '64'
    $start.Environment['VYX_BUILD_MEMORY_SLOTS'] = '0'
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $timer = [Diagnostics.Stopwatch]::StartNew()
    if (-not $process.Start()) { throw 'Could not start batch gate' }
    $outTask = $process.StandardOutput.ReadToEndAsync()
    $errTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(45000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw "Batch lifecycle timeout: $caseDir"
    }
    $timer.Stop()
    $stdout = $outTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $caseDir 'stdout.log'), $stdout)
    [IO.File]::WriteAllText((Join-Path $caseDir 'stderr.log'), $errTask.GetAwaiter().GetResult())
    $exitCode = $process.ExitCode
    $process.Dispose()
    $launches = [regex]::Matches($stdout, '\[build-sched\] launch step=\d+ active=(\d+)')
    $activePeak = ($launches | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum
    $pids = @(Get-ChildItem -LiteralPath $caseDir -Filter '*.pid' | ForEach-Object { [int](Get-Content -Raw -LiteralPath $_.FullName) })
    Start-Sleep -Milliseconds 250
    $live = @($pids | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    if ($live.Count -gt 0) {
        # Cleanup is restricted to PIDs recorded by our child executable, and
        # their image path is checked to avoid touching a reused PID.
        foreach ($remaining in $live) {
            if ($remaining.Path -eq (Join-Path $runDir 'child.exe')) { Stop-Process -Id $remaining.Id -Force }
        }
        throw "Batch left $($live.Count) root/descendant processes alive"
    }
    if ($exitCode -ne 0) { throw "Batch gate rejected $($case.Mode): exit $exitCode; $caseDir" }
    if ($activePeak -ne $case.Jobs) { throw "Expected $($case.Jobs) active tokens, got $activePeak" }
    $overlap = 0
    if ($case.Mode -ne 'failure') {
        $events = [Collections.Generic.List[object]]::new()
        for ($i = 0; $i -lt 40; $i++) {
            $events.Add([pscustomobject]@{Time=[long](Get-Content -Raw -LiteralPath (Join-Path $caseDir "start_$i.time"));Delta=1})
            $events.Add([pscustomobject]@{Time=[long](Get-Content -Raw -LiteralPath (Join-Path $caseDir "end_$i.time"));Delta=-1})
        }
        $active = 0
        foreach ($event in ($events | Sort-Object Time,Delta)) {
            $active += $event.Delta
            $overlap = [math]::Max($overlap,$active)
        }
        if ($overlap -lt $case.Jobs -or $overlap -gt $case.Jobs) {
            throw "Actual child lifetime overlap $overlap differs from jobs=$($case.Jobs)"
        }
    } else {
        if (@(Get-ChildItem -LiteralPath $caseDir -Filter 'descendant_completed_*').Count -ne 0) {
            throw 'A cancelled descendant completed its delayed output'
        }
        if ($launches.Count -ne 20) { throw "Failure batch launched pending actions: $($launches.Count)" }
    }
    $results.Add([pscustomobject]@{mode=$case.Mode;jobs=$case.Jobs;exit=$exitCode;wall_seconds=$timer.Elapsed.TotalSeconds;launches=$launches.Count;active_token_peak=$activePeak;actual_overlap=$overlap;recorded_processes=$pids.Count;survivors=$live.Count})
    Write-Host "OK $($case.Mode) jobs=$($case.Jobs) tokens=$activePeak overlap=$overlap survivors=$($live.Count)"
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDir 'results.json')
Write-Host "artifacts: $runDir"

param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$runDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
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

$system = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/project/build_system.vyx'))
$model = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/project/build_source_index.vyx')).Replace('module bootstrap.build_source_index;', 'module source_index_probe;')
$sets = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/project/build_sets.vyx'))
# This fixed revision predates the source-tree migration.
$old = (& git -C $repo show e52de175:bootstrap_compiler/src/core/build_system.vyx) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Cannot read reference source' }
$source = $system + "`n" + $sets + "`n" + [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/sema/policy.vyx')) + "`n" + $old
$queue = [Collections.Generic.Queue[string]]::new()
@('build_make_source_index', 'build_partition_dependency_stamp', 'build_auto_vyx_import_sources_mode', 'build_module_files_from_registry', 'build_write_peer_vyi_units', 'build_emit_peer_vyi_files') | ForEach-Object { $queue.Enqueue($_) }
$seen = [Collections.Generic.HashSet[string]]::new()
$bodies = [Collections.Generic.List[string]]::new()
while ($queue.Count) {
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
foreach ($g in [regex]::Matches($system, '(?m)^var g_build_\w+[^\r\n]*;')) {
    $name = [regex]::Match($g.Value, '\bg_build_\w+').Value
    if ($joined -match ('\b' + $name + '\b')) { $globals.Add($g.Value) }
}
$extern = [regex]::Match($system, '(?ms)^extern "C" \{.*?^\}').Value
$old = (& git -C $repo show e52de175:bootstrap_compiler/src/core/build_system.vyx) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Cannot read reference source' }
$reference = (Get-ProductionFunction $old 'build_auto_vyx_import_sources_mode').Replace('fn build_auto_vyx_import_sources_mode(', 'fn reference_auto_imports(')
$unit = $model + "`nuse std.clone;`nuse std.fs;`nuse std.string;`nuse std.serde.toml;`n" + $extern + "`n" + ($globals -join "`n") + "`n" + $joined + "`n" + $reference + "`n" + [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'probe.vyx'))
[IO.File]::WriteAllText((Join-Path $runDir 'probe.vyx'), $unit)
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = (Split-Path $Compiler) + ';' + $env:PATH
Push-Location $runDir
try {
    & $Compiler --src=file probe.vyx --emit=exe -o probe.exe -L (Split-Path $Compiler) -l vyx_compiler_backend *> compile.log
    if ($LASTEXITCODE -ne 0) { Get-Content compile.log -Tail 45; throw 'Index gate compile failed' }
    & ./probe.exe
    if ($LASTEXITCODE -ne 0) { throw "Index gate failed $LASTEXITCODE" }
    Write-Host "Source index gate passed: $runDir ($($seen.Count) production functions)"
} finally { Pop-Location }

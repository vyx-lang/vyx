param([string]$Compiler = '', [string]$BaselineRevision = 'e52de175')
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

$current = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/build_sets.vyx'))
$oldLines = & git -C $repo show ($BaselineRevision + ':bootstrap_compiler/src/core/build_system.vyx')
if ($LASTEXITCODE -ne 0) { throw 'Cannot read baseline production source' }
$old = $oldLines -join "`n"
$baselineFunctions = @('build_contains', 'build_pipe_set_has', 'build_pipe_set_add', 'build_pipe_set_merge') |
    ForEach-Object { Get-ProductionFunction $old $_ }
$probe = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'probe.vyx'))
$header = "module build_sets_probe;`nuse std.clone;`nuse std.collections;`nuse std.string;`nuse std.fs;`n"
$oldExtern = 'extern "C" { fn vyx_string_contains(hay: rawptr, hay_len: u64, needle: rawptr, needle_len: u64) -> i32; }'
[IO.File]::WriteAllText((Join-Path $runDir 'baseline.vyx'),
    $header + $oldExtern + "`n" + ($baselineFunctions -join "`n`n") + "`n" + $probe)
# Unit extraction: all new function bodies are copied verbatim. Only the module
# header gains the fixture's std.fs import; no implementation is re-created.
$newUnit = $current.Replace('module bootstrap.build_sets;', "module build_sets_probe;`nuse std.fs;")
[IO.File]::WriteAllText((Join-Path $runDir 'current.vyx'), $newUnit + "`n" + $probe)
foreach ($length in @(0, 1, 23, 24, 63, 64)) {
    [IO.File]::WriteAllText((Join-Path $runDir "length_$length.txt"), ('x' * $length))
}
$names = for ($i = 0; $i -lt 1436; $i++) { 'root_{0:D4}_method_payload' -f $i }
[IO.File]::WriteAllText((Join-Path $runDir 'whitespace.txt'), ([string][char]11 + [char]12 + 'a' + [char]11 + [char]12))
[IO.File]::WriteAllText((Join-Path $runDir 'spaces_long.txt'), (' ' * 160))
[IO.File]::WriteAllText((Join-Path $runDir 'trim_long.txt'), ((' ' * 80) + 'Aa' + (' ' * 80)))
[IO.File]::WriteAllText((Join-Path $runDir 'large.txt'), ($names -join '|'))
$savedPath = $env:PATH
$savedLlvm = $env:LLVM_ROOT
try {
    $env:LLVM_ROOT = Join-Path $repo 'clang'
    $env:PATH = (Split-Path $Compiler) + ';' + (Join-Path $env:LLVM_ROOT 'bin') + ';' + $env:PATH
    Push-Location $runDir
    try {
        Write-Host "compiler: $Compiler"
        Write-Host "compiler_sha256: $((Get-FileHash $Compiler -Algorithm SHA256).Hash)"
        Write-Host "runtime_sha256: $((Get-FileHash (Join-Path (Split-Path $Compiler) 'vyx_compiler_backend.dll') -Algorithm SHA256).Hash)"
        foreach ($name in @('current', 'baseline')) {
            & $Compiler --src=file (Join-Path $runDir "$name.vyx") --emit=exe -o (Join-Path $runDir "$name.exe") `
                -L (Split-Path $Compiler) -l vyx_compiler_backend *> "$name.compile.log"
            if ($LASTEXITCODE -ne 0) {
                Get-Content "$name.compile.log" -Tail 50
                throw "$name compile failed: $LASTEXITCODE"
            }
            & (Join-Path $runDir "$name.exe") equivalence "$name.results"
            if ($LASTEXITCODE -ne 0) { throw "$name equivalence run failed: $LASTEXITCODE" }
            & (Join-Path $runDir "$name.exe") memory 'large.txt' | Tee-Object "$name.memory.log"
            if ($LASTEXITCODE -ne 0) { throw "$name memory run failed: $LASTEXITCODE" }
        }
        $currentHash = (Get-FileHash 'current.results' -Algorithm SHA256).Hash
        $baselineHash = (Get-FileHash 'baseline.results' -Algorithm SHA256).Hash
        if ($currentHash -ne $baselineHash) { throw 'Old/new helper output bytes differ' }
        $deltas = @{}
        foreach ($name in @('current', 'baseline')) {
            $log = [IO.File]::ReadAllText((Join-Path $runDir "$name.memory.log"))
            $before = [regex]::Match($log, 'private_before=(\d+)')
            $after = [regex]::Match($log, 'private_after=(\d+)')
            if (-not $before.Success -or -not $after.Success) { throw 'Missing real OS memory observation' }
            $deltas[$name] = [Math]::Max(0, [long]$after.Groups[1].Value - [long]$before.Groups[1].Value)
        }
        if ($deltas.current -gt $deltas.baseline / 4 + 8MB) { throw 'New merge lost its bounded allocation improvement' }
        Write-Host "equivalence SHA256: $currentHash"
    } finally { Pop-Location }
} finally {
    $env:PATH = $savedPath
    $env:LLVM_ROOT = $savedLlvm
}
Write-Host "build_sets: OK; artifacts: $runDir"

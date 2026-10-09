param(
    [string]$Compiler = (Join-Path $PSScriptRoot '../../../bootstrap_compiler/out/boot.exe'),
    [string]$SourceRef = '',
    [switch]$ExpectLegacyResetFailure
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$runDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$lowerPath = Join-Path $repo 'bootstrap_compiler/src/codegen/llvm/llvm_lower.vyx'
$lower = [IO.File]::ReadAllText($lowerPath)
if ($SourceRef) {
    $historicalPath = $null
    foreach ($candidate in @('bootstrap_compiler/src/codegen/llvm/llvm_lower.vyx', 'bootstrap_compiler/src/codegen/llvm_lower.vyx')) {
        & git -C $repo cat-file -e "${SourceRef}:$candidate" 2>$null
        if ($LASTEXITCODE -eq 0) { $historicalPath = $candidate; break }
    }
    if (-not $historicalPath) { throw "Cannot find lowerer at $SourceRef" }
    $lower = (& git -C $repo show "${SourceRef}:$historicalPath") -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "Cannot read lowerer at $SourceRef" }
}
$sourceHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($lower)))
$ast = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/syntax/ast.vyx'))

function Get-ProductionFunction([string]$Source, [string]$Name) {
    $match = [regex]::Match($Source, '(?m)^(?:public )?fn ' + [regex]::Escape($Name) + '\(')
    if (-not $match.Success) { throw "Production helper missing: $Name" }
    $open = $Source.IndexOf('{', $match.Index)
    $depth = 1
    $end = $open + 1
    while ($end -lt $Source.Length -and $depth -gt 0) {
        if ($Source[$end] -eq '{') { $depth++ }
        if ($Source[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth -ne 0) { throw "Unbalanced production helper: $Name" }
    return $Source.Substring($match.Index, $end - $match.Index)
}

$prefix = @'
module lower_maps_probe;
extern "C" {
    fn malloc(bytes: u64) -> rawptr;
    fn free(ptr: rawptr);
    fn memcpy(dst: rawptr, src: rawptr, bytes: u64) -> rawptr;
    fn memset(dst: rawptr, value: i32, bytes: u64) -> rawptr;
    fn exit(status: i32);
}
// Only the diagnostic display sink is local to this standalone probe.
fn diag_emit_codegen(message: string) { print(message); }
'@
$pieces = [Collections.Generic.List[string]]::new()
$pieces.Add($prefix)
foreach ($name in @('ptr_offset', 'ast_read_rawptr', 'ast_write_rawptr', 'ast_read_i32', 'ast_write_i32', 'ast_read_i64', 'ast_write_i64')) {
    $pieces.Add((Get-ProductionFunction $ast $name))
}
# Extract the actual functions, including future descriptor support helpers.
$supportNames = [regex]::Matches($lower, '(?m)^fn (llvm_mir_id_map_\w+)\(') | ForEach-Object { $_.Groups[1].Value }
foreach ($name in $supportNames) { $pieces.Add((Get-ProductionFunction $lower $name)) }
foreach ($name in @('llvm_mir_alloc_ptr_map', 'llvm_mir_alloc_i32_map', 'llvm_mir_ptr_map_zero', 'llvm_mir_i32_map_zero', 'llvm_mir_map_get', 'llvm_mir_map_put', 'llvm_mir_map_clear_range', 'llvm_mir_i32_map_get', 'llvm_mir_i32_map_put', 'llvm_mir_i32_map_clear_range')) {
    $pieces.Add((Get-ProductionFunction $lower $name))
}
if ($supportNames -contains 'llvm_mir_id_map_dispose') {
    $pieces.Add('fn probe_dispose(map: rawptr) { llvm_mir_id_map_dispose(map); }')
} else {
    $pieces.Add('fn probe_dispose(map: rawptr) { free(map); }')
}
$pieces.Add([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'probe.vyx')))
$probePath = Join-Path $runDir 'lower_maps.vyx'
[IO.File]::WriteAllText($probePath, ($pieces -join "`n`n"))
$executable = Join-Path $runDir 'lower_maps.exe'
$oldLlvm = $env:LLVM_ROOT
try {
    $env:LLVM_ROOT = Join-Path $repo 'clang'
    & $compilerPath --src=file $probePath --emit=exe -o $executable 2>&1 |
        Tee-Object -FilePath (Join-Path $runDir 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "Probe compilation failed: $LASTEXITCODE; $runDir" }
    if ($ExpectLegacyResetFailure) { $probeArgs = @('baseline') } else { $probeArgs = @() }
    & $executable @probeArgs 2>&1 | Tee-Object -FilePath (Join-Path $runDir 'run.log')
    $status = $LASTEXITCODE
    $expected = 0
    if ($ExpectLegacyResetFailure) { $expected = 17 }
    "compiler=$compilerPath`nsource_ref=$SourceRef`nlower_sha256=$sourceHash`nexit=$status`nexpected=$expected" |
        Set-Content -LiteralPath (Join-Path $runDir 'result.txt')
    if ($status -ne $expected) { throw "Probe exit $status, expected $expected; $runDir" }
    if (-not $ExpectLegacyResetFailure -and $env:OS -eq 'Windows_NT') {
        $launcher = Join-Path $runDir 'oom_launcher.exe'
        & (Join-Path $repo 'clang/bin/clang++.exe') -std=c++20 -O2 (Join-Path $PSScriptRoot 'oom_launcher.cpp') -o $launcher
        if ($LASTEXITCODE -ne 0) { throw "OOM launcher compilation failed: $LASTEXITCODE" }
        foreach ($mode in @('oom', 'oom-create')) {
            $oomLog = Join-Path $runDir "$mode.log"
            & $launcher $executable $mode 2>&1 | Tee-Object -FilePath $oomLog
            if ($LASTEXITCODE -ne 0) { throw "OOM failure handling failed: $LASTEXITCODE; $runDir" }
            if (-not (Select-String -Path $oomLog -SimpleMatch 'LLVM MIR ID map allocation failed' -Quiet)) {
                throw "OOM diagnostic missing; $runDir"
            }
        }
    }
    Write-Output "PASS: lower map helpers; artifacts: $runDir"
} finally {
    $env:LLVM_ROOT = $oldLlvm
}

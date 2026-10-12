param([Parameter(Mandatory)][string]$Compiler, [string]$BaselineRevision = 'ee4d3ffb')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/names-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
function Extract([string]$text, [string]$name, [bool]$method = $false) {
    $indent = $(if ($method) { '    ' } else { '' })
    $start = [regex]::Match($text, '(?m)^' + $indent + '(?:public )?fn ' + [regex]::Escape($name) + '\(')
    if (!$start.Success) { throw "Missing $name" }
    $tail = $text.Substring($start.Index)
    $end = [regex]::Match($tail, '(?m)^' + $indent + '}\s*$')
    if (!$end.Success) { throw "Unclosed $name" }
    return $tail.Substring(0, $end.Index + $end.Length) + "`n"
}
$methodsPath = 'bootstrap_compiler/src/hir/builder/resolution/method_lookup.vyx'
$methods = [IO.File]::ReadAllText((Join-Path $repo $methodsPath))
$baseline = (& git -C $repo show ($BaselineRevision + ':' + $methodsPath)) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Cannot read baseline' }
$helpers = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/hir/builder/support/text_helpers.vyx'))
$policy = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/sema/policy.vyx'))
$text = @'
use std.collections;
extern "C" {
    fn vyx_alloc_string_len(n: i64) -> rawptr;
    fn memcpy(dest: rawptr, src: rawptr, n: i64) -> rawptr;
    fn vyx_rt_process_private_bytes() -> i64;
}
'@
foreach ($name in @('policy_mangle_hex_nibble','policy_mangle_hi_nibble','policy_mangle_lo_nibble','policy_ptr_offset','policy_mangle_part','policy_operator_legacy_method_name','policy_operator_symbol_method_name')) { $text += Extract $policy $name }
foreach ($name in @('hir_builder_string_contains','hir_builder_string_contains_parts','hir_builder_symbol_segment_for_name')) { $text += Extract $helpers $name }
$text += "`nclass NameMatcher { public marker: i32; }`nimpl NameMatcher {`n"
foreach ($name in @('function_name_matches_method_direct','function_name_matches_method')) { $text += Extract $methods $name $true }
foreach ($name in @('function_name_matches_method_direct','function_name_matches_method')) {
    $body = Extract $baseline $name $true
    $body = $body.Replace('function_name_matches_method_direct(', 'baseline_function_name_matches_method_direct(').Replace('function_name_matches_method(', 'baseline_function_name_matches_method(')
    $text += $body
}
$text += "}`n" + [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'names.vyx'))
[IO.File]::WriteAllText((Join-Path $run 'names.vyx'), $text, [Text.UTF8Encoding]::new($false))
foreach ($file in @('vyx_compiler_backend.dll','tbb12.dll')) { Copy-Item -LiteralPath (Join-Path (Split-Path $Compiler) $file) -Destination $run }
foreach ($opt in @(0,2)) {
    $exe = Join-Path $run "names-O$opt.exe"
    & $Compiler --src=file (Join-Path $run 'names.vyx') --emit=exe "-O$opt" -L (Split-Path $Compiler) -l vyx_compiler_backend -l vyx_runtime -l synchronization -l tbb12 -o $exe *> (Join-Path $run "compile-O$opt.log")
    if ($LASTEXITCODE -ne 0) { throw "Names O$opt compilation failed" }
    & $exe | Tee-Object (Join-Path $run "run-O$opt.log")
    if ($LASTEXITCODE -ne 0) { throw "Names O$opt assertions failed" }
}
@{ compiler=$Compiler; sha256=(Get-FileHash $Compiler -Algorithm SHA256).Hash; baseline=$BaselineRevision; result='PASS' } | ConvertTo-Json | Set-Content (Join-Path $run 'result.json')
Write-Output "Name matching O0/O2 PASS: $run"

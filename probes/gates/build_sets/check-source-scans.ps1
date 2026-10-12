param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler=(Resolve-Path -LiteralPath $Compiler).Path
$run=Join-Path $PSScriptRoot ('.runs/scan-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
function Read-Function([string]$text,[string]$name) {
    $match=[regex]::Match($text,'(?m)^(?:public )?fn '+[regex]::Escape($name)+'\(')
    if (!$match.Success) { throw "Missing production function $name" }
    $tail=$text.Substring($match.Index)
    $end=[regex]::Match($tail,'(?m)^}\s*$')
    if (!$end.Success) { throw "Unclosed function $name" }
    return $tail.Substring(0,$end.Index+$end.Length)+"`n"
}
$source=[IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/project/build_system.vyx'))
$model=[IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/project/build_source_index.vyx'))
$model=[regex]::Replace($model,'(?m)^(module|use) [^;]+;\r?\n','')
$sets=[IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/project/build_sets.vyx'))
$sets=[regex]::Replace($sets,'(?m)^(module|use) [^;]+;\r?\n','')
$start=$source.IndexOf('class BuildSourceScanText {')
$end=$source.IndexOf('fn build_source_text_for_scan(', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Missing cache storage declarations' }
$header=@'
use std.collections;
use std.fs;
use std.string;
extern "C" {
    fn getenv(name: rawptr) -> rawptr;
    fn vyx_bootstrap_platform() -> i32;
    fn vyx_bootstrap_abs_path(path: rawptr) -> rawptr;
    fn vyx_string_contains(hay: rawptr, hay_len: u64, needle: rawptr, needle_len: u64) -> i32;
    fn vyx_bootstrap_file_mtime(path: rawptr) -> i64;
    fn vyx_rt_process_private_bytes() -> i64;
}
fn build_file_mtime(path: string) -> i64 { return vyx_bootstrap_file_mtime(path.c_str()); }
fn build_is_windows() -> bool { return vyx_bootstrap_platform() == 1; }
fn build_env_fallback(name: string) -> string { return ""; }
var g_build_module_name_cache: string = "";
var g_build_module_identity_cache: string = "";
'@
$text=$header+"`n"+$model+"`n"+$sets+"`n"+$source.Substring($start,$end-$start)
foreach($name in @('build_is_ident_char','build_env_value','build_expand_env_spec','build_expand_env','build_host_path','build_ascii_lower','build_path_stem','build_safe_obj_name','build_quote_arg','build_contains','build_text_starts_with_at','build_source_contains_code_token','build_source_code_token_offset','build_source_uses_std_module','build_source_text_for_scan','build_source_scan_cache_key','build_source_scan_cache_get','build_line_at','build_line_count','build_stable_string','build_resolve_dci_import','build_abs_norm_path','build_is_abs_path','build_dirname','build_join_path','build_dci_import_for_group')) { $text+=Read-Function $source $name }
$text+='fn build_path_sep() -> string { if (build_is_windows()) { return "\\"; } return "/"; }'+"`n"
foreach($name in @('build_module_name_cache_get','build_module_identity_cache_get','build_norm_path','build_append_unique_line')) { $text+=Read-Function $source $name }
foreach($name in @('build_identifier_boundary_before','build_identifier_boundary_after','build_matching_angle_end','build_generic_call_roots_for_names_in_text','build_collect_generic_call_spans','build_generic_call_roots_for_source')) { $text+=Read-Function $source $name }
$contract=[IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/dci/dci_dcib.vyx'))
foreach($name in @('dci_path_list_count','dci_path_list_at','dci_path_list_contains','dci_path_list_append')) { $text+=Read-Function $contract $name }
$text+=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'generic_root_probe.vyx'))
$text+=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'source_scan_probe.vyx'))
[IO.File]::WriteAllText((Join-Path $run 'scan.vyx'),$text,[Text.UTF8Encoding]::new($false))
foreach($dll in @('vyx_compiler_backend.dll','tbb12.dll')) { Copy-Item -LiteralPath (Join-Path (Split-Path $Compiler) $dll) -Destination $run }
Push-Location $repo
try {
    $env:VYX_SCAN_TEST_VALUE='fixture-value'
    $env:VYX_SCAN_TEST_EMPTY=''
    $env:VYX_SCAN_TEST_LITERAL='$not-expanded'
    foreach($opt in @('0','2')) {
        & $Compiler --src=file (Join-Path $run 'scan.vyx') --emit=exe -o (Join-Path $run "scan-O$opt.exe") "-O$opt" -L (Split-Path $Compiler) -L (Join-Path $repo 'clang/lib') -l vyx_compiler_backend -l vyx_runtime -l synchronization -l tbb12 *> (Join-Path $run "compile-O$opt.log")
        if($LASTEXITCODE -ne 0) { throw "Scan cache compile O$opt failed: $run" }
        Push-Location $run
        try { & (Join-Path $run "scan-O$opt.exe") | Tee-Object "run-O$opt.log" }
        finally { Pop-Location }
        if($LASTEXITCODE -ne 0) { throw "Scan cache probe O$opt failed: $LASTEXITCODE" }
    }
} finally { Pop-Location }
Write-Output "Compiler=$Compiler; SHA256=$((Get-FileHash $Compiler).Hash); evidence=$run"

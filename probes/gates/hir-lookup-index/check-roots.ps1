param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/roots-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
function Extract([string]$text, [string]$name) {
    $start = [regex]::Match($text, '(?m)^(?:public )?fn ' + [regex]::Escape($name) + '\(')
    if (!$start.Success) { throw "Missing $name" }
    $tail = $text.Substring($start.Index)
    $end = [regex]::Match($tail, '(?m)^}\s*$')
    if (!$end.Success) { throw "Unclosed $name" }
    return $tail.Substring(0, $end.Index + $end.Length) + "`n"
}
$helpers = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/hir/builder/support/text_helpers.vyx'))
$policy = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/sema/policy.vyx'))
$text = @'
use std.collections;
extern "C" {
    fn malloc(n: u64) -> rawptr;
    fn free(p: rawptr);
    fn memcpy(dst: rawptr, src: rawptr, n: u64) -> rawptr;
    fn strncmp(a: rawptr, b: rawptr, n: i64) -> i32;
    fn vyx_rt_process_private_bytes() -> i64;
}
fn ptr_offset(p: rawptr, n: i64) -> rawptr { return (p as i64 + n) as rawptr; }
'@
foreach ($name in @('policy_generic_turbofish_start','policy_generic_plain_angle_start','policy_generic_call_args')) { $text += "@[borrow_args]`n" + (Extract $policy $name) }
foreach ($name in @('hir_builder_string_contains','hir_builder_root_name_contains','hir_builder_generic_root_names')) { $text += Extract $helpers $name }
$text += [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'roots.vyx'))
[IO.File]::WriteAllText((Join-Path $run 'roots.vyx'), $text, [Text.UTF8Encoding]::new($false))
foreach ($file in @('vyx_compiler_backend.dll','tbb12.dll')) { Copy-Item -LiteralPath (Join-Path (Split-Path $Compiler) $file) -Destination $run }
foreach ($opt in @(0,2)) {
    $exe = Join-Path $run "roots-O$opt.exe"
    & $Compiler --src=file (Join-Path $run 'roots.vyx') --emit=exe "-O$opt" -L (Split-Path $Compiler) -l vyx_compiler_backend -l vyx_runtime -l synchronization -l tbb12 -o $exe *> (Join-Path $run "compile-O$opt.log")
    if ($LASTEXITCODE -ne 0) { throw "Roots O$opt compilation failed" }
    & $exe | Tee-Object (Join-Path $run "run-O$opt.log")
    if ($LASTEXITCODE -ne 0) { throw "Roots O$opt assertions failed" }
}
$library = @'
fn identity<T>(x: T) -> T { return x; }
fn direct() -> i32 { return 9; }
fn unused() -> i32 { return 8; }
'@
$path = Join-Path $run 'library.vyx'
[IO.File]::WriteAllText($path, $library, [Text.UTF8Encoding]::new($false))
$ir = Join-Path $run 'library.ll'
& $Compiler --src=file $path --emit=ir --no-process-entry --export-top-level-roots --export-root-names 'direct|identity::<i32>' -o $ir *> (Join-Path $run 'library.log')
if ($LASTEXITCODE -ne 0) { throw 'Generic root IR failed' }
$body = [IO.File]::ReadAllText($ir)
foreach ($name in @('identity','direct')) {
    if ($body -notmatch ('(?m)^define [^\r\n]*@[^\r\n]*' + $name)) { throw "Missing definition: $name" }
}
if ($body -match '(?m)^define [^\r\n]*@[^\r\n]*unused') { throw 'Unselected root was emitted' }
@{ compiler=$Compiler; sha256=(Get-FileHash $Compiler -Algorithm SHA256).Hash; result='PASS' } | ConvertTo-Json | Set-Content (Join-Path $run 'result.json')
Write-Output "Export roots O0/O2 and generic IR PASS: $run"

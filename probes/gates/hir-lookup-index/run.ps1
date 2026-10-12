param([Parameter(Mandatory=$true)][string]$Compiler)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
function Read-Function([string]$path, [string]$name) {
    $source = [IO.File]::ReadAllText((Join-Path $repo $path))
    $start = [regex]::Match($source, '(?m)^(?:public )?fn ' + [regex]::Escape($name) + '\(')
    if (-not $start.Success) { throw "Missing production helper $name" }
    $tail = $source.Substring($start.Index)
    $next = [regex]::Match($tail, '(?m)^}\s*$')
    if (-not $next.Success) { throw "Missing end of $name" }
    return $tail.Substring(0, $next.Index + $next.Length) + "`n"
}
$support = @'
extern "C" {
    fn malloc(size: u64) -> rawptr;
    fn free(p: rawptr);
    fn memcpy(dest: rawptr, src: rawptr, size: u64) -> rawptr;
    fn memset(dest: rawptr, value: i32, size: u64) -> rawptr;
    fn strncmp(a: rawptr, b: rawptr, count: i64) -> i32;
}
fn hir_profile_malloc(size: u64) -> rawptr { return malloc(size); }
fn hir_id_invalid() -> i32 { return 0; }
fn ast_dup_c_string(s: string) -> rawptr {
    let p = malloc((s.len + 1) as u64);
    if (p != null) { memcpy(p, s.c_str(), (s.len + 1) as u64); }
    return p;
}
'@
foreach ($name in @('ast_read_rawptr','ast_write_rawptr','ast_read_i32','ast_write_i32','ast_read_i64','ast_write_i64')) {
    $support += Read-Function 'bootstrap_compiler/src/core/syntax/ast.vyx' $name
}
foreach ($name in @('hir_builder_string_hash_i32','hir_builder_hash_slot','hir_builder_alloc_ptr_buckets')) {
    $support += Read-Function 'bootstrap_compiler/src/hir/builder/support/name_maps.vyx' $name
}
$support += Read-Function 'bootstrap_compiler/src/hir/builder/support/text_helpers.vyx' 'hir_builder_c_string_equals'
$map = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/hir/builder/support/instance_maps.vyx'))
$map = [regex]::Replace($map, '(?m)^(module|use) [^;]+;\s*$', '')
$source = Join-Path $run 'maps.vyx'
[IO.File]::WriteAllText($source, $support + "`n" + $map + "`n" + [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'probe.vyx')), $utf8)
$savedUnits = $env:VYX_CODEGEN_UNITS
try {
    $env:VYX_CODEGEN_UNITS = '1'
    foreach ($opt in @(0, 2)) {
        $exe = Join-Path $run "maps-O$opt.exe"
        & $Compiler --src=file $source --emit=exe "-O$opt" -o $exe *> (Join-Path $run "build-O$opt.log")
        if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $run "build-O$opt.log"); throw "Compile O$opt failed" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "Map probe O$opt failed: $LASTEXITCODE" }
    }
} finally { $env:VYX_CODEGEN_UNITS = $savedUnits }
@{ compiler=$Compiler; sha256=(Get-FileHash $Compiler -Algorithm SHA256).Hash; result='PASS' } |
    ConvertTo-Json | Set-Content (Join-Path $run 'result.json')
Write-Host "Artifacts: $run"

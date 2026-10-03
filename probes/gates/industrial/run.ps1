# Industrial four-axis probes: pointer recast, generic infer/alias,
# layout inheritance, first-class dyn.
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$Boot = Join-Path $Root "bootstrap_compiler\out\boot.exe"
$OutDir = Join-Path $Root "bootstrap_compiler\out"
$Llvm = Join-Path $Root "clang"
if (Test-Path $Llvm) {
    $env:LLVM_ROOT = (Resolve-Path $Llvm).Path
    $env:PATH = "$OutDir;$env:LLVM_ROOT\bin;$env:PATH"
} else {
    $env:PATH = "$OutDir;$env:PATH"
}

if (-not (Test-Path $Boot)) {
    Write-Host "industrial FAIL: missing $Boot"
    exit 1
}

$fail = 0
$tmp = Join-Path $env:TEMP "vyx-industrial"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function Run-Ok([string]$name) {
    $src = Join-Path $PSScriptRoot "$name.vyx"
    $exe = Join-Path $tmp "$name.exe"
    $log = Join-Path $tmp "$name.log"
    & $Boot --src=file $src --emit=exe -o $exe *> $log
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: $name did not compile"
        Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 30
        $script:fail++
        return
    }
    & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: $name exit=$LASTEXITCODE"
        $script:fail++
        return
    }
    Write-Host "OK: $name"
}

function Run-Err([string]$name, [string]$needle) {
    $src = Join-Path $PSScriptRoot "$name.vyx"
    $ll = Join-Path $tmp "$name.ll"
    $log = Join-Path $tmp "$name.err.log"
    & $Boot --src=file $src --emit=ir -o $ll *> $log
    $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
    if ($text -match $needle) {
        Write-Host "OK: $name"
    } else {
        Write-Host "FAIL: $name expected /$needle/"
        if ($text) { $text.Split("`n") | Select-Object -Last 30 | ForEach-Object { Write-Host $_ } }
        $script:fail++
    }
}

Run-Ok "ptr_cast"
Run-Ok "ptr_arrow"
Run-Ok "ptr_arith"
Run-Ok "ptr_diff"
Run-Ok "ptr_rawptr"
Run-Ok "ptr_null"
Run-Ok "ptr_recast"
Run-Ok "nested_id"
Run-Ok "alias_generic"
Run-Ok "int_width"
Run-Ok "inherit_fields"
Run-Ok "inherit_chain"
Run-Ok "dyn_param"
Run-Ok "dyn_ref"
Run-Ok "dyn_let"
Run-Ok "dyn_return"
Run-Ok "dyn_field"
Run-Ok "dyn_assign"
Run-Ok "mut_dyn"
Run-Ok "supertrait"
Run-Ok "iface_default"
Run-Ok "multi_trait"
Run-Ok "vec_dyn"
Run-Ok "dict_bounds"
Run-Ok "box_dyn"
Run-Ok "box_dyn_make"
Run-Ok "dict_box_dyn"
Run-Ok "assoc_const"
Run-Ok "col_cstr"
Run-Ok "inherit_override"
Run-Ok "inherit_virtual_ref"
Run-Ok "inherit_multi"
Run-Err "inherit_diamond" "diamond"
Run-Err "inherit_diamond_linear" "diamond"
Run-Ok "class_colon_iface"
Run-Err "override_orphan" "override"
Run-Err "int_narrow_err" "type mismatch"
Run-Err "int_sign_err" "type mismatch"
Run-Err "int_unarrow_err" "type mismatch"
Run-Err "rawptr_value_err" "type mismatch"
Run-Err "ptr_borrow_arith_err" "type mismatch"

if ($fail -ne 0) {
    Write-Host "industrial FAIL: $fail probe(s)"
    exit 1
}
Write-Host "industrial E2E: OK"
exit 0

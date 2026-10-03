# Option/Result/enum storage size: Vec memcpy must copy tag+payload.
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$Boot = Join-Path $Root "bootstrap_compiler\out\boot.exe"
$OutDir = Join-Path $Root "bootstrap_compiler\out"
$env:PATH = "$OutDir;$env:PATH"
if (-not (Test-Path $Boot)) {
    Write-Host "option_vec FAIL: missing $Boot"
    exit 1
}
$tmp = Join-Path $env:TEMP "vyx-option-vec-gate"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$fail = 0
function Run-Ok([string]$name) {
    $src = Join-Path $PSScriptRoot "$name.vyx"
    $exe = Join-Path $tmp "$name.exe"
    & $Boot --src=file $src --emit=exe -o $exe -L $OutDir -l vyx_runtime
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: $name did not compile"
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
Run-Ok "standalone"
Run-Ok "payload"
Run-Ok "memcpy"
Run-Ok "get_ptr"
Run-Ok "closure_vec"
if ($fail -ne 0) {
    Write-Host "option_vec FAIL $fail"
    exit 1
}
Write-Host "option_vec OK"
exit 0

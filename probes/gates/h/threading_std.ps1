# OS threads + oneTBB pool: compile and run under boot.
$ErrorActionPreference = "Stop"
$OutputEncoding = [Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$Boot = Join-Path $Root "bootstrap_compiler\out\boot.exe"
$OutDir = Join-Path $Root "bootstrap_compiler\out"
$Clang = Join-Path $Root "clang"
$env:LLVM_ROOT = $Clang
$env:PATH = "$Clang\bin;$OutDir;$env:PATH"
$env:Path = $env:PATH

if (-not (Test-Path -LiteralPath $Boot)) {
    Write-Host "FAIL: missing $Boot"
    exit 1
}

$fail = 0
$src = Join-Path $PSScriptRoot "_threading_probe.vyx"
$exe = Join-Path $OutDir "_threading_probe.exe"

& $Boot --src=file $src --emit=exe -o $exe -L $OutDir -l vyx_runtime
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: boot compile threading probe"
    $fail++
} else {
    & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: threading probe exit=$LASTEXITCODE"
        $fail++
    } else {
        Write-Host "OK: threading+tbb ran"
    }
}

if ($fail -ne 0) {
    Write-Host "threading_std FAIL"
    exit 1
}
Write-Host "OK: threading_std"
exit 0

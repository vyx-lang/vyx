$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$LLVM = Join-Path $Root "clang"
$env:LLVM_ROOT = $LLVM
$env:PATH = "$LLVM\bin;$Root\bootstrap_compiler\out;$env:PATH"
$Boot = Join-Path $Root "bootstrap_compiler\out\boot.exe"
if (-not (Test-Path $Boot)) {
    throw "boot.exe missing; rebuild bootstrap_compiler/out/boot.exe"
}

$Probe = $PSScriptRoot
$Json = Join-Path $Probe "shared.dci"
$Dcib = Join-Path $Probe "shared.dcib"
$Src = Join-Path $Probe "shared.vyx"
$Ir = Join-Path $Probe "shared.ll"

python (Join-Path $Root "tools\dci\dcib.py") encode $Json $Dcib
if ($LASTEXITCODE -ne 0) { throw "dcib encode failed" }

& $Boot --src=file $Src --dci $Dcib --emit=ir -o $Ir
if ($LASTEXITCODE -ne 0) { throw "boot --emit=ir failed" }
if (-not (Test-Path $Ir)) { throw "missing shared.ll" }

$IrText = Get-Content -Raw $Ir
if ($IrText -match "error:") { throw "IR contains error:" }
if ($IrText -notmatch "__CxxFrameHandler3" -and $IrText -notmatch "__gxx_personality_v0") {
    throw "IR missing personality routine"
}
if ($IrText -notmatch "uwtable") { throw "IR missing uwtable" }

$Invoke = [regex]::Matches($IrText, 'invoke[^\n]*@contract_probe\(')
if ($Invoke.Count -lt 1) { throw "IR missing invoke @contract_probe" }
if ($IrText -match '(?m)^  call [^\n]*@contract_probe\(') {
    throw "shared_abi callee was lowered as call instead of invoke"
}

if ($IrText -match "__CxxFrameHandler3") {
    if ($IrText -notmatch "cleanuppad") { throw "MSVC shared_abi IR missing cleanuppad" }
    if ($IrText -notmatch "cleanupret") { throw "MSVC shared_abi IR missing cleanupret" }
    if ($IrText -match "landingpad") { throw "MSVC shared_abi IR used Itanium landingpad" }
} else {
    if ($IrText -notmatch "landingpad") { throw "Itanium shared_abi IR missing landingpad" }
    if ($IrText -notmatch "resume") { throw "Itanium shared_abi IR missing resume" }
}

if ($IrText -match "__cxa_begin_catch" -or $IrText -match "__cxa_throw" -or $IrText -match "__cxa_allocate_exception") {
    throw "shared_abi cleanup pad must not synthesize __cxa_* catch/throw"
}

Write-Output "dci-landingpad shared OK"
Write-Output $Invoke[0].Value

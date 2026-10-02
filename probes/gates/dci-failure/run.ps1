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
$Contracts = Join-Path $Probe "contracts"
$Generated = Join-Path $Probe "generated"
New-Item -ItemType Directory -Force -Path $Contracts, $Generated | Out-Null

$Header = Join-Path $Root "tools\dci\bench\fixtures\abi_fixtures.hpp"
$Dcib = Join-Path $Contracts "abi_fixtures.dcib"
$Stub = Join-Path $Generated "translate_unwind.cpp"
$Ir = Join-Path $Probe "checked_divide.ll"

python (Join-Path $Root "tools\dci\dci.py") adapter --language cpp $Header `
    -o $Dcib --stub-out $Stub --triplet windows_x64
if ($LASTEXITCODE -ne 0) { throw "adapter failed" }

& $Boot --src=file (Join-Path $Probe "checked_divide.vyx") --dci $Dcib --emit=ir -o $Ir
if ($LASTEXITCODE -ne 0) { throw "boot --emit=ir failed" }
$IrText = Get-Content -Raw $Ir
if ($IrText -notmatch "dci_tr_") { throw "IR missing translator dci_tr_ symbol" }
if ($IrText -match "__CxxFrameHandler3") { throw "IR registered __CxxFrameHandler3" }
if ($IrText -match "\?checked_divide@abi@@YAHHH@Z") { throw "IR calls original throwing symbol" }

Set-Location $Probe
& $Boot build --target dci_failure_probe -j 4
if ($LASTEXITCODE -ne 0) { throw "probe exe build failed" }
$Exe = Join-Path $Probe "target\dci_failure_probe.exe"
if (-not (Test-Path $Exe)) { $Exe = Join-Path $Probe "out\dci_failure_probe.exe" }
& $Exe
if ($LASTEXITCODE -ne 0) { throw "probe exit=$LASTEXITCODE" }
Write-Output "dci-failure OK: exit=0"

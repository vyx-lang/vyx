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
$Json = Join-Path $Probe "packed.dci"
$Dcib = Join-Path $Probe "packed.dcib"
$Src = Join-Path $Probe "packed.vyx"
$Ir = Join-Path $Probe "packed.ll"

python (Join-Path $Root "tools\dci\dcib.py") encode $Json $Dcib
if ($LASTEXITCODE -ne 0) { throw "dcib encode failed" }

& $Boot --src=file $Src --dci $Dcib --emit=ir -o $Ir
if ($LASTEXITCODE -ne 0) { throw "boot --emit=ir failed" }
if (-not (Test-Path $Ir)) { throw "missing packed.ll" }

$IrText = Get-Content -Raw $Ir
if ($IrText -match "error:") { throw "IR contains error:" }

$Decl = [regex]::Matches($IrText, '(?m)^declare[^\n]*@packed_sum\([^\n]*\)')
if ($Decl.Count -lt 1) { throw "IR missing declare @packed_sum" }
$DeclText = $Decl[0].Value
$InallocaOnDecl = ([regex]::Matches($DeclText, 'inalloca\(')).Count
if ($InallocaOnDecl -ne 1) {
    throw "declare @packed_sum must have exactly 1 inalloca param, got $InallocaOnDecl : $DeclText"
}
if ($DeclText -notmatch 'inalloca\(%dci\.inalloca\.packed_sum\)') {
    throw "declare @packed_sum inalloca pointee is not the packed frame: $DeclText"
}

if ($IrText -notmatch 'alloca inalloca %dci\.inalloca\.packed_sum') {
    throw "IR missing alloca inalloca of packed frame"
}
if ($IrText -notmatch 'getelementptr %dci\.inalloca\.packed_sum, ptr [^,]+, i32 0, i32 0') {
    throw "IR missing GEP of packed field 0"
}
if ($IrText -notmatch 'getelementptr %dci\.inalloca\.packed_sum, ptr [^,]+, i32 0, i32 1') {
    throw "IR missing GEP of packed field 1"
}

$Call = [regex]::Matches($IrText, 'call[^\n]*@packed_sum\([^\n]*\)')
if ($Call.Count -lt 1) { throw "IR missing call @packed_sum" }
$CallText = $Call[0].Value
$InallocaOnCall = ([regex]::Matches($CallText, 'inalloca\(')).Count
if ($InallocaOnCall -ne 1) {
    throw "call @packed_sum must have exactly 1 inalloca arg, got $InallocaOnCall : $CallText"
}
if ($CallText -notmatch 'inalloca\(%dci\.inalloca\.packed_sum\)') {
    throw "call @packed_sum inalloca pointee is not the packed frame: $CallText"
}

$Dup = [regex]::Matches($IrText, '%dci\.inalloca\.packed_sum\.\d+')
if ($Dup.Count -gt 0) {
    throw "packed frame type was uniqued instead of reused: $($Dup[0].Value)"
}

Write-Output "dci-inalloca packed OK"
Write-Output $DeclText
Write-Output $CallText

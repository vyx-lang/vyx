param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/generic-direct-return-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force -Path $run, (Join-Path $run 'interfaces') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'GenericProducer.vyx') (Join-Path $run 'GenericProducer.vyx')
Copy-Item (Join-Path $PSScriptRoot 'GenericConsumer.vyx') (Join-Path $run 'GenericConsumer.vyx')
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = (Join-Path $repo 'clang/bin') + ';' + $env:PATH
Push-Location $run
try {
    & $Compiler --src=file GenericProducer.vyx --emit=vyi --vyi-shallow -o interfaces/ProducerFree.vyi *> producer.log
    if ($LASTEXITCODE -ne 0) { throw 'generic producer failed' }
    & $Compiler --src=file GenericConsumer.vyx --interface-root interfaces --emit=exe -o consumer.exe *> consumer.log
    if ($LASTEXITCODE -ne 0) { Get-Content consumer.log -Tail 80; throw 'generic consumer failed' }
    & ./consumer.exe *> consumer.run.log
    if ($LASTEXITCODE -ne 0) { throw "generic consumer returned $LASTEXITCODE" }
    Write-Host "cross-module generic direct return: OK ($run)"
} finally { Pop-Location }

param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force -Path $run, (Join-Path $run 'interfaces') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'Producer.vyx') (Join-Path $run 'Producer.vyx')
Copy-Item (Join-Path $PSScriptRoot 'Consumer.vyx') (Join-Path $run 'Consumer.vyx')
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = (Join-Path $repo 'clang/bin') + ';' + $env:PATH
Push-Location $run
try {
    & $Compiler --src=file Producer.vyx --emit=vyi --vyi-shallow -o interfaces/Cross.GlobalEnum.vyi *> producer.log
    if ($LASTEXITCODE -ne 0) { throw 'cross-module producer failed' }
    & $Compiler --src=file Consumer.vyx --interface-root interfaces --emit=exe -o consumer.exe *> consumer.log
    if ($LASTEXITCODE -ne 0) { Get-Content consumer.log -Tail 80; throw 'cross-module consumer failed' }
    & ./consumer.exe *> consumer.run.log
    if ($LASTEXITCODE -ne 0) { throw "cross-module consumer returned $LASTEXITCODE" }
    Write-Host "cross-module globals/enums: OK ($run)"
} finally { Pop-Location }

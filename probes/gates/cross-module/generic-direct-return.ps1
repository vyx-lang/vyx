param([string]$Compiler = '', [string]$OutputDir = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$run = if ($OutputDir) { [IO.Path]::GetFullPath($OutputDir) } else { Join-Path $PSScriptRoot ('.runs/generic-direct-return-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
if (Test-Path -LiteralPath $run) { throw "Refusing to overwrite existing output: $run" }
New-Item -ItemType Directory -Force -Path $run, (Join-Path $run 'interfaces') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'GenericProducer.vyx') (Join-Path $run 'GenericProducer.vyx')
Copy-Item (Join-Path $PSScriptRoot 'GenericConsumer.vyx') (Join-Path $run 'GenericConsumer.vyx')
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = (Join-Path $repo 'clang/bin') + ';' + $env:PATH
$emitExit = $null
$consumerExit = $null
$runExit = $null
Push-Location $run
try {
    & $Compiler --src=file GenericProducer.vyx --emit=vyi --vyi-shallow -o interfaces/ProducerFree.vyi *> producer.log
    $emitExit = $LASTEXITCODE
    if ($emitExit -ne 0) { throw 'generic producer failed' }
    & $Compiler --src=file GenericConsumer.vyx --interface-root interfaces --emit=exe -o consumer.exe *> consumer.log
    $consumerExit = $LASTEXITCODE
    if ($consumerExit -ne 0) { Get-Content consumer.log -Tail 80; throw 'generic consumer failed' }
    & ./consumer.exe *> consumer.run.log
    $runExit = $LASTEXITCODE
    if ($runExit -ne 0) { throw "generic consumer returned $runExit" }
    Write-Host "cross-module generic direct return: OK ($run)"
} finally {
    [ordered]@{
        compiler = $Compiler
        compiler_sha256 = (Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash
        producer_interface_exit = $emitExit
        consumer_link_exit = $consumerExit
        consumer_run_exit = $runExit
        run = $run
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding utf8
    Pop-Location
}

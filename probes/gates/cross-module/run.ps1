param([string]$Compiler = '', [string]$OutputDir = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$run = if ($OutputDir) { [IO.Path]::GetFullPath($OutputDir) } else { Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
if (Test-Path -LiteralPath $run) { throw "Refusing to overwrite existing output: $run" }
New-Item -ItemType Directory -Force -Path $run, (Join-Path $run 'interfaces'), (Join-Path $run 'producer'), (Join-Path $run 'consumer') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'Producer.vyx') (Join-Path $run 'producer/Producer.vyx')
Copy-Item (Join-Path $PSScriptRoot 'Consumer.vyx') (Join-Path $run 'consumer/Consumer.vyx')
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = (Join-Path $repo 'clang/bin') + ';' + $env:PATH
$emitExit = $null
$objectExit = $null
$consumerExit = $null
$runExit = $null
Push-Location $run
try {
    & $Compiler --src=file producer/Producer.vyx --emit=vyi --vyi-shallow -o interfaces/Cross.GlobalEnum.vyi *> producer.log
    $emitExit = $LASTEXITCODE
    if ($emitExit -ne 0) { throw 'cross-module producer interface failed' }
    & $Compiler --src=file producer/Producer.vyx --emit=obj --export-top-level-roots -o producer/Producer.obj *> producer-object.log
    $objectExit = $LASTEXITCODE
    if ($objectExit -ne 0) { throw 'cross-module producer object failed' }
    Push-Location consumer
    try {
        & $Compiler --src=file Consumer.vyx --interface-root ../interfaces --emit=exe --link-obj ../producer/Producer.obj -o consumer.exe *> ../consumer.log
        $consumerExit = $LASTEXITCODE
        if ($consumerExit -ne 0) { Get-Content ../consumer.log -Tail 80; throw 'cross-module consumer failed' }
        & ./consumer.exe *> ../consumer.run.log
        $runExit = $LASTEXITCODE
        if ($runExit -ne 0) { throw "cross-module consumer returned $runExit (logs: $run)" }
    } finally { Pop-Location }
    Write-Host "cross-module globals/enums: OK ($run)"
} finally {
    [ordered]@{
        compiler = $Compiler
        compiler_sha256 = (Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash
        producer_interface_exit = $emitExit
        producer_object_exit = $objectExit
        consumer_link_exit = $consumerExit
        consumer_run_exit = $runExit
        run = $run
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding utf8
    Pop-Location
}

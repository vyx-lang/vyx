param(
    [string]$BootstrapCompiler = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

Push-Location $projectRoot
try {
    Remove-Item -Force -ErrorAction SilentlyContinue run_trace.txt, run_args_trace.txt, run_direct_args_trace.txt
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue .cache, target

    & $BootstrapCompiler run run_scripts_smoke:hello
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $tracePath = Join-Path $projectRoot "run_trace.txt"
    if (-not (Test-Path $tracePath)) {
        Write-Error "expected trace file not found"
    }

    $trace = (Get-Content -Raw -LiteralPath $tracePath).Replace("`r", "").TrimEnd()
    if ($trace -ne "run-script-ok") {
        Write-Error "unexpected trace content: $trace"
    }

    Remove-Item -Force -ErrorAction SilentlyContinue run_trace.txt
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue .cache, target

    & $BootstrapCompiler run run_scripts_smoke:hello_args
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $argTracePath = Join-Path $projectRoot "run_args_trace.txt"
    if (-not (Test-Path $argTracePath)) {
        Write-Error "expected script args trace file not found"
    }

    $argTrace = (Get-Content -Raw -LiteralPath $argTracePath).Replace("`r", "").TrimEnd()
    if ($argTrace -ne "run-script-args-ok") {
        Write-Error "unexpected script args trace content: $argTrace"
    }

    Remove-Item -Force -ErrorAction SilentlyContinue run_args_trace.txt
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue .cache, target

    & $BootstrapCompiler --src=file hello.vyx --run=aot -- --kind direct
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $directTracePath = Join-Path $projectRoot "run_direct_args_trace.txt"
    if (-not (Test-Path $directTracePath)) {
        Write-Error "expected direct args trace file not found"
    }

    $directTrace = (Get-Content -Raw -LiteralPath $directTracePath).Replace("`r", "").TrimEnd()
    if ($directTrace -ne "run-direct-args-ok") {
        Write-Error "unexpected direct args trace content: $directTrace"
    }

    Remove-Item -Force -ErrorAction SilentlyContinue run_direct_args_trace.txt
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue .cache, target
    exit 0
} finally {
    Remove-Item -Force -ErrorAction SilentlyContinue run_trace.txt, run_args_trace.txt, run_direct_args_trace.txt
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue .cache, target
    Pop-Location
}

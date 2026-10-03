param([string]$Runtime = '', [string]$Clang = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Runtime) { $Runtime = Join-Path $repo 'bootstrap_compiler/out/vyx_compiler_backend.dll' }
if (-not $Clang) { $Clang = Join-Path $repo 'clang/bin/clang++.exe' }
$Runtime = (Resolve-Path $Runtime).Path
$runDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$exe = Join-Path $runDir 'resources.exe'
& $Clang -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'probe.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "Probe compile failed: $LASTEXITCODE" }
$savedPath = $env:PATH
$savedNoJob = $env:VYX_BOOTSTRAP_DISABLE_JOB_OBJECT
try {
    $env:PATH = (Split-Path $Runtime) + ';' + $env:PATH
    $env:VYX_BOOTSTRAP_DISABLE_JOB_OBJECT = '0'
    Write-Host "runtime: $Runtime"
    Write-Host "sha256: $((Get-FileHash $Runtime -Algorithm SHA256).Hash)"
    & $exe $Runtime $runDir 2>&1 | Tee-Object (Join-Path $runDir 'tree.log')
    if ($LASTEXITCODE -ne 0) { throw "Resource tree probe failed: $LASTEXITCODE" }
    $env:VYX_BOOTSTRAP_DISABLE_JOB_OBJECT = '1'
    & $exe $Runtime $runDir --no-job 2>&1 | Tee-Object (Join-Path $runDir 'direct.log')
    if ($LASTEXITCODE -ne 0) { throw "Resource direct probe failed: $LASTEXITCODE" }
} finally {
    $env:PATH = $savedPath
    $env:VYX_BOOTSTRAP_DISABLE_JOB_OBJECT = $savedNoJob
}
Write-Host "artifacts: $runDir"

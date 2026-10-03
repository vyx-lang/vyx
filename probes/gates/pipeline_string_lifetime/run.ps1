param([string]$Runtime = '', [string]$Clang = '', [switch]$ExpectOldFailure)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Runtime) { $Runtime = Join-Path $repo 'bootstrap_compiler/out/vyx_compiler_backend.dll' }
if (-not $Clang) { $Clang = Join-Path $repo 'clang/bin/clang++.exe' }
$Runtime = (Resolve-Path $Runtime).Path
$runDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$exe = Join-Path $runDir 'string_cache.exe'
& $Clang -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'probe.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "Probe compile failed: $LASTEXITCODE" }
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path $Runtime) + ';' + $env:PATH
    Write-Host "runtime: $Runtime"
    Write-Host "sha256: $((Get-FileHash $Runtime -Algorithm SHA256).Hash)"
    & $exe $Runtime $runDir
    $rc = $LASTEXITCODE
    if ($ExpectOldFailure) {
        if ($rc -ne 10) { throw "Expected old stack-cache failure (10), got $rc" }
        Write-Host 'baseline stale pointer-length cache reproduced (exit 10)'
    } elseif ($rc -ne 0) {
        throw "String cache probe failed: $rc"
    }
} finally { $env:PATH = $savedPath }
Write-Host "artifacts: $runDir"

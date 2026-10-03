$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$root = (Resolve-Path (Join-Path $project '..\..\..')).Path
$boot = Join-Path $root 'bootstrap_compiler\out\boot.exe'
if (-not (Test-Path -LiteralPath $boot)) { throw "Build bootstrap_compiler/out/boot.exe first" }

Push-Location $project
try {
    & $boot build --target string_sret -j2
    if ($LASTEXITCODE -ne 0) { throw "string_sret build failed: $LASTEXITCODE" }
    & (Join-Path $project 'target\string_sret.exe')
    if ($LASTEXITCODE -ne 0) { throw "string_sret runtime failed: $LASTEXITCODE" }
    Write-Output 'string_sret: OK'
} finally {
    Pop-Location
}

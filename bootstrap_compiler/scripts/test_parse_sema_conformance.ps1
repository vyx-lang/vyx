# Run parse + sema conformance on bootstrap samples (fast gate).
# Full corpus: use -Full (all tests/**/*.vyx; slow).
#
# Usage:
#   pwsh bootstrap_compiler/scripts/test_parse_sema_conformance.ps1
#   pwsh .../test_parse_sema_conformance.ps1 -Full

param([switch]$Full)

$ErrorActionPreference = "Stop"
$bootstrapRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $bootstrapRoot
$bootstrapTests = Join-Path $repoRoot "tests/bootstrap"
$parseScript = Join-Path $bootstrapTests "parse_conformance/run.ps1"
$semaScript  = Join-Path $bootstrapTests "sema_conformance/run.ps1"
$recoveryScript = Join-Path $bootstrapTests "parser_recovery/run.ps1"
$registryFailureScript = Join-Path $bootstrapTests "module_registry_failure/run.ps1"

Write-Host "=== parser recovery ==="
& pwsh -NoProfile -File $recoveryScript
$p0 = $LASTEXITCODE

Write-Host "=== module registry failure propagation ==="
& pwsh -NoProfile -File $registryFailureScript
$pRegistry = $LASTEXITCODE

if (-not $Full) {
    Write-Host "=== parse conformance (samples) ==="
    & pwsh -NoProfile -File $parseScript -Samples
    $p1 = $LASTEXITCODE
    Write-Host "`n=== sema conformance (samples) ==="
    & pwsh -NoProfile -File $semaScript -Samples
    $p2 = $LASTEXITCODE
} else {
    Write-Host "=== parse conformance (full tests/) ==="
    & pwsh -NoProfile -File $parseScript
    $p1 = $LASTEXITCODE
    Write-Host "`n=== sema conformance (full tests/) ==="
    & pwsh -NoProfile -File $semaScript
    $p2 = $LASTEXITCODE
}

if ($p0 -ne 0 -or $pRegistry -ne 0 -or $p1 -ne 0 -or $p2 -ne 0) {
    exit 1
}
exit 0

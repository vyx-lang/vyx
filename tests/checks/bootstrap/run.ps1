[CmdletBinding()]
param(
    [ValidateSet('all','module_registry_failure','parser_recovery')]
    [string]$Case = 'all',
    [Alias('Compiler')]
    [string]$BootstrapCompiler = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$bootstrapTests = Join-Path $repoRoot 'tests\bootstrap'

$cases = @(
    [pscustomobject]@{ Name = 'module_registry_failure'; Path = Join-Path $bootstrapTests 'module_registry_failure\run.ps1' },
    [pscustomobject]@{ Name = 'parser_recovery'; Path = Join-Path $bootstrapTests 'parser_recovery\run.ps1' }
)
if ($Case -ne 'all') { $cases = @($cases | Where-Object Name -eq $Case) }
foreach ($entry in $cases) {
    if (-not (Test-Path -LiteralPath $entry.Path -PathType Leaf)) {
        throw "bootstrap check missing: $($entry.Path)"
    }
    Write-Host "[bootstrap] $($entry.Name)"
    if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
        & $entry.Path
    } else {
        & $entry.Path -Compiler $BootstrapCompiler
    }
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        throw "bootstrap check failed: $($entry.Name) (exit $LASTEXITCODE)"
    }
}
Write-Host "bootstrap checks PASS ($($cases.Count))"

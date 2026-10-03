param([Parameter(Mandatory)][string]$RequestFile)
$ErrorActionPreference = 'Stop'
$request = Get-Content -LiteralPath $RequestFile -Raw | ConvertFrom-Json -AsHashtable
& (Join-Path $PSScriptRoot 'Measure-Command.ps1') @request
exit $LASTEXITCODE

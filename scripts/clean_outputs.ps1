[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [switch]$Purge,
    [Alias('IncludeExamples')]
    [switch]$IncludeSamples,
    [switch]$IncludeBenchmarks
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path.TrimEnd('\')
$fileCandidates = @()

function Add-Candidate {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $resolved = (Resolve-Path -LiteralPath $Path).Path.TrimEnd('\')
    if ($resolved.Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not $resolved.StartsWith($repoRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing a cleanup path outside the repository: $resolved"
    }
    if ($script:candidates -notcontains $resolved) { $script:candidates += $resolved }
}

function Add-FileCandidate {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (-not $resolved.StartsWith($repoRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing a cleanup file outside the repository: $resolved"
    }
    if ($script:fileCandidates -notcontains $resolved) { $script:fileCandidates += $resolved }
}

function Add-GeneratedTrees {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$DirectoryNames
    )
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return }
    Get-ChildItem -LiteralPath $Root -Directory -Recurse -Force |
        Where-Object {
            $name = $_.Name
            @($DirectoryNames | Where-Object { $name -like $_ }).Count -gt 0
        } |
        ForEach-Object { Add-Candidate $_.FullName }
    Get-ChildItem -LiteralPath $Root -File -Recurse -Force |
        Where-Object { $_.Name -in @('.vyx_unit_sources', '.vyx-provenance') } |
        ForEach-Object { Add-FileCandidate $_.FullName }
}

$candidates = @()
Add-Candidate (Join-Path $repoRoot 'tests\.cache')

if (Test-Path -LiteralPath (Join-Path $repoRoot 'tests')) {
    Get-ChildItem -LiteralPath (Join-Path $repoRoot 'tests') -Directory -Recurse -Force |
        Where-Object { $_.Name -in @('.cache', 'target') } |
        ForEach-Object { Add-Candidate $_.FullName }
}

if ($IncludeBenchmarks -and (Test-Path -LiteralPath (Join-Path $repoRoot 'benchmarks'))) {
    Get-ChildItem -LiteralPath (Join-Path $repoRoot 'benchmarks') -Directory -Force |
        Where-Object { $_.Name -like 'out*' } |
        ForEach-Object { Add-Candidate $_.FullName }
    Add-GeneratedTrees (Join-Path $repoRoot 'benchmarks\workloads') @('.cache', 'target')
}

if ($IncludeSamples -and (Test-Path -LiteralPath (Join-Path $repoRoot 'samples\projects'))) {
    Add-GeneratedTrees (Join-Path $repoRoot 'samples\projects') @('.cache', 'target', 'cppDir', 'dist', 'build', 'tmp_*', '.cache_bak_*', 'shader_cache_*')
}

if (Test-Path -LiteralPath (Join-Path $repoRoot 'Zyn\out.pre_cold')) {
    Add-Candidate (Join-Path $repoRoot 'Zyn\out.pre_cold')
}

# A top-level cache/target makes all of its descendants redundant.  Collapse
# the list before invoking Remove-Item so a purge is deterministic and never
# attempts to remove a child after its parent.
$collapsed = @()
foreach ($candidate in ($candidates | Sort-Object Length)) {
    $covered = $false
    foreach ($parent in $collapsed) {
        if ($candidate.StartsWith($parent + '\', [StringComparison]::OrdinalIgnoreCase)) {
            $covered = $true
            break
        }
    }
    if (-not $covered) { $collapsed += $candidate }
}
$candidates = $collapsed

if (-not $Purge) {
    Write-Host 'Dry run. Pass -Purge to remove the listed generated directories.'
    $WhatIfPreference = $true
}

foreach ($candidate in ($candidates | Sort-Object)) {
    if ($PSCmdlet.ShouldProcess($candidate, 'remove generated output')) {
        Remove-Item -LiteralPath $candidate -Recurse -Force
    }
    else {
        Write-Host "[clean] $candidate"
    }
}

foreach ($candidate in ($fileCandidates | Sort-Object)) {
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
    if ($PSCmdlet.ShouldProcess($candidate, 'remove generated output file')) {
        Remove-Item -LiteralPath $candidate -Force
    }
    else {
        Write-Host "[clean] $candidate"
    }
}

if ($candidates.Count -eq 0) {
    Write-Host 'No generated output directories found.'
}

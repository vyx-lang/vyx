# Validate package manifests and their canonical standard-library sources.
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $repoRoot 'bootstrap_compiler\std_packages'
function Get-CanonicalLayout {
    param([string]$Root)

    $errors = New-Object System.Collections.Generic.List[string]
    $sourceIndex = @{}
    $packageCount = 0
    $canonicalCount = 0

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        [void]$errors.Add("canonical package root missing: $Root")
        return [pscustomobject]@{ Errors=$errors; SourceIndex=$sourceIndex; PackageCount=0; CanonicalCount=0 }
    }

    foreach ($packageDir in @(Get-ChildItem -LiteralPath $Root -Directory | Sort-Object Name)) {
        $manifest = Join-Path $packageDir.FullName 'Vyx.toml'
        if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
            [void]$errors.Add("$($packageDir.Name): missing Vyx.toml")
            continue
        }
        $packageCount++
        $raw = Get-Content -Raw -LiteralPath $manifest
        $isSourcePackage = $raw -match '(?m)^\s*type\s*=\s*"source"\s*(?:#.*)?$'
        $declared = New-Object System.Collections.Generic.HashSet[string] ([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($match in [regex]::Matches($raw, '"(src[/\\][^"\r\n]+\.vyx)"')) {
            [void]$declared.Add($match.Groups[1].Value.Replace('/', [System.IO.Path]::DirectorySeparatorChar))
        }
        if ($declared.Count -eq 0) {
            [void]$errors.Add("$($packageDir.Name): manifest declares no src/*.vyx source")
            continue
        }

        foreach ($relative in @($declared | Sort-Object)) {
            $absolute = Join-Path $packageDir.FullName $relative
            if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) {
                [void]$errors.Add("$($packageDir.Name): declared source missing: $relative")
                continue
            }
            $canonicalCount++
            if ($isSourcePackage) {
                $name = [System.IO.Path]::GetFileName($relative)
                if ($sourceIndex.ContainsKey($name)) {
                    [void]$errors.Add("duplicate source-package module '$name'")
                } else {
                    $sourceIndex[$name] = $absolute
                }
            }
        }

        $srcRoot = Join-Path $packageDir.FullName 'src'
        if (-not (Test-Path -LiteralPath $srcRoot -PathType Container)) {
            [void]$errors.Add("$($packageDir.Name): src directory missing")
            continue
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $srcRoot -Recurse -File -Filter '*.vyx')) {
            $relative = $file.FullName.Substring($packageDir.FullName.Length + 1)
            if (-not $declared.Contains($relative)) {
                [void]$errors.Add("$($packageDir.Name): undeclared canonical source: $relative")
            }
        }
    }

    return [pscustomobject]@{
        Errors = $errors
        SourceIndex = $sourceIndex
        PackageCount = $packageCount
        CanonicalCount = $canonicalCount
    }
}

$driftCount = 0
$layout = Get-CanonicalLayout $packageRoot
foreach ($message in $layout.Errors) {
    Write-Host "[DRIFT] $message" -ForegroundColor Red
    $driftCount++
}
if ($layout.Errors.Count -eq 0) {
    Write-Host ("[OK]    canonical package layout: {0} packages, {1} declared Vyx sources" -f $layout.PackageCount,$layout.CanonicalCount) -ForegroundColor Green
}

if ($driftCount -gt 0) {
    Write-Host ''
    Write-Host "std package drift detected in $driftCount item(s)." -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host ("Canonical package layout valid: {0} source-package modules." -f $layout.SourceIndex.Count) -ForegroundColor Green
exit 0

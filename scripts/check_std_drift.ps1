# Guard the canonical standard packages and their compatibility mirrors.
#
# Canonical sources: bootstrap_compiler/std_packages/*/src/
# Seed mirror:       bootstrap_compiler/std/
# Frozen host tree:  std/

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $repoRoot 'bootstrap_compiler\std_packages'
$seedStd = Join-Path $repoRoot 'bootstrap_compiler\std'
$hostStd = Join-Path $repoRoot 'std'

$hostCoreFiles = @(
    'dict.vyx', 'hashmap.vyx', 'hashset.vyx', 'btree.vyx',
    'vec.vyx', 'set.vyx', 'collections.vyx'
)

# Native-SDK/platform bindings that deliberately remain direct, seed-only
# opt-ins until they receive dedicated package manifests.
$seedOnlyModules = @(
    'cacao.vyx', 'curl.vyx', 'dll.vyx', 'grpc.vyx', 'httpclient.vyx',
    'llvm.vyx', 'openssl.vyx', 'sqlite.vyx', 'stb_image.vyx',
    'websocket.vyx', 'win32.vyx'
)

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

function Get-MirrorNormalizedText {
    param([string]$Path)
    $raw = ([System.IO.File]::ReadAllText($Path) -replace "`r`n", "`n") -replace "`r", "`n"
    [string[]]$lines = @($raw -split "`n" | ForEach-Object { $_ -replace '[ \t]+$', '' })
    $last = $lines.Count - 1
    while ($last -ge 0 -and $lines[$last].Length -eq 0) { $last-- }
    if ($last -lt 0) { return '' }
    return (($lines[0..$last]) -join "`n")
}

function Remove-Block {
    param([string[]]$Lines, [string]$StartPattern)
    $out = New-Object System.Collections.Generic.List[string]
    $depth = 0
    $inside = $false
    foreach ($line in $Lines) {
        if (-not $inside -and $line -match $StartPattern) { $inside = $true; $depth = 0 }
        if ($inside) {
            $depth += ([regex]::Matches($line, '\{')).Count
            $depth -= ([regex]::Matches($line, '\}')).Count
            if ($depth -le 0 -and $line -match '\}') { $inside = $false }
            continue
        }
        [void]$out.Add($line)
    }
    return $out.ToArray()
}

function Get-HostCoreNormalizedLines {
    param([string]$Path)
    $lines = Get-Content -LiteralPath $Path
    $lines = Remove-Block $lines '^\s*impl\s+Default\s+for\s'
    $lines = Remove-Block $lines '^\s*fn\s+dict_default_value'
    $lines = Remove-Block $lines '^\s*public\s+fn\s+get\(key:\s*K\)\s*->\s*V\b'
    $lines = Remove-Block $lines '^\s*public\s+fn\s+get\(key:\s*string\)\s*->\s*V\b'
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($line in $lines) {
        $t = $line
        if ($t -match '^\s*@\[' -or $t -match '^\s*///' -or
            $t -match '^\s*//' -or $t -match '^\s*use\s+std\.') { continue }
        $t = $t -replace '\s+where\s+V:\s*Default', ''
        $t = $t -replace '([A-Za-z_][A-Za-z0-9_]*)::<[^{};]+>\s*\{', '$1 {'
        $t = $t -replace '\.c_str\(\)', '.ptr'
        $t = $t -replace 'dict_default_value::<V>\(\)', '0'
        $t = $t -replace 'V::default_value\(\)', '0'
        $t = $t -replace '^(\s*var\s+[A-Za-z_][A-Za-z0-9_]*:\s*[A-Z][A-Za-z0-9_]*)\s*=\s*0\s*;', '$1;'
        $t = ($t -replace '//.*$', '').Trim() -replace '\s+', ' '
        if ($t.Length -gt 0) { [void]$out.Add($t) }
    }
    return $out.ToArray()
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

if (-not (Test-Path -LiteralPath $seedStd -PathType Container)) {
    Write-Host "[DRIFT] seed mirror missing: $seedStd" -ForegroundColor Red
    $driftCount++
} else {
    foreach ($name in @($layout.SourceIndex.Keys | Sort-Object)) {
        $seedFile = Join-Path $seedStd $name
        if (-not (Test-Path -LiteralPath $seedFile -PathType Leaf)) {
            Write-Host "[DRIFT] $name missing in bootstrap_compiler/std/ seed mirror" -ForegroundColor Red
            $driftCount++
        } elseif ((Get-MirrorNormalizedText $layout.SourceIndex[$name]) -cne (Get-MirrorNormalizedText $seedFile)) {
            Write-Host "[DRIFT] $name differs between canonical source and seed mirror" -ForegroundColor Red
            $driftCount++
        }
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $seedStd -File -Filter '*.vyx')) {
        if (-not $layout.SourceIndex.ContainsKey($file.Name) -and $file.Name -notin $seedOnlyModules) {
            Write-Host "[DRIFT] unclassified seed-only module: $($file.Name)" -ForegroundColor Red
            $driftCount++
        }
    }
}

if (-not (Test-Path -LiteralPath $hostStd -PathType Container)) {
    Write-Host "[DRIFT] frozen host std missing: $hostStd" -ForegroundColor Red
    $driftCount++
} else {
    foreach ($name in $hostCoreFiles) {
        $hostFile = Join-Path $hostStd $name
        if (-not (Test-Path -LiteralPath $hostFile -PathType Leaf) -or
            -not $layout.SourceIndex.ContainsKey($name)) {
            Write-Host "[DRIFT] $name missing from host or canonical package tree" -ForegroundColor Red
            $driftCount++
            continue
        }
        [string[]]$hostLines = @(Get-HostCoreNormalizedLines $hostFile)
        [string[]]$canonicalLines = @(Get-HostCoreNormalizedLines $layout.SourceIndex[$name])
        if (($hostLines -join "`n") -cne ($canonicalLines -join "`n")) {
            Write-Host "[DRIFT] $name core bodies differ between frozen host and canonical source" -ForegroundColor Red
            $driftCount++
        } else {
            Write-Host "[OK]    host core: $name" -ForegroundColor Green
        }
    }
}

if ($driftCount -gt 0) {
    Write-Host ''
    Write-Host "std package drift detected in $driftCount item(s)." -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host ("No std drift: {0} canonical source-package modules match the seed mirror." -f $layout.SourceIndex.Count) -ForegroundColor Green
exit 0

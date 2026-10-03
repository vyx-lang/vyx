param(
    [string]$BootstrapCompiler = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("vyx-versioned-field-migrate-" + [Guid]::NewGuid().ToString("N"))
$tempSrc = Join-Path $tempRoot "src"
New-Item -ItemType Directory -Path $tempSrc -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot "Vyx.toml") -Destination $tempRoot
Copy-Item -LiteralPath (Join-Path $projectRoot "Vyx.lock") -Destination $tempRoot
Get-ChildItem -LiteralPath (Join-Path $projectRoot "src") -Filter "*.vyx" -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $tempSrc }

try {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        # Drive the manifest build path. `--src=project <dir>` resolves the
        # versioned sibling modules through a different pipeline and reports an
        # unrelated `E2000: undefined function 'Record'` instead of the
        # migration diagnostic this fixture pins.
        Push-Location -LiteralPath $tempRoot
        try {
            $diagnostics = (& $BootstrapCompiler build 2>&1 | Out-String)
            $exitCode = $LASTEXITCODE
        } finally {
            Pop-Location
        }
    } finally {
        $ErrorActionPreference = $previous
    }

    if ($exitCode -ne 1) {
        throw "expected field migration diagnostic exit code 1, got $exitCode`n$diagnostics"
    }
    foreach ($needle in @(
        "error: E2400:",
        "field migration for 'renamed' references missing source field 'missing'",
        "add_v2.vyx"
    )) {
        if ($diagnostics.IndexOf($needle, [StringComparison]::Ordinal) -lt 0) {
            throw "field migration diagnostic is missing '$needle'`n$diagnostics"
        }
    }
    if ($diagnostics.IndexOf(":0:0:", [StringComparison]::Ordinal) -ge 0) {
        throw "field migration diagnostic contains a zero source location`n$diagnostics"
    }
    if ($diagnostics.IndexOf("undefined function 'Record'", [StringComparison]::Ordinal) -ge 0) {
        throw "field migration diagnostic cascaded into an unrelated constructor error`n$diagnostics"
    }
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "versioned_field_migrate_diagnostic PASS"

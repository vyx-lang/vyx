[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [int]$Jobs = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# The manifest has no [build] section, so the compiler writes the executable
# next to Vyx.toml instead of into target/.
$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path

$exeName = if ($env:OS -eq "Windows_NT") {
    "versioned_module_import.exe"
} else {
    "versioned_module_import"
}
$exePath = Join-Path $projectRoot $exeName
Remove-Item -LiteralPath $exePath -Force -ErrorAction SilentlyContinue

# Drive the manifest build path. `--src=project <dir>` resolves the versioned
# sibling modules through a different pipeline and mis-binds them (see the
# `--src=project` consistency note in docs/TESTING_GUIDE.md), so the fixture
# pins the end-user build entry point instead.
Push-Location $projectRoot
try {
    & $BootstrapCompiler build -j $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "versioned module import build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "versioned module import build did not produce $exePath"
}

# main.vyx returns a distinct non-zero code per versioned-resolution check.
& $exePath
if ($LASTEXITCODE -ne 0) {
    throw "versioned module import executable failed with exit code $LASTEXITCODE"
}

Write-Host "versioned_module_import: PASS"

param(
    [string]$BootstrapCompiler = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$depRoot = Join-Path $projectRoot "dep"
$shellDepRoot = Join-Path $projectRoot "shell_dep"
$appRoot = Join-Path $projectRoot "app"
$exe = Join-Path $appRoot "target\vyi_alias_sizeof_layout_app.exe"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

foreach ($path in @(
    (Join-Path $depRoot ".cache"),
    (Join-Path $depRoot "out"),
    (Join-Path $shellDepRoot ".cache"),
    (Join-Path $shellDepRoot "out"),
    (Join-Path $appRoot ".cache"),
    (Join-Path $appRoot "target")
)) {
    Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
}

Push-Location $appRoot
try {
    & $BootstrapCompiler build -j 2
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    Write-Error "project build did not produce $exe"
}

$output = & $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (($output -join "`n").Trim() -ne "vyi_alias_sizeof_layout OK") {
    Write-Error "unexpected output: $($output -join '`n')"
}

Write-Host "vyi_alias_sizeof_layout: OK"

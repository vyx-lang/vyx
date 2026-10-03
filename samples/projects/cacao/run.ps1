param(
    [string]$BootstrapCompiler = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

Push-Location $projectRoot
try {
    & $BootstrapCompiler build -j 2
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $projectRoot "target\cacao_dci.exe") --smoke
    exit $LASTEXITCODE
} finally {
    Pop-Location
}

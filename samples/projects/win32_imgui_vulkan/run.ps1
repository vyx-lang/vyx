param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [switch]$NoRun
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    foreach ($cand in @(
        (Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"),
        (Join-Path $repoRoot "bootstrap_compiler\boot_a.exe"),
        (Join-Path $repoRoot "cmake-build-debug\vyxc.exe")
    )) {
        if (Test-Path $cand) {
            $BootstrapCompiler = $cand
            break
        }
    }
}
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    throw "Bootstrap compiler not found."
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    foreach ($cand in @(
        (Join-Path $repoRoot "bootstrap_compiler\out"),
        (Join-Path $repoRoot "build_yolo_vyxcg\vyx_codegen"),
        (Join-Path $repoRoot "cmake-build-debug\vyx_codegen")
    )) {
        if (Test-Path $cand) {
            $RuntimeDir = $cand
            break
        }
    }
}
if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = (Resolve-Path $RuntimeDir).Path
}

Push-Location $projectRoot
try {
    if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
        $env:Path = $RuntimeDir + ";" + $env:Path
    }
    & $BootstrapCompiler build
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $exe = Join-Path $projectRoot "target\win32_imgui_vulkan.exe"
    if (-not (Test-Path $exe)) {
        Write-Error "expected executable not found: $exe"
    }

    if ($NoRun) {
        Write-Host "build complete: $exe"
        exit 0
    }

    & $exe
    exit $LASTEXITCODE
} finally {
    Pop-Location
}

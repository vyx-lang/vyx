param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [switch]$NoRun
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$sdlLibDir = (Resolve-Path (Join-Path $projectRoot "third_party\SDL3\lib\x64")).Path

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
    $env:Path = $sdlLibDir + ";" + $env:Path
    & $BootstrapCompiler build
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $targetDir = Join-Path $projectRoot "target"
    Copy-Item -Force (Join-Path $sdlLibDir "SDL3.dll") (Join-Path $targetDir "SDL3.dll")
    $exe = Join-Path $targetDir "sdl3_imgui_vulkan.exe"
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

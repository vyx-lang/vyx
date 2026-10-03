param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$isWindowsPlatform = $env:OS -eq "Windows_NT"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    foreach ($cand in @(
        (Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"),
        (Join-Path $repoRoot "bootstrap_compiler\boot_a.exe"),
        (Join-Path $repoRoot "cmake-build-debug\vyxc.exe")
    )) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) {
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
        if (Test-Path -LiteralPath $cand -PathType Container) {
            $RuntimeDir = $cand
            break
        }
    }
}
if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    throw "Vyx runtime directory not found."
}
$RuntimeDir = (Resolve-Path $RuntimeDir).Path

Push-Location $projectRoot
try {
    $env:Path = $RuntimeDir + [System.IO.Path]::PathSeparator + $env:Path
    & $BootstrapCompiler build
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $exeName = if ($isWindowsPlatform) { "mixed_c_cpp_vyx.exe" } else { "mixed_c_cpp_vyx" }
    $exe = Join-Path $projectRoot ("target\" + $exeName)
    if (-not (Test-Path $exe)) {
        Write-Error "expected executable not found: $exe"
    }

    & $exe
    exit $LASTEXITCODE
} finally {
    Pop-Location
}

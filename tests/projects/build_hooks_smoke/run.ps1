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
    Remove-Item -Force -ErrorAction SilentlyContinue hook_trace.txt, hook_trace.txt.copy
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue .cache, target
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue hooks\.cache

    & $BootstrapCompiler build
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $trace = (Get-Content -Raw -LiteralPath (Join-Path $projectRoot "hook_trace.txt")).Replace("`r", "")
    $expected = @(
        "build-pre",
        "target-prebuild",
        "precompile",
        "postcompile",
        "target-postbuild",
        "build-post"
    ) -join "`n"
    if ($trace.TrimEnd() -ne $expected) {
        Write-Error "unexpected hook trace:`n$trace"
    }

    if (-not (Test-Path (Join-Path $projectRoot "hook_trace.txt.copy"))) {
        Write-Error "expected copied trace not found"
    }

    Remove-Item -Force -ErrorAction SilentlyContinue hook_trace.txt, hook_trace.txt.copy
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue .cache, target, hooks\.cache
    exit 0
} finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue hooks\.cache
    Pop-Location
}

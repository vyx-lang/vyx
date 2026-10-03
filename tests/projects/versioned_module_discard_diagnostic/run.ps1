[CmdletBinding()]
param(
    [string]$BootstrapCompiler = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDir = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $projectDir "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$probeDir = Join-Path $projectDir "target\discard_diagnostic_probe"

function Remove-GeneratedTree([string]$relative) {
    $candidate = Join-Path $projectDir $relative
    if (-not (Test-Path -LiteralPath $candidate)) { return }
    $resolvedProject = (Resolve-Path -LiteralPath $projectDir).Path
    $resolved = (Resolve-Path -LiteralPath $candidate).Path
    if (-not $resolved.StartsWith($resolvedProject, [StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to clean outside test root: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

@('.cache', 'target') | ForEach-Object { Remove-GeneratedTree $_ }
[void][IO.Directory]::CreateDirectory($probeDir)

Push-Location $projectDir
try {
    $buildText = (& $BootstrapCompiler build -j 1 2>&1 | Out-String)
    $buildExit = $LASTEXITCODE
    if ($buildExit -ne 1) {
        throw "discarded method build must fail in Sema with exit 1; exit=$buildExit`n$buildText"
    }
    foreach ($required in @(
        "E2400",
        "method 'add4' is discarded",
        "add4@1.0.0[`"_1`"](...)"
    )) {
        if ($buildText.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            throw "discard diagnostic is missing '$required'`n$buildText"
        }
    }
    if ($buildText -notmatch '(?i)src[\\/]main\.vyx:\d+:\d+: error: E2400') {
        throw "discard diagnostic is not anchored to src/main.vyx`n$buildText"
    }
    foreach ($forbidden in @("undefined symbol", "executable link failed", "lld-link:")) {
        if ($buildText.IndexOf($forbidden, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "discard leaked past Sema into the linker: '$forbidden'`n$buildText"
        }
    }

    $vyiPath = Join-Path $probeDir "Add.vyi"
    $vyiText = (& $BootstrapCompiler --src=file (Join-Path $projectDir "src\add_v2.vyx") `
        --emit=vyi --vyi-shallow -o $vyiPath 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "VYI emission failed`n$vyiText"
    }
    $interfaceText = [IO.File]::ReadAllText($vyiPath)
    foreach ($required in @("@[migrate(", "@[discard(", 'fromVer="1.0.0"')) {
        if ($interfaceText.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            throw "VYI dropped semantic attribute '$required'`n$interfaceText"
        }
    }

    Write-Host "versioned_module_discard_diagnostic: OK"
} finally {
    if (Test-Path -LiteralPath $probeDir) {
        Remove-Item -LiteralPath $probeDir -Recurse -Force
    }
    Pop-Location
    @('.cache', 'target') | ForEach-Object { Remove-GeneratedTree $_ }
}

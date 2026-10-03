param(
    [Parameter(Mandatory = $true)]
    [string]$BootstrapCompiler
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') {
    Write-Host 'manifest_target_scope: SKIP (requires the Windows MSVC target toolchain)'
    exit 77
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$compiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path

function Remove-TestArtifact([string]$relative) {
    $candidate = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $candidate)) { return }
    $resolvedRoot = (Resolve-Path -LiteralPath $root).Path
    $resolved = (Resolve-Path -LiteralPath $candidate).Path
    if (-not $resolved.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to clean outside test root: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

$generatedArtifacts = @(
    '.cache',
    'target',
    'deps/global/.cache',
    'deps/global/out',
    'deps/local_a/.cache',
    'deps/local_a/out',
    'deps/local_b/.cache',
    'deps/local_b/out'
)
$generatedArtifacts | ForEach-Object { Remove-TestArtifact $_ }

Push-Location $root
try {
    $output = & $compiler build --target app_a --triplet x64_windows 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | Out-String)
    if ($exitCode -ne 0) {
        throw "target-scoped build failed ($exitCode):`n$text"
    }
    if ($text -notmatch 'target=app_a') {
        throw "build output did not report selected target:`n$text"
    }
    # `x64_windows` intentionally expands through the vendor-less LLVM alias
    # `x86_64-windows-msvc`; LLVM normalizes that spelling to `unknown`.
    if ($text -notmatch 'triplet=x86_64-unknown-windows-msvc') {
        throw "build output did not report canonical triplet:`n$text"
    }
    if (-not (Test-Path -LiteralPath 'target/app_a.exe')) {
        throw 'selected target app_a.exe was not produced'
    }
    if (-not (Test-Path -LiteralPath 'target/scope_helper.lib')) {
        throw 'depends_on target scope_helper.lib was not produced'
    }
    if (-not (Test-Path -LiteralPath 'deps/global/out/global_dep.lib')) {
        throw 'global dependency was not built'
    }
    if (-not (Test-Path -LiteralPath 'deps/local_a/out/local_a_dep.lib')) {
        throw 'app_a local dependency was not built'
    }
    if (Test-Path -LiteralPath 'target/app_b.exe') {
        throw 'unselected target app_b.exe was built'
    }
    if (Test-Path -LiteralPath 'deps/local_b/out/local_b_dep.lib') {
        throw 'unselected target local dependency was built'
    }

    & (Resolve-Path -LiteralPath 'target/app_a.exe').Path | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "app_a.exe returned $LASTEXITCODE"
    }

    # The vendor-less MSVC alias is the host ABI; a GNU environment is not.
    # Keep the host's flat vyx_runtime.lib out of the cross-runtime search.
    $savedRuntimeRoot = $env:VYX_RUNTIME_ROOT
    try {
        $env:VYX_RUNTIME_ROOT = ''
        $crossOutput = & $compiler build --target app_a --triplet x86_64-windows-gnu 2>&1
        $crossExit = $LASTEXITCODE
    } finally {
        $env:VYX_RUNTIME_ROOT = $savedRuntimeRoot
    }
    $crossText = ($crossOutput | Out-String)
    if ($crossExit -eq 0 -or $crossText -notmatch 'target runtime not found for `x86_64-unknown-windows-gnu`') {
        throw "cross-ABI target used the host runtime or failed before runtime selection:`n$crossText"
    }

    $badOutput = & $compiler build --target missing_target 2>&1
    $badExit = $LASTEXITCODE
    $badText = ($badOutput | Out-String)
    if ($badExit -eq 0 -or $badText -notmatch "manifest target not found: 'missing_target'") {
        throw "unknown target diagnostic is not stable:`n$badText"
    }

    Write-Host 'manifest_target_scope OK'
} finally {
    Pop-Location
    $generatedArtifacts | ForEach-Object { Remove-TestArtifact $_ }
}

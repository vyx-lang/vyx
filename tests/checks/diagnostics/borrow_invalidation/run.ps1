param(
    [string]$BootstrapCompiler = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $compilerName = if ($env:OS -eq "Windows_NT") { "boot.exe" } else { "boot" }
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$invalid = (Resolve-Path -LiteralPath (Join-Path $projectRoot "fixtures\invalid_after_mutation.vyx")).Path
$invalidDefer = (Resolve-Path -LiteralPath (Join-Path $projectRoot "fixtures\invalid_defer_capture.vyx")).Path
$invalidClosure = (Resolve-Path -LiteralPath (Join-Path $projectRoot "fixtures\invalid_closure_capture.vyx")).Path
$invalidClosureEscape = (Resolve-Path -LiteralPath (Join-Path $projectRoot "fixtures\invalid_closure_escape.vyx")).Path
$valid = (Resolve-Path -LiteralPath (Join-Path $projectRoot "fixtures\valid_nll.vyx")).Path
$validReborrow = (Resolve-Path -LiteralPath (Join-Path $projectRoot "fixtures\valid_reborrow_generation.vyx")).Path
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vyx-borrow-invalidation-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    $invalidOutput = Join-Path $tempRoot "invalid.ll"
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $diagnostics = (& $BootstrapCompiler --src=file $invalid --emit=ir -o $invalidOutput 2>&1 | Out-String)
        $invalidExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }

    if ($invalidExit -ne 1) {
        throw "borrow invalidation fixture returned exit code $invalidExit instead of 1`n$diagnostics"
    }
    $required = @(
        "error: E3101: borrowed reference 'view' may be invalidated by call to 'mutate'",
        "note: N3101: reference 'view' borrows this receiver's storage here",
        "value.mutate();",
        "let view = value.at();"
    )
    foreach ($needle in $required) {
        if ($diagnostics.IndexOf($needle, [StringComparison]::Ordinal) -lt 0) {
            throw "borrow invalidation diagnostic is missing '$needle'`n$diagnostics"
        }
    }
    if ($diagnostics.IndexOf(":0:0:", [StringComparison]::Ordinal) -ge 0) {
        throw "borrow invalidation diagnostic contains a zero source location`n$diagnostics"
    }

    foreach ($capturedInvalid in @($invalidDefer, $invalidClosure, $invalidClosureEscape)) {
        $capturedOutput = Join-Path $tempRoot (([IO.Path]::GetFileNameWithoutExtension($capturedInvalid)) + ".ll")
        $previousErrorAction = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $capturedDiagnostics = (& $BootstrapCompiler --src=file $capturedInvalid --emit=ir -o $capturedOutput 2>&1 | Out-String)
            $capturedExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorAction
        }
        if ($capturedExit -ne 1) {
            throw "captured borrow fixture '$capturedInvalid' returned exit code $capturedExit instead of 1`n$capturedDiagnostics"
        }
        foreach ($needle in @(
            "error: E3101: borrowed captured reference 'view' may be invalidated by call to 'mutate'",
            "note: N3101: captured reference 'view' borrows this receiver's storage here",
            "value.mutate();"
        )) {
            if ($capturedDiagnostics.IndexOf($needle, [StringComparison]::Ordinal) -lt 0) {
                throw "captured borrow diagnostic is missing '$needle'`n$capturedDiagnostics"
            }
        }
        if ($capturedDiagnostics.IndexOf(":0:0:", [StringComparison]::Ordinal) -ge 0) {
            throw "captured borrow diagnostic contains a zero source location`n$capturedDiagnostics"
        }
    }

    $validOutput = Join-Path $tempRoot "valid.ll"
    & $BootstrapCompiler --src=file $valid --emit=ir -o $validOutput
    if ($LASTEXITCODE -ne 0) {
        throw "NLL fixture failed to compile with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $validOutput)) {
        throw "NLL fixture did not emit LLVM IR"
    }

    $validReborrowOutput = Join-Path $tempRoot "valid_reborrow.ll"
    & $BootstrapCompiler --src=file $validReborrow --emit=ir -o $validReborrowOutput
    if ($LASTEXITCODE -ne 0) {
        throw "borrow value-generation fixture failed to compile with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $validReborrowOutput)) {
        throw "borrow value-generation fixture did not emit LLVM IR"
    }
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "borrow_invalidation PASS"

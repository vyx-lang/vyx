param(
    [string]$Python = "python",
    [string]$Rustc = "rustc",
    [string]$Triplet = "x64_windows"
)

$ErrorActionPreference = "Stop"
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $project "..\..\..\..")
$cli = Join-Path $repo "tools\dci\dci.py"
$source = Join-Path $project "native\lib.rs"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("vyx-dci-rust-" + [Guid]::NewGuid().ToString("N"))

New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $contract = Join-Path $temp "rust_adapter.dcib"
    $debugJson = Join-Path $temp "rust_adapter.dci.json"

    & $Python $cli adapter --language rust $source `
        --rustc $Rustc `
        --triplet $Triplet `
        --crate-name dci_rust_adapter `
        --edition 2024 `
        --output $contract `
        --debug-json $debugJson
    if ($LASTEXITCODE -ne 0) {
        throw "Rust DCI Adapter failed with exit code $LASTEXITCODE"
    }

    & $Python $cli validate --strict $contract
    if ($LASTEXITCODE -ne 0) {
        throw "strict DCI validation failed with exit code $LASTEXITCODE"
    }

    $document = Get-Content $debugJson -Raw | ConvertFrom-Json
    if ($document.source.language -ne "rust") {
        throw "contract source language is not rust"
    }
    if ($document.exports.layouts.Count -ne 3) {
        throw "expected 3 verified layouts, got $($document.exports.layouts.Count)"
    }
    if ($document.exports.symbols.Count -ne 3) {
        throw "expected 3 executable symbols, got $($document.exports.symbols.Count)"
    }
    if ($document.exports.rejected_symbols.Count -ne 1) {
        throw "expected exactly one fail-closed symbol rejection"
    }
    if ($document.exports.rejected_symbols[0].link_name -ne "dci_payload_not_exported") {
        throw "unexpected rejected symbol identity"
    }
    if ($document.exports.rejected_symbols[0].reason -notmatch "data-carrying enum") {
        throw "rejected payload did not carry the expected reason"
    }

    Write-Host "dci_rust_adapter: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

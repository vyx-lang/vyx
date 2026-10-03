param(
    [string]$BootstrapCompiler = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $compilerName = if ($env:OS -eq "Windows_NT") { "vyxc.exe" } else { "vyxc" }
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$fixture = (Resolve-Path -LiteralPath (Join-Path $projectRoot "fixtures\borrowed_delete.vyx")).Path
$output = Join-Path ([System.IO.Path]::GetTempPath()) ("vyx-delete-diagnostic-" + [Guid]::NewGuid().ToString("N") + ".ll")

$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $diagnostics = (& $BootstrapCompiler --src=file $fixture --emit=ir -o $output 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorAction
    Remove-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue
}

if ($exitCode -ne 1) {
    throw "borrowed delete diagnostic returned exit code $exitCode instead of 1`n$diagnostics"
}

$required = @(
    "error: E3101: delete cannot release borrowed storage",
    "error: E3000: delete requires an owned pointer/reference value",
    ($fixture + ":4:"),
    "4 |     delete p1;"
)
foreach ($needle in $required) {
    if ($diagnostics.IndexOf($needle, [StringComparison]::Ordinal) -lt 0) {
        throw "borrowed delete diagnostic is missing '$needle'`n$diagnostics"
    }
}
if ($diagnostics.IndexOf(":0:0:", [StringComparison]::Ordinal) -ge 0) {
    throw "borrowed delete diagnostic still contains a zero source location`n$diagnostics"
}

Write-Host "delete_diagnostic_location PASS"

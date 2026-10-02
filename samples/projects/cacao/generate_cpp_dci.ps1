param(
    [string]$CacaoRoot = "E:\Dev\C++\Cacao",
    [string]$Clang = "",
    [switch]$AllPublicHeaders
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cacaoRoot = (Resolve-Path $CacaoRoot).Path
$includeRoot = Join-Path $cacaoRoot "include"
$generatedInclude = Join-Path $cacaoRoot "cmake-build-debug\Debug\include"

if ([string]::IsNullOrWhiteSpace($Clang)) {
    $Clang = (Resolve-Path (Join-Path $repoRoot "clang\bin\clang++.exe")).Path
} else {
    $Clang = (Resolve-Path $Clang).Path
}

$adapter = Join-Path $repoRoot "tools\dci\dci_adapter_msvc.py"
$output = Join-Path $projectRoot "dci\cacao-cpp-full.dcib"
$debugJson = Join-Path $projectRoot "dci\cacao-cpp-full.dci"
$arguments = @(
    $adapter,
    "--out", $output,
    "--debug-json-out", $debugJson,
    "--include", (Join-Path $includeRoot "Cacao.hpp"),
    "--clang", $Clang,
    "--target", "x86_64-pc-windows-msvc",
    "--std", "c++20"
)
if ($AllPublicHeaders) { $arguments += "--scan-public-root" }
$arguments += @(
    "--",
    "-I", (Join-Path $projectRoot ".adapter_include"),
    "-I", $includeRoot,
    "-I", $generatedInclude,
    "-D_WIN32",
    "-D_WINDOWS"
)

& python @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& python (Join-Path $repoRoot "tools\dci\dcib.py") decode $output $debugJson
exit $LASTEXITCODE

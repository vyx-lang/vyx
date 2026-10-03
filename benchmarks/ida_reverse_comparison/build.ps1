param(
    [string]$VyxCompiler = "",
    [string]$Clangxx = "clang++",
    [string]$Rustc = "rustc",
    [string]$LlvmStrip = "llvm-strip",
    [string]$LlvmReadObj = "llvm-readobj",
    [string]$LlvmPdbUtil = "llvm-pdbutil",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $root "..\.."))
$sourceRoot = Join-Path $root "src"

if ([string]::IsNullOrWhiteSpace($VyxCompiler)) {
    $VyxCompiler = Join-Path $repoRoot "dist\vyx-windows-x86_64-llvm22\vyxc.exe"
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $root "out"
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Resolve-Tool([string]$Value) {
    if (Test-Path -LiteralPath $Value -PathType Leaf) {
        return (Resolve-Path -LiteralPath $Value).Path
    }
    return (Get-Command $Value -ErrorAction Stop).Source
}

function Invoke-Checked([string]$FilePath, [string[]]$ArgumentList) {
    Write-Host "> $FilePath $($ArgumentList -join ' ')"
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function First-Line([string]$Text) {
    $lines = @($Text -split "\r?\n" | Where-Object { $_.Trim().Length -gt 0 })
    if ($lines.Count -eq 0) { return "" }
    return ([string]$lines[0]).Trim()
}

$vyxc = Resolve-Tool $VyxCompiler
$clang = Resolve-Tool $Clangxx
$rust = Resolve-Tool $Rustc
$strip = Resolve-Tool $LlvmStrip
$readObj = Resolve-Tool $LlvmReadObj
$pdbUtil = Resolve-Tool $LlvmPdbUtil

$symbolVyx = Join-Path $OutDir "vyx.symbols.exe"
$symbolCpp = Join-Path $OutDir "cpp.symbols.exe"
$symbolRust = Join-Path $OutDir "rust.symbols.exe"
$pdbVyx = Join-Path $OutDir "vyx.symbols.pdb"
$pdbCpp = Join-Path $OutDir "cpp.symbols.pdb"
$pdbRust = Join-Path $OutDir "rust.symbols.pdb"
$rawVyx = Join-Path $OutDir "vyx.nosymbols.raw.exe"
$rawCpp = Join-Path $OutDir "cpp.nosymbols.raw.exe"
$rawRust = Join-Path $OutDir "rust.nosymbols.raw.exe"
$finalVyx = Join-Path $OutDir "vyx.exe"
$finalCpp = Join-Path $OutDir "cpp.exe"
$finalRust = Join-Path $OutDir "rust.exe"

$vyxSource = Join-Path $sourceRoot "main.vyx"
$cppSource = Join-Path $sourceRoot "main.cpp"
$rustSource = Join-Path $sourceRoot "main.rs"

Invoke-Checked $vyxc @(
    "--emit=exe",
    "--src=file", $vyxSource,
    "-O3",
    "--mir-opt", "3",
    "--llvm-opt", "3",
    "-g",
    "-o", $symbolVyx
)

Invoke-Checked $vyxc @(
    "--emit=exe",
    "--src=file", $vyxSource,
    "-O3",
    "--mir-opt", "3",
    "--llvm-opt", "3",
    "-o", $rawVyx
)

Invoke-Checked $clang @(
    $cppSource,
    "-std=c++20",
    "-O3",
    "-DNDEBUG",
    "-fno-exceptions",
    "-fno-rtti",
    "-gcodeview",
    "-fuse-ld=lld",
    "-Wl,/OPT:REF",
    "-Wl,/OPT:ICF",
    "-Wl,/DEBUG:FULL",
    "-Wl,/PDB:$pdbCpp",
    "-Wl,/PDBALTPATH:%_PDB%",
    "-o", $symbolCpp
)

Invoke-Checked $clang @(
    $cppSource,
    "-std=c++20",
    "-O3",
    "-DNDEBUG",
    "-fno-exceptions",
    "-fno-rtti",
    "-fuse-ld=lld",
    "-Wl,/OPT:REF",
    "-Wl,/OPT:ICF",
    "-Wl,/DEBUG:NONE",
    "-o", $rawCpp
)

Invoke-Checked $rust @(
    $rustSource,
    "--target", "x86_64-pc-windows-msvc",
    "-C", "opt-level=3",
    "-C", "debuginfo=2",
    "-C", "panic=abort",
    "-C", "codegen-units=1",
    "-C", "lto=off",
    "-C", "linker=lld-link",
    "-C", "link-arg=/OPT:REF",
    "-C", "link-arg=/OPT:ICF",
    "-C", "link-arg=/DEBUG:FULL",
    "-C", "link-arg=/PDB:$pdbRust",
    "-C", "link-arg=/PDBALTPATH:%_PDB%",
    "-o", $symbolRust
)

Invoke-Checked $rust @(
    $rustSource,
    "--target", "x86_64-pc-windows-msvc",
    "-C", "opt-level=3",
    "-C", "debuginfo=0",
    "-C", "panic=abort",
    "-C", "codegen-units=1",
    "-C", "lto=off",
    "-C", "linker=lld-link",
    "-C", "link-arg=/OPT:REF",
    "-C", "link-arg=/OPT:ICF",
    "-C", "link-arg=/DEBUG:NONE",
    "-o", $rawRust
)

Invoke-Checked $strip @("--strip-all", "-o", $finalVyx, $rawVyx)
Invoke-Checked $strip @("--strip-all", "-o", $finalCpp, $rawCpp)
Invoke-Checked $strip @("--strip-all", "-o", $finalRust, $rawRust)

foreach ($raw in @($rawVyx, $rawCpp, $rawRust)) {
    if (Test-Path -LiteralPath $raw -PathType Leaf) {
        [System.IO.File]::Delete($raw)
    }
}

$hashRows = foreach ($name in @(
    "vyx.symbols.exe", "vyx.symbols.pdb", "vyx.exe",
    "cpp.symbols.exe", "cpp.symbols.pdb", "cpp.exe",
    "rust.symbols.exe", "rust.symbols.pdb", "rust.exe"
)) {
    $path = Join-Path $OutDir $name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name"
}
Set-Content -LiteralPath (Join-Path $OutDir "SHA256SUMS.txt") -Value $hashRows -Encoding ascii

$vyxVersion = First-Line ((& $vyxc --help 2>&1 | Out-String))
$clangVersion = First-Line ((& $clang --version 2>&1 | Out-String))
$rustVersion = First-Line ((& $rust --version 2>&1 | Out-String))
$stripVersion = First-Line ((& $strip --version 2>&1 | Out-String))
$toolchainRows = @(
    "target=x86_64-pc-windows-msvc",
    "optimization=O3",
    "lto=disabled",
    "symbolized=CodeView plus PDB",
    "stripped=no-debug O3 build plus llvm-strip --strip-all",
    "vyx=$vyxVersion",
    "cpp=$clangVersion",
    "rust=$rustVersion",
    "strip=$stripVersion"
)
Set-Content -LiteralPath (Join-Path $OutDir "toolchains.txt") -Value $toolchainRows -Encoding utf8

& (Join-Path $root "verify.ps1") `
    -OutDir $OutDir `
    -LlvmReadObj $readObj `
    -LlvmPdbUtil $pdbUtil
if ($LASTEXITCODE -ne 0) {
    throw "verification failed with exit code $LASTEXITCODE"
}

Write-Host "IDA comparison binaries are ready in $OutDir"

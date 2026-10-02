[CmdletBinding()]
param(
    [string]$Triplet = "x86_64-unknown-linux-gnu",
    [string]$RecordsPath = "",
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "..")).Path
$compiler = Join-Path $projectRoot "out\boot.exe"
$records = if ([string]::IsNullOrWhiteSpace($RecordsPath)) {
    Join-Path $projectRoot ".cache\records_boot.tmp"
} else {
    [IO.Path]::GetFullPath($RecordsPath)
}
$llvmNm = Join-Path $repoRoot "clang\bin\llvm-nm.exe"
$outDir = Join-Path $projectRoot "out\linux"
$sourceList = Join-Path $outDir "crate_boot.sources"
$object = Join-Path $outDir "crate_boot.o"
$candidate = Join-Path $outDir "crate_boot.new.o"
$log = Join-Path $outDir "crate_boot.emit.log"

foreach ($required in @($compiler, $records, $llvmNm)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required build input is missing: $required. Build the current boot target first."
    }
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Use the object records from the current partitioned boot build. A cached
# project-unit source list belongs to the build that created it, not to the
# current manifest: an incomplete list lets imports acquire bodyless/shallow
# declarations and produces unresolved MIR target references.
$sources = [Collections.Generic.List[string]]::new()
$selected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in [IO.File]::ReadLines($records)) {
    if ($line.StartsWith("__vyi:")) { continue }
    $fields = $line.Split("`t")
    if ($fields.Length -lt 2) { continue }
    $path = $fields[0].Replace('\', '/')
    if ($path -notmatch '^src/(core|hir|mir|codegen)/.+\.vyx$') { continue }
    if ($fields[1] -notmatch '\.obj$') { continue }
    if (-not $selected.Add($path)) { throw "Duplicate boot object record: $path" }
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $path) -PathType Leaf)) {
        throw "Boot object record points to a missing source: $path"
    }
    $sources.Add($path)
}
if ($sources.Count -eq 0 -or -not $selected.Contains('src/core/main.vyx')) {
    throw "Current boot object records contain no compiler entry and sources: $records"
}

# Reject an incomplete record set before spending minutes and gigabytes on
# cross emission. Every bootstrap import in the selected unit needs its source
# in the same list; shallow interfaces alone are insufficient for this crate.
$modulePaths = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -Filter '*.vyx' -File -Recurse) {
    $text = [IO.File]::ReadAllText($file.FullName)
    $module = [regex]::Match($text, '(?m)^\s*module\s+(bootstrap\.[A-Za-z0-9_.]+)\s*;')
    if (-not $module.Success) { continue }
    $relative = [IO.Path]::GetRelativePath($projectRoot, $file.FullName).Replace('\', '/')
    $name = $module.Groups[1].Value
    if ($modulePaths.ContainsKey($name)) { throw "Duplicate bootstrap module: $name" }
    $modulePaths.Add($name, $relative)
}
foreach ($path in $sources) {
    $text = [IO.File]::ReadAllText((Join-Path $projectRoot $path))
    foreach ($import in [regex]::Matches($text, '(?m)^\s*use\s+(bootstrap\.[A-Za-z0-9_.]+)\s*;')) {
        $name = $import.Groups[1].Value
        if (-not $modulePaths.ContainsKey($name)) { throw "Unmapped bootstrap import $name in $path" }
        $dependency = $modulePaths[$name]
        if (-not $selected.Contains($dependency)) {
            throw "Incomplete boot object records: $path imports $name ($dependency)"
        }
    }
}

$recordsTime = (Get-Item -LiteralPath $records).LastWriteTimeUtc
$compilerTime = (Get-Item -LiteralPath $compiler).LastWriteTimeUtc
foreach ($path in $sources) {
    $sourceTime = (Get-Item -LiteralPath (Join-Path $projectRoot $path)).LastWriteTimeUtc
    if ($sourceTime -gt $recordsTime -or $sourceTime -gt $compilerTime) {
        throw "Compiler source changed after the boot build: $path. Rebuild boot before cross emission."
    }
}

[IO.File]::WriteAllLines($sourceList, $sources)
Write-Host "Linux boot crate: $($sources.Count) compiler sources, target=$Triplet"
if ($DryRun) { return }

# Remove the old canonical input before invoking the compiler. A failed emit
# must make the later container link fail, never link a previous SDK object.
if (Test-Path -LiteralPath $object -PathType Leaf) {
    Move-Item -LiteralPath $object -Destination "$object.stale.bin" -Force
}
if (Test-Path -LiteralPath $candidate -PathType Leaf) {
    Remove-Item -LiteralPath $candidate -Force
}

$oldCodegenUnits = [Environment]::GetEnvironmentVariable('VYX_CODEGEN_UNITS')
$oldLlvmRoot = [Environment]::GetEnvironmentVariable('LLVM_ROOT')
try {
    $env:VYX_CODEGEN_UNITS = '1'
    if ([string]::IsNullOrWhiteSpace($oldLlvmRoot)) { $env:LLVM_ROOT = Join-Path $repoRoot 'clang' }
    Push-Location $projectRoot
    try {
        & $compiler --src=file 'src\core\main.vyx' `
            --project-unit-sources $sourceList --emit=obj `
            -L out -l vyx_compiler_backend -l vyx_runtime -l synchronization `
            -O2 "--triplet=$Triplet" -o $candidate *> $log
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($exitCode -ne 0) { throw "Linux crate_boot emit failed (exit $exitCode): $log" }
} finally {
    [Environment]::SetEnvironmentVariable('VYX_CODEGEN_UNITS', $oldCodegenUnits)
    [Environment]::SetEnvironmentVariable('LLVM_ROOT', $oldLlvmRoot)
}

if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
    throw "Linux crate_boot emit produced no object: $log"
}
$stream = [IO.File]::OpenRead($candidate)
try {
    $magic = [byte[]]::new(4)
    if ($stream.Read($magic, 0, 4) -ne 4 -or
        $magic[0] -ne 0x7f -or $magic[1] -ne 0x45 -or
        $magic[2] -ne 0x4c -or $magic[3] -ne 0x46) {
        throw "Linux crate_boot output is not ELF: $candidate"
    }
} finally {
    $stream.Dispose()
}
$symbols = @(& $llvmNm --defined-only $candidate)
if ($LASTEXITCODE -ne 0 -or $symbols.Count -eq 0) {
    throw "Linux crate_boot output has no defined symbols: $candidate"
}
Move-Item -LiteralPath $candidate -Destination $object
Write-Host "Linux boot crate ready: $object ($($symbols.Count) defined symbols)"

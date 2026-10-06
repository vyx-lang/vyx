[CmdletBinding()]
param(
    [string]$Triplet = "x86_64-unknown-linux-gnu",
    [string]$RecordsPath = "",
    # Where crate_boot.o and its scratch files land. Each cross target needs its
    # own directory: the container link and the smoke scripts read
    # out/linux/crate_*.o by name, so emitting an aarch64 object over the
    # x86_64 one would silently cross-link two architectures.
    [string]$OutDir = "",
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
$outDir = if ([string]::IsNullOrWhiteSpace($OutDir)) {
    Join-Path $projectRoot "out\linux"
} else {
    [IO.Path]::GetFullPath($OutDir)
}
$sourceList = Join-Path $outDir "crate_boot.sources"
$object = Join-Path $outDir "crate_boot.o"
$candidate = Join-Path $outDir "crate_boot.new.o"
$log = Join-Path $outDir "crate_boot.emit.log"

# ELF e_machine. x86-64 is 62, AArch64 is 183. The magic check alone cannot tell
# two cross targets apart, so a stale or mistyped --triplet would sail through
# and produce an SDK whose compiler cannot run on the machine it was named for.
$expectedMachine = switch -Regex ($Triplet) {
    '^aarch64' { 183 }
    '^riscv64' { 243 }
    '^i[3-6]86' { 3 }
    default     { 62 }
}

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

# WriteAllLines uses Environment.NewLine, so on Windows the list lands as CRLF.
# --project-unit-sources splits on "\n" and keeps the "\r" as part of each path,
# so every entry resolves to a file that does not exist: the compiler exits 127
# with an empty log and no diagnostic, which reads like a crash in the emit.
[IO.File]::WriteAllText($sourceList, (($sources -join "`n") + "`n"))
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
    $header = [byte[]]::new(20)
    if ($stream.Read($header, 0, 20) -ne 20 -or
        $header[0] -ne 0x7f -or $header[1] -ne 0x45 -or
        $header[2] -ne 0x4c -or $header[3] -ne 0x46) {
        throw "Linux crate_boot output is not ELF: $candidate"
    }
    if ($header[4] -ne 2) {
        throw "Linux crate_boot output is not ELF64: $candidate (EI_CLASS=$($header[4]))"
    }
    $machine = [int]$header[18] -bor ([int]$header[19] -shl 8)
    if ($machine -ne $expectedMachine) {
        throw "Linux crate_boot e_machine=$machine but --triplet=$($Triplet) expects $expectedMachine`: $candidate"
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

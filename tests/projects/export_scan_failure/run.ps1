param(
    [string]$BootstrapCompiler = "",
    [string]$Clang = ""
)

$ErrorActionPreference = "Stop"

if ($env:OS -ne "Windows_NT") {
    Write-Host "export_scan_failure SKIP (Windows .def export scan only)"
    exit 0
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path
if ([string]::IsNullOrWhiteSpace($Clang)) {
    $Clang = Join-Path $repoRoot "clang\bin\clang++.exe"
}
$Clang = (Resolve-Path $Clang).Path

$cache = Join-Path $projectRoot ".cache"
$fakeBin = Join-Path $cache "fake-llvm-bin"
New-Item -ItemType Directory -Force -Path $fakeBin | Out-Null
$fakeSource = Join-Path $fakeBin "failing_nm.cpp"
$fakeNm = Join-Path $fakeBin "llvm-nm.exe"
[IO.File]::WriteAllText($fakeSource, @'
#include <cstdio>
int main() {
    std::puts("partial_symbol_must_not_be_cached");
    std::fputs("intentional llvm-nm failure\n", stderr);
    return 23;
}
'@)
& $Clang $fakeSource -O0 -fms-runtime-lib=static -o $fakeNm
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $fakeNm -PathType Leaf)) {
    throw "failed to build the llvm-nm failure fixture"
}

function Invoke-ExportScanFailure {
    param([int]$Jobs)

    $exportsDir = Join-Path $cache "exports"
    $targetDir = Join-Path $projectRoot "target"
    Remove-Item -LiteralPath $exportsDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $targetDir -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $cache -Filter "*.def" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force

    $stdout = Join-Path $cache ("export_scan_j{0}.out.log" -f $Jobs)
    $stderr = Join-Path $cache ("export_scan_j{0}.err.log" -f $Jobs)
    $previousLlvmBin = $env:LLVM_BIN
    $previousErrorAction = $ErrorActionPreference
    $env:LLVM_BIN = $fakeBin
    $ErrorActionPreference = "Continue"
    Push-Location $projectRoot
    try {
        & $BootstrapCompiler build -j $Jobs > $stdout 2> $stderr
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
        $ErrorActionPreference = $previousErrorAction
        if ($null -eq $previousLlvmBin) {
            Remove-Item Env:LLVM_BIN -ErrorAction SilentlyContinue
        } else {
            $env:LLVM_BIN = $previousLlvmBin
        }
    }

    if ($exitCode -eq 0) {
        throw "export scan failure unexpectedly succeeded with -j$Jobs"
    }
    $log = ((Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue) +
            (Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue)) -join "`n"
    if ($log.IndexOf("export scan failed", [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "-j$Jobs did not propagate the llvm-nm failure"
    }
    if ($log.IndexOf("link failed for", [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "-j$Jobs continued into the linker after export scan failure"
    }
    if (Test-Path -LiteralPath $exportsDir) {
        $cached = @(Get-ChildItem -LiteralPath $exportsDir -Filter "*.exports" -File -ErrorAction SilentlyContinue)
        if ($cached.Count -ne 0) {
            throw "-j$Jobs cached partial llvm-nm stdout"
        }
    }
    $defs = @(Get-ChildItem -LiteralPath $cache -Filter "*.def" -File -ErrorAction SilentlyContinue)
    if ($defs.Count -ne 0) {
        throw "-j$Jobs wrote a .def file after export scan failure"
    }
    if (Test-Path -LiteralPath $targetDir) {
        $outputs = @(Get-ChildItem -LiteralPath $targetDir -File -Recurse -ErrorAction SilentlyContinue)
        if ($outputs.Count -ne 0) {
            throw "-j$Jobs produced link output after export scan failure"
        }
    }
}

Invoke-ExportScanFailure -Jobs 1
Invoke-ExportScanFailure -Jobs 4

$exportsDir = Join-Path $cache "exports"
$targetDir = Join-Path $projectRoot "target"

# The publish temp path is `<export cache entry>.publish.tmp`, and the entry
# name embeds the scanned object path. Derive it from the export cache that one
# successful build produces instead of pinning an object name, which changes
# whenever the crate/CGU naming scheme does.
Remove-Item -LiteralPath $exportsDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $targetDir -Recurse -Force -ErrorAction SilentlyContinue
$previousLlvmBinForPrime = $env:LLVM_BIN
$env:LLVM_BIN = Split-Path -Parent $Clang
Push-Location $projectRoot
try {
    & $BootstrapCompiler build -j 1 *> (Join-Path $cache "export_prime.out.log")
} finally {
    Pop-Location
    if ($null -eq $previousLlvmBinForPrime) {
        Remove-Item Env:LLVM_BIN -ErrorAction SilentlyContinue
    } else {
        $env:LLVM_BIN = $previousLlvmBinForPrime
    }
}
$exportEntries = @(Get-ChildItem -LiteralPath $exportsDir -Filter "*.exports" -File -ErrorAction SilentlyContinue)
if ($exportEntries.Count -eq 0) {
    throw "priming build did not populate the export cache"
}
Remove-Item -LiteralPath $exportsDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $targetDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $exportsDir | Out-Null
foreach ($entry in $exportEntries) {
    New-Item -ItemType Directory -Path (Join-Path $exportsDir ($entry.Name + ".publish.tmp")) | Out-Null
}
$publishOut = Join-Path $cache "export_publish_failure.out.log"
$publishErr = Join-Path $cache "export_publish_failure.err.log"
$previousLlvmBin = $env:LLVM_BIN
$env:LLVM_BIN = Split-Path -Parent $Clang
Push-Location $projectRoot
try {
    & $BootstrapCompiler build -j 1 > $publishOut 2> $publishErr
    $publishExit = $LASTEXITCODE
} finally {
    Pop-Location
    if ($null -eq $previousLlvmBin) {
        Remove-Item Env:LLVM_BIN -ErrorAction SilentlyContinue
    } else {
        $env:LLVM_BIN = $previousLlvmBin
    }
    Remove-Item -LiteralPath $exportsDir -Recurse -Force -ErrorAction SilentlyContinue
}
if ($publishExit -eq 0) {
    throw "export cache publish failure unexpectedly succeeded"
}
$publishLog = ((Get-Content -LiteralPath $publishOut -ErrorAction SilentlyContinue) +
               (Get-Content -LiteralPath $publishErr -ErrorAction SilentlyContinue)) -join "`n"
if ($publishLog.IndexOf("cannot publish export cache", [StringComparison]::OrdinalIgnoreCase) -lt 0) {
    throw "export cache write failure did not propagate"
}
if (Test-Path -LiteralPath $targetDir) {
    $publishOutputs = @(Get-ChildItem -LiteralPath $targetDir -File -Recurse -ErrorAction SilentlyContinue)
    if ($publishOutputs.Count -ne 0) {
        throw "export cache write failure produced link output"
    }
}
Write-Host "export_scan_failure: OK"

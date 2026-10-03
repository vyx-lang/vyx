[CmdletBinding()]
param(
    [string]$BootstrapCompiler = ".\bootstrap_compiler\out\boot.exe"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
$compilerPath = (Resolve-Path -LiteralPath (Join-Path $repoRoot $BootstrapCompiler)).Path
if (-not $env:LLVM_ROOT) {
    $clangRoot = Join-Path $repoRoot "clang"
    if (Test-Path -LiteralPath $clangRoot) {
        $env:LLVM_ROOT = (Resolve-Path -LiteralPath $clangRoot).Path
    }
}
if ($env:LLVM_ROOT) {
    $env:PATH = (Join-Path $env:LLVM_ROOT "bin") + [IO.Path]::PathSeparator + $env:PATH
}

$outDir = Join-Path $env:TEMP "vyx-async-probe"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Invoke-Boot {
    param([string[]]$ArgumentList)
    $proc = Start-Process -FilePath $compilerPath `
        -ArgumentList $ArgumentList `
        -WorkingDirectory $repoRoot -NoNewWindow -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $outDir "boot.out.txt") `
        -RedirectStandardError (Join-Path $outDir "boot.err.txt")
    return $proc.ExitCode
}

$splitSrc = Join-Path $repoRoot "probes\gates\async\await_split.vyx"
$dumpRc = Invoke-Boot @("--src=file", $splitSrc, "--dump-mir2")
$dumpText = (Get-Content -LiteralPath (Join-Path $outDir "boot.out.txt") -Raw) + (Get-Content -LiteralPath (Join-Path $outDir "boot.err.txt") -Raw)
if ($dumpRc -ne 0) {
    Write-Host "async FAIL: dump-mir2 exit=$dumpRc"
    Write-Host $dumpText
    exit $dumpRc
}
if ($dumpText -notmatch "term=7") {
    Write-Host "async FAIL: dump-mir2 missing term=7 (mir_term_yield)"
    exit 1
}

$out = Join-Path $outDir "await_split.exe"
$emitRc = Invoke-Boot @("--src=file", $splitSrc, "--emit=exe", "-o", $out)
if ($emitRc -ne 0) {
    Write-Host "async FAIL: await_split emit exit=$emitRc"
    Get-Content -LiteralPath (Join-Path $outDir "boot.err.txt")
    exit $emitRc
}
$run = Start-Process -FilePath $out -WorkingDirectory $repoRoot -NoNewWindow -PassThru -Wait
if ($run.ExitCode -ne 0) {
    Write-Host "async FAIL: await_split exit=$($run.ExitCode) (expected 0 / log 21 + pending_returns>=1)"
    exit $run.ExitCode
}

$source = Join-Path $repoRoot "probes\gates\async\interleave.vyx"
$out2 = Join-Path $outDir "interleave.exe"
$emit2 = Invoke-Boot @("--src=file", $source, "--emit=exe", "-o", $out2)
if ($emit2 -ne 0) {
    Write-Host "async FAIL: interleave emit exit=$emit2"
    exit $emit2
}
$run2 = Start-Process -FilePath $out2 -WorkingDirectory $repoRoot -NoNewWindow -PassThru -Wait
if ($run2.ExitCode -ne 0) {
    Write-Host "async FAIL: interleave exit=$($run2.ExitCode) (expected 0 / log 21)"
    exit $run2.ExitCode
}
Write-Host "async OK: await-split term=7 + pending_returns + interleave exit=0"

param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [string]$Zig = ""
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "dci_zig_abi: SKIP (fixture requires zig x86_64-windows-msvc)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot ".cache"
$contracts = Join-Path $projectRoot "contracts"
$nativeSrc = Join-Path $projectRoot "native\lib.zig"
$dcib = Join-Path $contracts "native.dcib"
$nativeLib = Join-Path $projectRoot "target\native.lib"
$exe = Join-Path $projectRoot "target\dci_zig_abi.exe"
$dciCli = Join-Path $repoRoot "tools\dci\dci.py"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($Zig)) {
    $Zig = (Get-Command zig -ErrorAction Stop).Source
}
$Zig = (Resolve-Path $Zig).Path

if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = (Resolve-Path $RuntimeDir).Path
    $env:Path = $RuntimeDir + ";" + $env:Path
}

if ([string]::IsNullOrWhiteSpace($env:LLVM_ROOT)) {
    $env:LLVM_ROOT = Join-Path $repoRoot "clang"
}

New-Item -ItemType Directory -Force -Path $cache | Out-Null
New-Item -ItemType Directory -Force -Path $contracts | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot "target") | Out-Null

function Fail-Test {
    param([string]$Message)
    Write-Host "FAILED: $Message"
    exit 1
}

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        Fail-Test $Message
    }
}

function Invoke-Checked {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$Name)
    $out = Join-Path $cache ($Name + ".out.log")
    $err = Join-Path $cache ($Name + ".err.log")
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $FilePath @ArgumentList > $out 2> $err
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) {
        Write-Host "FAILED: $Name"
        if (Test-Path $out) { Get-Content $out -Tail 80 }
        if (Test-Path $err) { Get-Content $err -Tail 80 }
        exit $code
    }
}

$python = (Get-Command python -ErrorAction Stop).Source

Invoke-Checked -FilePath $Zig -ArgumentList @(
    "build-lib", $nativeSrc,
    "-O", "ReleaseFast",
    "-target", "x86_64-windows-msvc",
    "-static",
    "--name", "native",
    "-fcompiler-rt",
    "-lc",
    "-femit-bin=$nativeLib"
) -Name "zig_staticlib"

Assert-Test (Test-Path -LiteralPath $nativeLib) "zig did not emit target\native.lib"

Invoke-Checked -FilePath $python -ArgumentList @(
    $dciCli, "adapter", "--language", "zig", $nativeSrc,
    "--crate-name", "native",
    "--zig", $Zig,
    "--triplet", "x64_windows",
    "--zig-arg=-O", "--zig-arg=ReleaseFast", "--zig-arg=-lc",
    "--artifact", $nativeLib,
    "-o", $dcib
) -Name "adapter_zig_o3"

Assert-Test (Test-Path -LiteralPath $dcib) "Zig O3 native.dcib was not generated"

$dcibDecode = @'
import json
import sys
from pathlib import Path
from tools.dci.dcib import decode

print(json.dumps(decode(Path(sys.argv[1]).read_bytes())))
'@
Push-Location $repoRoot
try {
    Invoke-Checked -FilePath $python -ArgumentList @("-c", $dcibDecode, $dcib) -Name "decode_zig_dcib"
} finally {
    Pop-Location
}

$abi = Get-Content -Raw -LiteralPath (Join-Path $cache "decode_zig_dcib.out.log") | ConvertFrom-Json
Assert-Test ($abi.source.language -eq "zig") "Zig contract source.language is not zig"
$flags = (($abi.source.compiler.flags) -join " ")
Assert-Test ($flags.IndexOf("ReleaseFast", [StringComparison]::Ordinal) -ge 0) "Zig contract is missing ReleaseFast"
$pair = @($abi.exports.layouts | Where-Object { $_.type_name -eq "native.Pair" })
Assert-Test ($pair.Count -eq 1) "Zig contract is missing native.Pair"
Assert-Test ($pair[0].size -eq 8) "native.Pair size is not the zig-measured 8 bytes"
$pairSum = @($abi.exports.symbols | Where-Object { $_.link_name -eq "pair_sum" })
Assert-Test ($pairSum.Count -eq 1) "Zig contract is missing pair_sum"
$cMul = @($abi.exports.symbols | Where-Object { $_.link_name -eq "c_mul" })
Assert-Test ($cMul.Count -eq 1) "Zig contract is missing c_mul"
$nativeLayout = @($abi.exports.layouts | Where-Object { $_.type_name -eq "native.Native" })
Assert-Test ($nativeLayout.Count -eq 1) "Zig contract is missing native.Native"
Assert-Test ($nativeLayout[0].size -eq 16) "native.Native size is not the zig-measured 16 bytes"
Assert-Test ($nativeLayout[0].representation -eq "native") "native.Native representation is not native"
$nativeFields = @{}
foreach ($field in $nativeLayout[0].fields) { $nativeFields[$field.name] = $field.offset }
Assert-Test ($nativeFields["wide"] -eq 0) "native.Native.wide is not at zig-measured offset 0"
Assert-Test ($nativeFields["flag"] -eq 8) "native.Native.flag is not at zig-measured offset 8"
$nativeHeap = @($abi.exports.symbols | Where-Object { $_.link_name -eq "native_heap" })
Assert-Test ($nativeHeap.Count -eq 1) "Zig contract is missing native_heap"
$nativeFlag = @($abi.exports.symbols | Where-Object { $_.link_name -eq "native_flag" })
Assert-Test ($nativeFlag.Count -eq 1) "Zig contract is missing native_flag"

Push-Location $projectRoot
try {
    Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList @("build", "-j", "10") -Name "project_build"
} finally {
    Pop-Location
}

Assert-Test (Test-Path -LiteralPath $exe) "project build did not produce target\dci_zig_abi.exe"

$runOut = Join-Path $cache "run_exe.out.log"
$runErr = Join-Path $cache "run_exe.err.log"
& $exe > $runOut 2> $runErr
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: run_exe (exit $LASTEXITCODE)"
    Get-Content $runOut -Tail 80
    Get-Content $runErr -Tail 80
    exit $LASTEXITCODE
}
$actual = ((Get-Content $runOut) -join "`n").Trim()
Assert-Test ($actual -eq "dci_zig_abi OK") "unexpected executable output: $actual"

Write-Host "dci_zig_abi: OK"

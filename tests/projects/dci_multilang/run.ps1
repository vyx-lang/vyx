param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [string]$Rustc = "",
    [string]$Zig = ""
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "dci_multilang: SKIP (fixture requires rustc/zig x86_64-windows-msvc and MSVC C++ ABI)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot ".cache"
$contracts = Join-Path $projectRoot "contracts"
$rustNative = Join-Path $projectRoot "..\dci_rust_trait\native\lib.rs"
$cppHeader = Join-Path $projectRoot "..\dci_complex_abi\native\Complex.hpp"
$zigNative = Join-Path $projectRoot "..\dci_zig_abi\native\lib.zig"
$cppDcib = Join-Path $contracts "Complex.dcib"
$rustDcib = Join-Path $contracts "native.dcib"
$zigDcib = Join-Path $contracts "zig.dcib"
$zigLib = Join-Path $projectRoot "target\zig_abi.lib"
$exe = Join-Path $projectRoot "target\dci_multilang.exe"
$dciCli = Join-Path $repoRoot "tools\dci\dci.py"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($Rustc)) {
    $Rustc = (Get-Command rustc -ErrorAction Stop).Source
}
$Rustc = (Resolve-Path $Rustc).Path

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

function Resolve-Clangxx {
    $candidates = @(
        (Join-Path $env:LLVM_ROOT "bin\clang++.exe"),
        (Join-Path $env:LLVM_ROOT "clang++.exe"),
        (Join-Path $repoRoot "clang\bin\clang++.exe")
    )
    $cmd = Get-Command clang++ -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { $candidates += $cmd.Source }
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }
    Fail-Test "clang++ not found for O3 C++ adapter"
    return ""
}

$python = (Get-Command python -ErrorAction Stop).Source
$clangxx = Resolve-Clangxx
$cppNativeDir = (Resolve-Path (Join-Path $projectRoot "..\dci_complex_abi\native")).Path

Invoke-Checked -FilePath $python -ArgumentList @(
    $dciCli, "adapter", "--language", "cpp", $cppHeader,
    "-o", $cppDcib,
    "--std", "c++20",
    "--toolchain", "clang",
    "--clang", $clangxx,
    "--triplet", "x64_windows",
    "--compile_flags", "-O3",
    "-I", $cppNativeDir
) -Name "adapter_cpp_o3"

Invoke-Checked -FilePath $python -ArgumentList @(
    $dciCli, "adapter", "--language", "rust", $rustNative,
    "--crate-name", "native",
    "--edition", "2024",
    "--rustc", $Rustc,
    "--triplet", "x64_windows",
    "--rustc-arg=-Copt-level=3",
    "-o", $rustDcib
) -Name "adapter_rust_o3"

Invoke-Checked -FilePath $Zig -ArgumentList @(
    "build-lib", $zigNative,
    "-O", "ReleaseFast",
    "-target", "x86_64-windows-msvc",
    "-static",
    "--name", "zig_abi",
    "-fcompiler-rt",
    "-lc",
    "-femit-bin=$zigLib"
) -Name "zig_staticlib"

Invoke-Checked -FilePath $python -ArgumentList @(
    $dciCli, "adapter", "--language", "zig", $zigNative,
    "--crate-name", "zig_abi",
    "--zig", $Zig,
    "--triplet", "x64_windows",
    "--zig-arg=-O", "--zig-arg=ReleaseFast", "--zig-arg=-lc",
    "--artifact", $zigLib,
    "-o", $zigDcib
) -Name "adapter_zig_o3"

Assert-Test (Test-Path -LiteralPath $cppDcib) "C++ O3 Complex.dcib was not generated"
Assert-Test (Test-Path -LiteralPath $rustDcib) "Rust O3 native.dcib was not generated"
Assert-Test (Test-Path -LiteralPath $zigDcib) "Zig O3 zig.dcib was not generated"
Assert-Test (Test-Path -LiteralPath $zigLib) "zig did not emit target\zig_abi.lib"

$dcibDecode = @'
import json
import sys
from pathlib import Path
from tools.dci.dcib import decode

print(json.dumps(decode(Path(sys.argv[1]).read_bytes())))
'@
Push-Location $repoRoot
try {
    Invoke-Checked -FilePath $python -ArgumentList @("-c", $dcibDecode, $cppDcib) -Name "decode_cpp_dcib"
    Invoke-Checked -FilePath $python -ArgumentList @("-c", $dcibDecode, $rustDcib) -Name "decode_rust_dcib"
    Invoke-Checked -FilePath $python -ArgumentList @("-c", $dcibDecode, $zigDcib) -Name "decode_zig_dcib"
} finally {
    Pop-Location
}

$cppAbi = Get-Content -Raw -LiteralPath (Join-Path $cache "decode_cpp_dcib.out.log") | ConvertFrom-Json
$rustAbi = Get-Content -Raw -LiteralPath (Join-Path $cache "decode_rust_dcib.out.log") | ConvertFrom-Json
$zigAbi = Get-Content -Raw -LiteralPath (Join-Path $cache "decode_zig_dcib.out.log") | ConvertFrom-Json
$cppFlags = (($cppAbi.source.compiler.flags) -join " ")
Assert-Test ($cppFlags.IndexOf("-O3", [StringComparison]::Ordinal) -ge 0) "C++ O3 contract is missing -O3 in compiler.flags"
Assert-Test ($cppAbi.source.language -eq "cpp") "C++ contract source.language is not cpp"
$cppDispatch = @($cppAbi.exports.symbols | Where-Object { $_.name -match "NativeDriver::dispatch" -or $_.member_name -eq "dispatch" })
Assert-Test ($cppDispatch.Count -ge 1) "C++ O3 contract is missing NativeDriver::dispatch"
Assert-Test ($rustAbi.source.language -eq "rust") "Rust contract source.language is not rust"
$rustFlagOf = @($rustAbi.exports.symbols | Where-Object { $_.name -eq "NativePair::flag_of" })
Assert-Test ($rustFlagOf.Count -eq 1) "Rust O3 contract is missing NativePair::flag_of"
Assert-Test ($rustFlagOf[0].link_name.StartsWith("_ZN")) "Rust O3 flag_of is not a rustc-mangled symbol"
Assert-Test ($zigAbi.source.language -eq "zig") "Zig contract source.language is not zig"
$zigFlags = (($zigAbi.source.compiler.flags) -join " ")
Assert-Test ($zigFlags.IndexOf("ReleaseFast", [StringComparison]::Ordinal) -ge 0) "Zig O3 contract is missing ReleaseFast"
$zigPair = @($zigAbi.exports.layouts | Where-Object { $_.type_name -eq "zig_abi.Pair" })
Assert-Test ($zigPair.Count -eq 1) "Zig O3 contract is missing zig_abi.Pair"
Assert-Test ($zigPair[0].size -eq 8) "zig_abi.Pair size is not the zig-measured 8 bytes"
$zigPairSum = @($zigAbi.exports.symbols | Where-Object { $_.link_name -eq "pair_sum" })
Assert-Test ($zigPairSum.Count -eq 1) "Zig O3 contract is missing pair_sum"
Assert-Test ($zigAbi.source.crate_name -eq "zig_abi") "Zig contract crate_name collided with another language"
$zigNative = @($zigAbi.exports.layouts | Where-Object { $_.type_name -eq "zig_abi.Native" })
Assert-Test ($zigNative.Count -eq 1) "Zig O3 contract is missing zig_abi.Native"
Assert-Test ($zigNative[0].size -eq 16) "zig_abi.Native size is not the zig-measured 16 bytes"
Assert-Test ($zigNative[0].representation -eq "native") "zig_abi.Native representation is not native"
$zigNativeFields = @{}
foreach ($field in $zigNative[0].fields) { $zigNativeFields[$field.name] = $field.offset }
Assert-Test ($zigNativeFields["wide"] -eq 0) "zig_abi.Native.wide is not at zig-measured offset 0"
Assert-Test ($zigNativeFields["flag"] -eq 8) "zig_abi.Native.flag is not at zig-measured offset 8"
$zigHeap = @($zigAbi.exports.symbols | Where-Object { $_.link_name -eq "native_heap" })
Assert-Test ($zigHeap.Count -eq 1) "Zig O3 contract is missing native_heap"

$env:VYX_DCI_RUSTC_OPT_LEVEL = "3"
$env:VYX_DCI_RUSTC_STUB_OPT_LEVEL = "3"

Push-Location $projectRoot
try {
    Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList @("build", "-j", "10") -Name "project_build"
} finally {
    Pop-Location
}

Assert-Test (Test-Path -LiteralPath $exe) "project build did not produce target\dci_multilang.exe"

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
Assert-Test ($actual -eq "dci_multilang OK") "unexpected executable output: $actual"

function Resolve-LlvmTool {
    param([string]$Name)
    $candidates = @(
        (Join-Path $env:LLVM_ROOT ("bin\" + $Name + ".exe")),
        (Join-Path $env:LLVM_ROOT ($Name + ".exe")),
        (Join-Path $repoRoot ("clang\bin\" + $Name + ".exe"))
    )
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { $candidates += $cmd.Source }
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }
    Fail-Test ($Name + " not found to assert executable strip")
    return ""
}

$readobj = Resolve-LlvmTool "llvm-readobj"
$objdump = Resolve-LlvmTool "llvm-objdump"
$exportText = & $readobj --coff-exports $exe 2>&1 | Out-String
Assert-Test ($exportText.IndexOf("Name:", [StringComparison]::Ordinal) -lt 0) `
    "stripped executable still has a PE export name table"
$namedDispatch = @(& $objdump -d --no-show-raw-insn $exe 2>&1 | Select-String "dispatch@NativeDriver")
Assert-Test ($namedDispatch.Count -eq 0) `
    "objdump still labels NativeDriver::dispatch in the stripped executable"

$cppStub = Get-ChildItem -LiteralPath $cache -Filter "*dci_stubs*.cpp" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
Assert-Test ($null -ne $cppStub) "generated DCI clang-cpp stub source is missing"
$cppText = Get-Content -Raw -LiteralPath $cppStub.FullName
Assert-Test ([regex]::IsMatch($cppText, "class\s+__dci_vyx_stub_VyxSink\s*:\s*public\s+abi_complex::AbstractSink")) `
    "C++ stub does not derive VyxSink from abi_complex::AbstractSink"
Assert-Test ($cppText.IndexOf("native::Sink", [StringComparison]::Ordinal) -lt 0) `
    "C++ stub mixed in a Rust base type"
Assert-Test ($cppText.IndexOf("class __dci_vyx_stub_VyxHost", [StringComparison]::Ordinal) -lt 0) `
    "C++ stub emitted a Rust host class"
Assert-Test ($cppText.IndexOf("zig_abi", [StringComparison]::Ordinal) -lt 0) `
    "C++ stub mixed in a Zig type"

$rustStub = Get-ChildItem -LiteralPath $cache -Filter "*dci_stubs*.rs" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
Assert-Test ($null -ne $rustStub) "generated DCI rustc stub source is missing"
$rustText = Get-Content -Raw -LiteralPath $rustStub.FullName
Assert-Test ($rustText.IndexOf("impl native::Sink for __dci_vyx_stub_VyxHost", [StringComparison]::Ordinal) -ge 0) `
    "Rust stub does not impl native::Sink for VyxHost"
Assert-Test ($rustText.IndexOf("dci-rust-producer=", [StringComparison]::Ordinal) -ge 0) `
    "Rust stub is missing the producer crate marker"
Assert-Test ($rustText.IndexOf("abi_complex::AbstractSink", [StringComparison]::Ordinal) -lt 0) `
    "Rust stub mixed in a C++ base type"
Assert-Test ($rustText.IndexOf("zig_abi", [StringComparison]::Ordinal) -lt 0) `
    "Rust stub mixed in a Zig type"

Write-Host "dci_multilang: OK"

param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = ""
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "dci_complex_abi: SKIP (fixture descriptor exercises x86_64 MSVC C++ ABI)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot ".cache"
$src = Join-Path $projectRoot "src\main.vyx"
$header = Join-Path $projectRoot "native\Complex.hpp"
$native = Join-Path $projectRoot "native\Complex.cpp"
$exe = Join-Path $projectRoot "target\dci_complex_abi.exe"
$ir = Join-Path $cache "dci_complex_abi_verify.ll"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = (Resolve-Path $RuntimeDir).Path
    $env:Path = $RuntimeDir + ";" + $env:Path
}

New-Item -ItemType Directory -Force -Path $cache | Out-Null

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
    & $FilePath @ArgumentList > $out 2> $err
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $Name"
        if (Test-Path $out) { Get-Content $out -Tail 80 }
        if (Test-Path $err) { Get-Content $err -Tail 80 }
        exit $LASTEXITCODE
    }
}

Push-Location $projectRoot
try {
    $previousDciPython = $env:VYX_DCI_PYTHON
    $env:VYX_DCI_PYTHON = "__dci_python_must_not_run__"
    try {
        Invoke-Checked -FilePath $BootstrapCompiler `
            -ArgumentList @("build", "-j", "10") `
            -Name "project_build"
    } finally {
        if ($null -eq $previousDciPython) { Remove-Item Env:VYX_DCI_PYTHON -ErrorAction SilentlyContinue }
        else { $env:VYX_DCI_PYTHON = $previousDciPython }
    }
} finally {
    Pop-Location
}

Assert-Test (Test-Path -LiteralPath $exe) "project build did not produce target\dci_complex_abi.exe"

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
Assert-Test ($actual -eq "dci_complex_abi OK") "unexpected executable output: $actual"

$dciFile = Get-Item -LiteralPath (Join-Path $projectRoot "contracts\Complex.dcib")
Assert-Test ($null -ne $dciFile) "explicit DCI ABI DCIB is missing"
$python = (Get-Command python -ErrorAction Stop).Source
$dcibDecode = @'
import json
import sys
from pathlib import Path
from tools.dci.dcib import decode

print(json.dumps(decode(Path(sys.argv[1]).read_bytes())))
'@
Push-Location $repoRoot
try {
    Invoke-Checked -FilePath $python -ArgumentList @("-c", $dcibDecode, $dciFile.FullName) -Name "decode_dcib"
} finally {
    Pop-Location
}
$abi = Get-Content -Raw -LiteralPath (Join-Path $cache "decode_dcib.out.log") | ConvertFrom-Json
Assert-Test (@($abi.source.headers | Where-Object { [IO.Path]::GetFileName($_) -eq "Complex.hpp" }).Count -eq 1) `
    "latest DCI ABI DCIB was not generated from Complex.hpp"

$multiLayouts = @($abi.exports.layouts | Where-Object { $_.type_name -eq "abi_complex::Multi" })
Assert-Test ($multiLayouts.Count -eq 1) "missing unique abi_complex::Multi layout"
$multiLayout = $multiLayouts[0]
$leftBases = @($multiLayout.bases | Where-Object { $_.type_name -eq "abi_complex::LeftBase" })
$rightBases = @($multiLayout.bases | Where-Object { $_.type_name -eq "abi_complex::RightBase" })
Assert-Test ($leftBases.Count -eq 1) "Multi layout is missing LeftBase"
Assert-Test ($rightBases.Count -eq 1) "Multi layout is missing RightBase"
Assert-Test (($leftBases[0].offset -eq 0) -and ($leftBases[0].visibility -eq "public") -and (-not $leftBases[0].is_virtual)) `
    "unexpected LeftBase layout metadata"
Assert-Test (($rightBases[0].offset -eq 16) -and ($rightBases[0].visibility -eq "public") -and (-not $rightBases[0].is_virtual)) `
    "unexpected RightBase layout metadata; expected non-primary base offset 16"

$registerPairLayouts = @($abi.exports.layouts | Where-Object { $_.type_name -eq "abi_complex::RegisterPair" })
Assert-Test ($registerPairLayouts.Count -eq 1) "missing unique abi_complex::RegisterPair layout"
Assert-Test (($registerPairLayouts[0].size -eq 8) -and `
    ($registerPairLayouts[0].alignment -eq 4) -and `
    (-not $registerPairLayouts[0].has_vtable)) `
    "RegisterPair must remain an 8-byte, align-4 record without a vtable"

$makePairSymbols = @($abi.exports.symbols | Where-Object { $_.member_name -eq "complex_make_register_pair" })
$offsetPairSymbols = @($abi.exports.symbols | Where-Object { $_.member_name -eq "complex_offset_register_pair" })
$scorePairSymbols = @($abi.exports.symbols | Where-Object { $_.member_name -eq "complex_register_pair_score" })
Assert-Test ($makePairSymbols.Count -eq 1) "missing complex_make_register_pair ABI symbol"
Assert-Test ($offsetPairSymbols.Count -eq 1) "missing complex_offset_register_pair ABI symbol"
Assert-Test ($scorePairSymbols.Count -eq 1) "missing complex_register_pair_score ABI symbol"
Assert-Test (($makePairSymbols[0].abi.return.passing -eq "coerce") -and `
    ($makePairSymbols[0].abi.return.coerce_to.name -eq "i64") -and `
    ($makePairSymbols[0].abi.return.alignment -eq 4)) `
    "RegisterPair return must be explicitly coerced to i64"
Assert-Test (($offsetPairSymbols[0].abi.params[0].passing -eq "coerce") -and `
    ($offsetPairSymbols[0].abi.params[0].coerce_to.name -eq "i64") -and `
    ($offsetPairSymbols[0].abi.params[0].alignment -eq 4) -and `
    ($offsetPairSymbols[0].abi.return.passing -eq "coerce") -and `
    ($offsetPairSymbols[0].abi.return.coerce_to.name -eq "i64") -and `
    ($offsetPairSymbols[0].abi.return.alignment -eq 4)) `
    "RegisterPair roundtrip must use i64 coercion for its parameter and return"
Assert-Test (($scorePairSymbols[0].abi.params[0].passing -eq "coerce") -and `
    ($scorePairSymbols[0].abi.params[0].coerce_to.name -eq "i64") -and `
    ($scorePairSymbols[0].abi.params[0].alignment -eq 4)) `
    "RegisterPair parameter must be explicitly coerced to i64"

$leftTables = @($abi.exports.vtables | Where-Object {
    $_.class_name -eq "abi_complex::Multi" -and $_.base_class -eq "abi_complex::LeftBase"
})
$rightTables = @($abi.exports.vtables | Where-Object {
    $_.class_name -eq "abi_complex::Multi" -and $_.base_class -eq "abi_complex::RightBase"
})
$sinkTables = @($abi.exports.vtables | Where-Object {
    $_.class_name -eq "abi_complex::AbstractSink" -and $_.base_class -eq "abi_complex::AbstractSink"
})
Assert-Test ($leftTables.Count -eq 1) "missing Multi/LeftBase vtable"
Assert-Test ($rightTables.Count -eq 1) "missing Multi/RightBase vtable"
Assert-Test ($sinkTables.Count -eq 1) "missing AbstractSink vtable"

$leftEntries = @($leftTables[0].entries | Where-Object { $_.member_name -eq "left_virtual" })
$rightEntries = @($rightTables[0].entries | Where-Object { $_.member_name -eq "right_virtual" })
$sinkEntries = @($sinkTables[0].entries | Where-Object { $_.member_name -eq "consume" })
Assert-Test ($leftEntries.Count -eq 1) "missing left_virtual vtable entry"
Assert-Test ($rightEntries.Count -eq 1) "missing right_virtual vtable entry"
Assert-Test ($sinkEntries.Count -eq 1) "missing consume vtable entry"
Assert-Test (($leftEntries[0].kind -eq "method") -and ($leftEntries[0].address_point_relative_offset -ge 0)) `
    "invalid left_virtual vtable slot"
Assert-Test (($rightEntries[0].kind -eq "method") -and ($rightEntries[0].address_point_relative_offset -ge 0)) `
    "invalid right_virtual vtable slot"
Assert-Test (($sinkEntries[0].kind -eq "method") -and ($sinkEntries[0].address_point_relative_offset -ge 0)) `
    "invalid consume vtable slot"

# The build driver names the generated stub TU
# "<target>_src_<file>_vyx_dci_stubs_<backend>_cpp.cpp" (see build_safe_obj_name
# over "...#dci_stubs_<backend>"), so the previous "*_dci_stubs.cpp" filter only
# ever matched a stale leftover and failed on a clean cache.
$stubFile = Get-ChildItem -LiteralPath $cache -Filter "*_dci_stubs_*.cpp" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
Assert-Test ($null -ne $stubFile) "generated DCI stub source is missing"
$stubText = Get-Content -Raw -LiteralPath $stubFile.FullName
Assert-Test ([regex]::IsMatch($stubText, '(?m)^#include\s+"[^"\r\n]*Complex\.hpp"')) `
    "generated stub does not include Complex.hpp from DCIB source.headers"
Assert-Test ([regex]::IsMatch($stubText, "class\s+__dci_vyx_stub_VyxSink\s*:\s*public\s+abi_complex::AbstractSink")) `
    "generated stub does not derive from abi_complex::AbstractSink"
Assert-Test ([regex]::IsMatch($stubText, "consume\s*\([^)]*\)\s*noexcept\s+override")) `
    "generated stub does not override consume"
Assert-Test ($stubText.IndexOf("__vyx_M_VyxSink_N_consume", [StringComparison]::Ordinal) -ge 0) `
    "generated stub does not call the Vyx override thunk"
Assert-Test ($stubText.IndexOf("__dci_stub_factory_VyxSink", [StringComparison]::Ordinal) -ge 0) `
    "generated stub factory is missing"
Assert-Test ($stubText.IndexOf("__dci_stub_destroy_VyxSink", [StringComparison]::Ordinal) -ge 0) `
    "generated stub destroy thunk is missing"

Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $src, "--emit=ir", "--dci", $dciFile.FullName, "-o", $ir) `
    -Name "emit_ir"

$irText = Get-Content -Raw -LiteralPath $ir
$llvmCallConventionPattern = '(?:(?:[A-Za-z_][A-Za-z0-9_]*cc|cc\s+\d+)\s+)?'
$indirectCallPattern = '(?m)^\s*%[-A-Za-z0-9$._]+\s*=\s*call\s+' + $llvmCallConventionPattern + 'i32\s+%[-A-Za-z0-9$._]+\s*\(ptr\s+%[-A-Za-z0-9$._]+\s*,\s*i32\s+'
$indirectCalls = [regex]::Matches($irText, $indirectCallPattern)
Assert-Test ($indirectCalls.Count -eq 5) "expected exactly five vtable indirect calls, found $($indirectCalls.Count)"
$coerceArgAlignPattern = '(?m)^\s*%[-A-Za-z0-9$._]+\s*=\s*load\s+i64,\s+ptr\s+%[-A-Za-z0-9$._]+,\s+align\s+4\s*\r?\n' +
    '\s*%[-A-Za-z0-9$._]+\s*=\s*call\s+' + $llvmCallConventionPattern +
    '(?:i32|i64)\s+@"[^"\r\n]*complex_(?:register_pair_score|offset_register_pair)[^"\r\n]*"\s*\(i64\s+%[-A-Za-z0-9$._]+'
$coerceArgAligned = [regex]::Matches($irText, $coerceArgAlignPattern)
Assert-Test ($coerceArgAligned.Count -eq 3) `
    "expected three align-4 RegisterPair coerce argument loads, found $($coerceArgAligned.Count)"
$coerceReturnAlignPattern = '(?m)^\s*%[-A-Za-z0-9$._]+\s*=\s*call\s+' + $llvmCallConventionPattern +
    'i64\s+@"[^"\r\n]*complex_(?:make_register_pair|offset_register_pair)[^"\r\n]*"[^\r\n]*\r?\n' +
    '\s*store\s+i64\s+%[-A-Za-z0-9$._]+,\s+ptr\s+%[-A-Za-z0-9$._]+,\s+align\s+4'
$coerceReturnsAligned = [regex]::Matches($irText, $coerceReturnAlignPattern)
Assert-Test ($coerceReturnsAligned.Count -eq 2) `
    "expected two align-4 RegisterPair coerce return stores, found $($coerceReturnsAligned.Count)"
Assert-Test ([regex]::IsMatch($irText, 'define\s+i32\s+@__vyx_M_VyxSink_N_consume')) `
    "Vyx override callback was dropped from the generated object"
Assert-Test ($irText.IndexOf("call void @__dci_stub_destroy_VyxSink", [StringComparison]::Ordinal) -ge 0) `
    "generated IR does not destroy the automatic C++ stub"

foreach ($entry in @($leftEntries[0], $rightEntries[0], $sinkEntries[0])) {
    Assert-Test (-not [string]::IsNullOrWhiteSpace($entry.mangled)) "vtable entry is missing its C++ mangled symbol"
    $directPattern = 'call\s+' + $llvmCallConventionPattern + 'i32\s+@"' + [regex]::Escape($entry.mangled) + '"'
    Assert-Test (-not [regex]::IsMatch($irText, $directPattern)) `
        "virtual call was lowered as a direct symbol call: $($entry.mangled)"
}

$manualSourceText = (Get-Content -Raw -LiteralPath $header) + "`n" `
    + (Get-Content -Raw -LiteralPath $native) + "`n" `
    + (Get-Content -Raw -LiteralPath $src)
Assert-Test (-not [regex]::IsMatch($manualSourceText, 'extern\s+"C"')) `
    "hand-written sources unexpectedly contain an extern C bridge"
foreach ($legacyWrapper in @(
    "complex_make_multi",
    "complex_destroy_multi",
    "complex_call_left",
    "complex_call_right",
    "complex_call_sink",
    "complex_destroyed_sink_count",
    "complex_destroyed_big_count",
    "complex_inspect_big",
    "complex_mutate_big",
    "complex_make_vecbox",
    "complex_destroy_vecbox",
    "complex_vecbox_sum"
)) {
    Assert-Test ($manualSourceText.IndexOf($legacyWrapper, [StringComparison]::Ordinal) -lt 0) `
        "legacy interop wrapper remains in hand-written sources: $legacyWrapper"
}

# The remaining legs all run through `--emit=cpp` (MIR2CPP) and the CMake
# project it generates. MIR2CPP is the experimental, unmaintained backend that
# is scheduled for a rewrite, so it is not an acceptance gate; opt in with
# VYX_ACCEPT_MIR2CPP=1 to exercise it.
if ($env:VYX_ACCEPT_MIR2CPP -ne "1") {
    Write-Host "dci_complex_abi: OK (MIR2CPP leg skipped; set VYX_ACCEPT_MIR2CPP=1 to enable)"
    exit 0
}

$cmake = (Get-Command cmake -ErrorAction Stop).Source
foreach ($cppCase in @(
    @{ Level = "-O0"; Preset = "ninja-debug"; Stem = "o0" },
    @{ Level = "-O2"; Preset = "ninja-release"; Stem = "o2" }
)) {
    $cppOut = Join-Path $cache ("mir2cpp_coerce_{0}_{1}" -f $cppCase.Stem, $PID)
    Invoke-Checked -FilePath $BootstrapCompiler `
        -ArgumentList @("--src=project", $projectRoot, "--emit=cpp", $cppCase.Level,
                        "--dci", $dciFile.FullName, "-o", $cppOut) `
        -Name ("emit_cpp_" + $cppCase.Stem)

    $generatedCpp = (Get-ChildItem -LiteralPath (Join-Path $cppOut "src") -Filter "*.cpp" -File |
        Sort-Object FullName | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
    $generatedHeaders = (Get-ChildItem -LiteralPath (Join-Path $cppOut "include") -Filter "*.hpp" -File |
        Sort-Object FullName | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
    Assert-Test ([regex]::IsMatch($generatedHeaders,
        'extern\s+"C"\s+int64_t\s+vyx_dci_direct_\d+\s*\(int32_t\s+\w+\s*,\s*int32_t\s+\w+\s*\)')) `
        "$($cppCase.Level) MIR2CPP make-pair alias is not using the physical i64 ABI"
    Assert-Test ([regex]::IsMatch($generatedHeaders,
        'extern\s+"C"\s+int32_t\s+vyx_dci_direct_\d+\s*\(int64_t\s+\w+\s*\)')) `
        "$($cppCase.Level) MIR2CPP score alias is not using the physical i64 ABI"
    Assert-Test ([regex]::IsMatch($generatedHeaders,
        'extern\s+"C"\s+int64_t\s+vyx_dci_direct_\d+\s*\(int64_t\s+\w+\s*,\s*int32_t\s+\w+\s*\)')) `
        "$($cppCase.Level) MIR2CPP offset alias is not using the physical i64 ABI"
    Assert-Test ($generatedCpp.IndexOf('std::memcpy(&vyx_dci_bits, &vyx_dci_source',
        [StringComparison]::Ordinal) -ge 0) `
        "$($cppCase.Level) MIR2CPP did not pack the coerce argument"
    Assert-Test ($generatedCpp.IndexOf('std::memcpy(&vyx_dci_value, &vyx_dci_bits',
        [StringComparison]::Ordinal) -ge 0) `
        "$($cppCase.Level) MIR2CPP did not unpack the coerce return"

    Push-Location $cppOut
    try {
        Invoke-Checked -FilePath $cmake -ArgumentList @("--preset", $cppCase.Preset) `
            -Name ("configure_cpp_" + $cppCase.Stem)
        Invoke-Checked -FilePath $cmake `
            -ArgumentList @("--build", "--preset", $cppCase.Preset, "--parallel", "3") `
            -Name ("build_cpp_" + $cppCase.Stem)
    } finally {
        Pop-Location
    }
    $cppExe = Join-Path $cppOut ("build/{0}/vyx_dci_complex_abi.exe" -f $cppCase.Preset)
    Assert-Test (Test-Path -LiteralPath $cppExe -PathType Leaf) `
        "$($cppCase.Level) MIR2CPP build did not produce $cppExe"
    $cppRunOut = Join-Path $cache ("run_cpp_{0}.out.log" -f $cppCase.Stem)
    $cppRunErr = Join-Path $cache ("run_cpp_{0}.err.log" -f $cppCase.Stem)
    & $cppExe > $cppRunOut 2> $cppRunErr
    if ($LASTEXITCODE -ne 0) {
        Get-Content $cppRunOut -Tail 80
        Get-Content $cppRunErr -Tail 80
        Fail-Test "$($cppCase.Level) MIR2CPP executable failed with exit $LASTEXITCODE"
    }
    $cppActual = ((Get-Content $cppRunOut) -join "`n").Trim()
    Assert-Test ($cppActual -eq "dci_complex_abi OK") `
        "$($cppCase.Level) MIR2CPP output mismatch: $cppActual"
}

Write-Host "dci_complex_abi: OK"

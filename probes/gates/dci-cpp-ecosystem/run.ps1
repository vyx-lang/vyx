param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { Write-Output 'dci-cpp-ecosystem: Windows ICU fixture required'; exit 77 }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$runtime = Join-Path $repo 'bootstrap_compiler/out'
$icu = Join-Path $repo 'Zyn/vendor/ICU'
$clang = Join-Path $repo 'clang/bin/clang++.exe'
$out = Join-Path $PSScriptRoot 'out'
$contracts = Join-Path $PSScriptRoot 'contracts'
New-Item -ItemType Directory -Force -Path $out, $contracts | Out-Null
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = "$env:LLVM_ROOT/bin;$runtime;$icu/bin/x64;$env:PATH"

$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'upstream.json') -Raw | ConvertFrom-Json
foreach ($entry in $pin.sha256.PSObject.Properties) {
    $source = Join-Path $icu $entry.Name
    if (!(Test-Path -LiteralPath $source)) { throw "Missing real ICU dependency: $source" }
    $actual = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ($actual -ne $entry.Value) { throw "ICU dependency changed; review upstream.json: $source" }
}
if (!(Select-String -LiteralPath (Join-Path $icu 'include/unicode/uvernum.h') -Pattern '^#define U_ICU_VERSION "78\.3"$' -Quiet)) {
    throw 'This fixture requires the pinned ICU 78.3 headers and binaries'
}

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments, [string]$Name)
    $stdout = Join-Path $out "$Name.stdout.log"
    $stderr = Join-Path $out "$Name.stderr.log"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $FilePath @Arguments > $stdout 2> $stderr
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) {
        Get-Content -LiteralPath $stdout -Tail 60
        Get-Content -LiteralPath $stderr -Tail 60
        throw "$Name failed: $code"
    }
    return $stdout
}

$adapter = Join-Path $repo 'tools/dci/dci_adapter_msvc.py'
$descriptor = Join-Path $contracts 'icu.dcib'
$contractJson = Join-Path $out 'icu.json'
$adapterArgs = @($adapter, '--include', (Join-Path $PSScriptRoot 'icu.hpp'),
    '--project-root', $PSScriptRoot, '--out', $descriptor,
    '--debug-json-out', $contractJson, '--boundary', 'shared_abi',
    '--borrow-return', 'UnicodeString::append', '--borrow-return', 'UnicodeString::toUpper',
    '--borrow-return', 'UnicodeString::toLower', '--artifact', (Join-Path $icu 'lib/x64/icuuc78.lib'),
    '--target', 'x86_64-pc-windows-msvc', '--std', 'c++17', '-j', '4',
    '--', '-I', (Join-Path $icu 'include'))
Invoke-Checked python $adapterArgs 'adapter' | Out-Null
Invoke-Checked python @((Join-Path $PSScriptRoot 'check_contract.py'), $contractJson) 'contract' | Out-Null

# Run the actual Vyx project build, including its manifest/native library link.
Push-Location $PSScriptRoot
try { Invoke-Checked $Compiler @('build', '-j1') 'project' | Out-Null }
finally { Pop-Location }
$projectExe = Join-Path $PSScriptRoot 'target/dci_cpp_ecosystem.exe'
$projectLog = Invoke-Checked $projectExe @() 'project-run'
if (([IO.File]::ReadAllText($projectLog)).Trim() -ne 'DCI ICU 78.3 OK') { throw 'Unexpected project output' }

# Compile an independent original-API C++ reference; it exports no Vyx bridge.
$reference = Join-Path $out 'reference.exe'
Invoke-Checked $clang @((Join-Path $PSScriptRoot 'reference.cpp'), '-std=c++17', '-O2',
    "-I$icu/include", (Join-Path $icu 'lib/x64/icuuc78.lib'),
    '-fms-runtime-lib=static', '-fuse-ld=lld', '-o', $reference) 'reference-build' | Out-Null
$referenceLog = Invoke-Checked $reference @() 'reference-run'
$expected = ([IO.File]::ReadAllText($referenceLog)).Trim()
if ($expected -ne 'DCI ICU 78.3 OK') { throw 'Unexpected native reference output' }

foreach ($level in @('O0', 'O2')) {
    $ir = Join-Path $out "$level.ll"
    $exe = Join-Path $out "$level.exe"
    Invoke-Checked $Compiler @('--src=file', (Join-Path $PSScriptRoot 'main.vyx'), '--emit=ir', "-$level", '-o', $ir) "$level-ir" | Out-Null
    $text = [IO.File]::ReadAllText($ir)
    foreach ($original in @('??0UnicodeString@icu_78@@QEAA@HHH@Z', '??1UnicodeString@icu_78@@UEAA@XZ',
                            '?caseCompare@UnicodeString@icu_78@@QEBA', '?toUpper@UnicodeString@icu_78@@QEAA')) {
        if (!$text.Contains($original)) { throw "$level omitted original ICU symbol: $original" }
    }
    Invoke-Checked $clang @($ir, "-L$runtime", '-lvyx_runtime', '-ltbb12', '-lsynchronization', '-lws2_32',
        (Join-Path $icu 'lib/x64/icuuc78.lib'), '-fms-runtime-lib=static', '-fuse-ld=lld', '-o', $exe) "$level-link" | Out-Null
    $runLog = Invoke-Checked $exe @() "$level-run"
    if (([IO.File]::ReadAllText($runLog)).Trim() -ne $expected) { throw "$level differs from native reference" }
    Write-Output "$level ICU native-reference parity OK"
}
Write-Output 'dci-cpp-ecosystem: OK (ICU 78.3, real C++ DLL API, AOT)'

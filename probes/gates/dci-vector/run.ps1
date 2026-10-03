param([string]$Compiler = '')

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$runtime = Join-Path $repo 'bootstrap_compiler/out'
$llvm = Join-Path $repo 'clang'
$clang = Join-Path $llvm 'bin/clang++.exe'
$env:LLVM_ROOT = $llvm
$env:PATH = "$llvm/bin;$runtime;$env:PATH"
$run = Join-Path $PSScriptRoot 'out'
$cache = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.cache'))
New-Item -ItemType Directory -Force -Path $run | Out-Null

function Invoke-Checked {
    param([string]$Tool, [string[]]$Arguments, [string]$Name)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Tool @Arguments > (Join-Path $run "$Name.out.log") 2> (Join-Path $run "$Name.err.log")
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) {
        Get-Content (Join-Path $run "$Name.out.log") -Tail 60
        Get-Content (Join-Path $run "$Name.err.log") -Tail 60
        throw "$Name failed with exit $code"
    }
}

# Discard only this fixture's derived state. The cold build must discover all
# instantiations from Vyx; the header and adapter command list no closed type.
if ($cache -ne [IO.Path]::GetFullPath("$PSScriptRoot/.cache") -or
    !$cache.StartsWith([IO.Path]::GetFullPath($PSScriptRoot) + [IO.Path]::DirectorySeparatorChar)) {
    throw 'fixture cache path escaped the fixture'
}
if (Test-Path -LiteralPath $cache) { Remove-Item -LiteralPath $cache -Recurse -Force }

$python = (Get-Command python -ErrorAction Stop).Source
Invoke-Checked $python @((Join-Path $repo 'tools/dci/dci_adapter_msvc.py'),
    '--include', (Join-Path $PSScriptRoot 'vector.hpp'),
    '--out', (Join-Path $PSScriptRoot 'vector.dcib'),
    '--target', 'x86_64-pc-windows-msvc', '--boundary', 'shared_abi',
    '--borrow-template-return', 'std::vector::data',
    '--borrow-template-return', 'std::vector::at', '-j', '2') 'adapter'

Push-Location $PSScriptRoot
try { Invoke-Checked $Compiler @('build', '-j', '1') 'cold-build' }
finally { Pop-Location }
Invoke-Checked $python @((Join-Path $PSScriptRoot 'check_contract.py')) 'closed-contract'
Invoke-Checked (Join-Path $PSScriptRoot 'target/dci_vector.exe') @() 'cold-run'

$supplement = Join-Path $cache 'dci_closed_facts_dci_vector.dcib'
$stubs = Join-Path $cache 'dci_vector_main_vyx_dci_stubs_external.cpp'
if (!(Test-Path -LiteralPath $supplement) -or !(Test-Path -LiteralPath $stubs)) {
    throw 'cold build did not produce its discovered ABI contract and native materialization'
}
Invoke-Checked $clang @('-c', (Join-Path $PSScriptRoot 'native.cpp'), '-O2', '-std=c++17',
    '-fexceptions', '-fcxx-exceptions', '-fms-runtime-lib=static',
    '-mno-incremental-linker-compatible', '-o', (Join-Path $run 'native.obj')) 'native-harness'
foreach ($level in @('O0', 'O2')) {
    $ir = Join-Path $run "$level.ll"
    $hooked = Join-Path $run "$level.tracked.ll"
    $stubObject = Join-Path $run "$level.stubs.obj"
    $exe = Join-Path $run "$level.exe"
    Invoke-Checked $Compiler @('--src=file', (Join-Path $PSScriptRoot 'main.vyx'),
        '--dci', $supplement, '--emit=ir', "-$level", '-o', $ir) "$level-ir"
    $text = [IO.File]::ReadAllText($ir)
    if (!$text.Contains('dci_list_')) { throw "$level IR omitted the producer brace constructor" }
    # Intercept only DCI storage allocation calls in the Vyx IR. Native std
    # vectors keep their genuine constructors, methods, allocator and destructor.
    $text = $text.Replace('@malloc', '@tracked_malloc').Replace('@calloc', '@tracked_calloc').Replace('@free', '@tracked_free')
    [IO.File]::WriteAllText($hooked, $text)
    Invoke-Checked $clang @('-c', $stubs, "-$level", '-std=c++17', '-fexceptions',
        '-fcxx-exceptions', '-fms-runtime-lib=static', '-o', $stubObject) "$level-native-members"
    Invoke-Checked $clang @($hooked, (Join-Path $run 'native.obj'), $stubObject,
        "-L$runtime", '-lvyx_runtime', '-ltbb12', '-lsynchronization', '-lws2_32',
        '-fms-runtime-lib=static', '-fuse-ld=lld', '-o', $exe) "$level-link"
    Invoke-Checked $exe @() "$level-run"
    $actual = Get-Content (Join-Path $run "$level-run.out.log") -Raw
    if ($actual -notmatch 'comparisons=896 pushes=405504 exceptions=2 objects=132/132 std_allocations=(\d+)/\1 OK' -or
        $actual -notmatch 'dci_vector OK') { throw "$level did not report the complete measured stress result: $actual" }
    Write-Output "$level $(($actual -split "`r?`n")[0])"
}

$negative = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'narrowing'))
$negativeCache = [IO.Path]::GetFullPath((Join-Path $negative '.cache'))
$negativeOutput = [IO.Path]::GetFullPath((Join-Path $negative 'target'))
if ($negativeCache -ne [IO.Path]::GetFullPath("$PSScriptRoot/narrowing/.cache") -or
    !$negativeCache.StartsWith($negative + [IO.Path]::DirectorySeparatorChar)) {
    throw 'negative fixture cache path escaped its fixture'
}
if ($negativeOutput -ne [IO.Path]::GetFullPath("$PSScriptRoot/narrowing/target") -or
    !$negativeOutput.StartsWith($negative + [IO.Path]::DirectorySeparatorChar)) {
    throw 'negative fixture output path escaped its fixture'
}
if (Test-Path -LiteralPath $negativeCache) { Remove-Item -LiteralPath $negativeCache -Recurse -Force }
if (Test-Path -LiteralPath $negativeOutput) { Remove-Item -LiteralPath $negativeOutput -Recurse -Force }
Push-Location $negative
try {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Compiler build -j 1 > (Join-Path $run 'narrowing.out.log') 2> (Join-Path $run 'narrowing.err.log')
    $negativeCode = $LASTEXITCODE
    $ErrorActionPreference = $previous
} finally { Pop-Location }
$negativeText = [IO.File]::ReadAllText((Join-Path $run 'narrowing.out.log')) +
                [IO.File]::ReadAllText((Join-Path $run 'narrowing.err.log'))
$negativeContract = Join-Path $negativeCache 'dci_closed_facts_dci_vector_narrowing.dcib'
$negativeClosureLog = "$negativeContract.log"
if (!(Test-Path -LiteralPath $negativeClosureLog)) {
    throw 'negative build omitted the producer adapter diagnostic log'
}
$negativeText += [IO.File]::ReadAllText($negativeClosureLog)
if ($negativeCode -eq 0 -or $negativeText -notmatch 'cannot be narrowed|narrowing conversion') {
    throw "the producer failed to reject C++ brace narrowing: $negativeText"
}
if ((Test-Path -LiteralPath $negativeContract) -or
    (Test-Path -LiteralPath (Join-Path $negativeOutput 'dci_vector_narrowing.exe'))) {
    throw 'rejected brace narrowing produced closed ABI facts or an executable'
}
Write-Output 'C++ double-to-int brace narrowing rejected OK'

Push-Location $PSScriptRoot
try { Invoke-Checked $Compiler @('build', '-j', '1') 'warm-build' }
finally { Pop-Location }
Invoke-Checked (Join-Path $PSScriptRoot 'target/dci_vector.exe') @() 'warm-run'
Write-Output 'dci-vector: cold/warm discovery, C++ narrowing rejection and AOT O0/O2 native standard library stress OK'

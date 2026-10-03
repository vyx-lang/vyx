[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [string]$ClangCxx = "",
    [int]$Jobs = 10,
    [int]$TimeoutSec = 600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Jobs -lt 1) { throw "Jobs must be at least 1." }
if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) {
    throw "TimeoutSec must be between 1 and 600."
}
if ($env:OS -ne "Windows_NT") {
    Write-Host "mir2cpp_dci_direct_opaque: SKIP (fixture requires its x86_64-pc-windows-msvc DCI descriptor)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "..\..\..")).Path
. (Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1")

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($ClangCxx)) {
    $ClangCxx = Join-Path $repoRoot "clang\bin\clang++.exe"
}
$ClangCxx = (Resolve-Path -LiteralPath $ClangCxx).Path

$cmakeExe = (Get-Command cmake -ErrorAction Stop).Source
$ninjaExe = (Get-Command ninja -ErrorAction Stop).Source
$descriptor = (Resolve-Path -LiteralPath (Join-Path $projectRoot "contracts\OpaqueBox.dcib")).Path
$debugDescriptor = (Resolve-Path -LiteralPath (Join-Path $projectRoot "contracts\OpaqueBox.dci")).Path
$mainSource = (Resolve-Path -LiteralPath (Join-Path $projectRoot "src\main.vyx")).Path
$reassignNegativeSource = (Resolve-Path -LiteralPath (Join-Path $projectRoot "src\reassign_negative.vyx")).Path
$runRoot = Join-Path $projectRoot (".cache\run_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
$logsRoot = Join-Path $runRoot "logs"
$cppOut = Join-Path $runRoot "cpp"
New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null

function Invoke-CheckedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [int]$ExpectedExit = 0,
        [string]$ExpectedText = ""
    )

    $stdoutLog = Join-Path $logsRoot ($Name + ".stdout.log")
    $stderrLog = Join-Path $logsRoot ($Name + ".stderr.log")
    $dialogLog = Join-Path $logsRoot ($Name + ".dialog.log")
    $rows = @(Invoke-VyxProcess -FilePath $FilePath `
        -ArgumentList $ArgumentList `
        -WorkingDirectory $WorkingDirectory `
        -StdoutLog $stdoutLog `
        -StderrLog $stderrLog `
        -DialogLog $dialogLog `
        -TimeoutSec $TimeoutSec)
    if ($rows.Count -eq 0) { throw "$Name returned no process result" }
    $result = $rows[-1]
    $stdout = if (Test-Path -LiteralPath $stdoutLog) { Get-Content -Raw -LiteralPath $stdoutLog } else { "" }
    $stderr = if (Test-Path -LiteralPath $stderrLog) { Get-Content -Raw -LiteralPath $stderrLog } else { "" }
    $combined = $stdout + "`n" + $stderr
    if ($result.TimedOut) { throw "$Name timed out after $TimeoutSec seconds" }
    if ($result.ExitCode -ne $ExpectedExit) {
        throw "$Name expected exit $ExpectedExit, got $($result.ExitCode).`n$combined"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedText) -and
        $combined.IndexOf($ExpectedText, [StringComparison]::Ordinal) -lt 0) {
        throw "$Name did not emit '$ExpectedText'.`n$combined"
    }
    Write-Host "[$Name] exit=$ExpectedExit"
    return [pscustomobject]@{ Result = $result; Stdout = $stdout; Stderr = $stderr }
}

$expected = "mir2cpp_dci_direct_opaque OK"
[void](Invoke-CheckedProcess -Name "project_build" -FilePath $BootstrapCompiler `
    -ArgumentList @("build", "-j", "$Jobs") -WorkingDirectory $projectRoot)

$nativeExe = Join-Path $projectRoot "target\mir2cpp_dci_direct_opaque.exe"
if (-not (Test-Path -LiteralPath $nativeExe -PathType Leaf)) {
    throw "Native executable not found: $nativeExe"
}
[void](Invoke-CheckedProcess -Name "run_native" -FilePath $nativeExe `
    -WorkingDirectory $projectRoot -ExpectedText $expected)

[void](Invoke-CheckedProcess -Name "reassign_nested_negative" -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $reassignNegativeSource, "--emit=cpp", "--dci", $descriptor,
                    "-o", (Join-Path $runRoot "reassign_nested_cpp")) `
    -WorkingDirectory $projectRoot -ExpectedExit 1 `
    -ExpectedText "must initialize a final storage place directly")

$badDescriptor = Join-Path $runRoot "unsupported_abi.dci"
$descriptorDocument = Get-Content -Raw -LiteralPath $debugDescriptor | ConvertFrom-Json
$constructors = @($descriptorDocument.exports.symbols | Where-Object {
    $_.owner -eq "dci_opaque::OpaqueBox" -and $_.kind -eq "constructor"
})
if ($constructors.Count -ne 1) {
    throw "Expected exactly one OpaqueBox constructor in the debug descriptor"
}
$constructorReturn = $constructors[0].abi.return
if ($constructorReturn.passing -ne "direct" -or
    $constructorReturn.type.name -ne "dci_opaque::OpaqueBox" -or
    $constructorReturn.type.kind -ne "class" -or
    $constructorReturn.type.reference -ne "pointer") {
    throw "OpaqueBox constructor must use the x86_64 MSVC direct owner-pointer return ABI"
}
$descriptorDocument.schema.encoding = "json"
$constructors[0].abi.receiver.passing = "indirect"
$badJson = $descriptorDocument | ConvertTo-Json -Depth 100
[IO.File]::WriteAllText($badDescriptor, $badJson, [Text.UTF8Encoding]::new($false))

[void](Invoke-CheckedProcess -Name "unsupported_abi_negative" -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $mainSource, "--emit=cpp", "--dci", $badDescriptor,
                    "-o", (Join-Path $runRoot "unsupported_abi_cpp")) `
    -WorkingDirectory $projectRoot -ExpectedExit 1 `
    -ExpectedText "unsupported direct DCI ABI")

$staleConstructorDescriptor = Join-Path $runRoot "stale_constructor_abi.dci"
$constructors[0].abi.receiver.passing = "direct"
$constructors[0].abi.return.type = [pscustomobject]@{
    name = "void"
    kind = "primitive"
}
$staleConstructorJson = $descriptorDocument | ConvertTo-Json -Depth 100
[IO.File]::WriteAllText(
    $staleConstructorDescriptor,
    $staleConstructorJson,
    [Text.UTF8Encoding]::new($false))

[void](Invoke-CheckedProcess -Name "stale_constructor_abi_negative" -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $mainSource, "--emit=ir", "--dci", $staleConstructorDescriptor,
                    "-o", (Join-Path $runRoot "stale_constructor.ll")) `
    -WorkingDirectory $projectRoot -ExpectedExit 1 `
    -ExpectedText "must return its owner pointer directly")

[void](Invoke-CheckedProcess -Name "emit_cpp" -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=project", $projectRoot, "--emit=cpp", "--dci", $descriptor,
                    "-j", "$Jobs", "-o", $cppOut) `
    -WorkingDirectory $projectRoot)

$generatedFiles = @(Get-ChildItem -LiteralPath (Join-Path $cppOut "include") -File -Filter "*.hpp") +
                  @(Get-ChildItem -LiteralPath (Join-Path $cppOut "src") -File -Filter "*.cpp")
$generatedText = ($generatedFiles | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
if ($generatedText.IndexOf('#include "OpaqueBox.hpp"', [StringComparison]::Ordinal) -ge 0) {
    throw "Generated MIR2CPP code depends on the producer C++ header"
}
if ($generatedText.IndexOf("dci_opaque::OpaqueBox", [StringComparison]::Ordinal) -ge 0) {
    throw "Generated MIR2CPP code uses the producer's concrete C++ class type"
}
foreach ($typedCall in @(".valid(", ".value(", ".add(")) {
    if ($generatedText.IndexOf($typedCall, [StringComparison]::Ordinal) -ge 0) {
        throw "Generated MIR2CPP code contains typed C++ member call: $typedCall"
    }
}
foreach ($needle in @("alignas(8)", "storage[16]", "vyx_dci_direct_", "??0OpaqueBox@dci_opaque@@", "??1OpaqueBox@dci_opaque@@")) {
    if ($generatedText.IndexOf($needle, [StringComparison]::Ordinal) -lt 0) {
        throw "Generated MIR2CPP code is missing DCI direct-lowering marker: $needle"
    }
}
$constructorAliasMatch = [regex]::Match(
    $generatedText,
    '(?m)extern "C"[^\r\n]*(?<alias>vyx_dci_direct_[0-9]+)\([^\r\n]*\?\?0OpaqueBox@dci_opaque@@')
$destructorAliasMatch = [regex]::Match(
    $generatedText,
    '(?m)extern "C"[^\r\n]*(?<alias>vyx_dci_direct_[0-9]+)\([^\r\n]*\?\?1OpaqueBox@dci_opaque@@')
if (-not $constructorAliasMatch.Success -or -not $destructorAliasMatch.Success) {
    throw "Generated MIR2CPP declarations do not identify the OpaqueBox constructor/destructor aliases"
}
$constructorAlias = [regex]::Escape($constructorAliasMatch.Groups['alias'].Value)
$destructorAlias = [regex]::Escape($destructorAliasMatch.Groups['alias'].Value)
$reassignPattern = $destructorAlias +
                   '\(reinterpret_cast<void\*>\(reinterpret_cast<unsigned char\*>\(&\(vyx_local_box_(?<local>[0-9]+)\)\)\)\);\s*' +
                   $constructorAlias +
                   '\(reinterpret_cast<void\*>\(&\(vyx_local_box_\k<local>\)\)'
if (-not [regex]::IsMatch($generatedText, $reassignPattern)) {
    throw "Generated MIR2CPP code does not destroy the old DCI object before reconstructing the same storage"
}

[void](Invoke-CheckedProcess -Name "cmake_configure" -FilePath $cmakeExe `
    -ArgumentList @("--preset", "ninja-release",
                    "-DCMAKE_CXX_COMPILER:FILEPATH=$ClangCxx",
                    "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninjaExe") `
    -WorkingDirectory $cppOut)
[void](Invoke-CheckedProcess -Name "cmake_build" -FilePath $cmakeExe `
    -ArgumentList @("--build", "--preset", "ninja-release", "--parallel", "$Jobs") `
    -WorkingDirectory $cppOut)

$generatedExe = Join-Path $cppOut "build\ninja-release\vyx_mir2cpp_dci_direct_opaque.exe"
if (-not (Test-Path -LiteralPath $generatedExe -PathType Leaf)) {
    throw "Generated executable not found: $generatedExe"
}
[void](Invoke-CheckedProcess -Name "run_generated" -FilePath $generatedExe `
    -WorkingDirectory $cppOut -ExpectedText $expected)

Write-Host "mir2cpp_dci_direct_opaque: OK"
Write-Host "artifacts: $runRoot"

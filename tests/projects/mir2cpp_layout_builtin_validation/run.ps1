param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = ""
)

$ErrorActionPreference = "Stop"

# MIR2CPP / `--emit=cpp` is an experimental backend that is not a release or
# fixed-point acceptance gate (docs/TESTING_GUIDE.md, "Backend policy"), and
# this case gates that backend's emitted C++ only. It is kept for manual runs
# of MIR2CPP and self-skips in the project sweep; set VYX_ACCEPT_MIR2CPP=1 to
# force it. Exit 77 is the sweep's "skipped" sentinel.
if ($env:VYX_ACCEPT_MIR2CPP -ne "1") {
    Write-Host "mir2cpp_layout_builtin_validation: skipped (experimental MIR2CPP backend; set VYX_ACCEPT_MIR2CPP=1 to run)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot (".cache\validation_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
$isWindowsPlatform = $env:OS -eq "Windows_NT"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = Split-Path -Parent $BootstrapCompiler
}
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$env:Path = $RuntimeDir + [System.IO.Path]::PathSeparator + $env:Path
$runtimeArgs = @("-L", $RuntimeDir, "-l", "vyx_runtime")

New-Item -ItemType Directory -Force -Path $cache | Out-Null

function Invoke-ExpectedFailure {
    param(
        [string]$Name,
        [string]$Source,
        [string]$ExpectedDiagnostic,
        [string]$Emit = "cpp"
    )

    $outDir = Join-Path $cache $Name
    if ($Emit -ne "cpp") { $outDir = Join-Path $cache ($Name + "." + $Emit) }
    $stdout = Join-Path $cache ($Name + ".out.log")
    $stderr = Join-Path $cache ($Name + ".err.log")
    & $BootstrapCompiler --src=file $Source ("--emit=" + $Emit) -o $outDir > $stdout 2> $stderr
    $exitCode = $LASTEXITCODE
    $text = ((Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue) +
             (Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue)) -join "`n"
    if ($exitCode -ne 1) {
        Write-Host $text
        throw "$Name expected exit 1, got $exitCode"
    }
    if ($text.IndexOf($ExpectedDiagnostic, [StringComparison]::Ordinal) -lt 0) {
        Write-Host $text
        throw "$Name missing diagnostic: $ExpectedDiagnostic"
    }
}

$mainSource = Join-Path $projectRoot "src\main.vyx"
$nativeExe = Join-Path $cache $(if ($isWindowsPlatform) { "native_layout.exe" } else { "native_layout" })
& $BootstrapCompiler --src=file $mainSource --run=aot -o $nativeExe @runtimeArgs
if ($LASTEXITCODE -ne 0) { throw "LLVM layout ABI validation failed with exit $LASTEXITCODE" }

$validOut = Join-Path $cache "valid"
& $BootstrapCompiler --src=file $mainSource --emit=cpp -o $validOut @runtimeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not (Test-Path -LiteralPath (Join-Path $validOut "CMakeLists.txt") -PathType Leaf)) {
    throw "valid MIR2CPP emission did not produce CMakeLists.txt"
}
$generatedSource = (Get-ChildItem -LiteralPath (Join-Path $validOut "src") -Filter "*.cpp" -File |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
foreach ($expectedExpr in @(
    "(static_cast<int64_t>(8) != 8)",
    "(static_cast<int64_t>(16) != 16)",
    "(static_cast<int64_t>(32) != 32)",
    "(static_cast<int64_t>(24) != 24)"
)) {
    if ($generatedSource.IndexOf($expectedExpr, [StringComparison]::Ordinal) -lt 0) {
        throw "generated MIR2CPP source is missing Vyx ABI constant: $expectedExpr"
    }
}
if ($generatedSource.IndexOf("sizeof(vyx_type_ReprOuter_", [StringComparison]::Ordinal) -lt 0) {
    throw "generated MIR2CPP source is missing backend representation sizeof for ReprOuter"
}

$clangName = if ($isWindowsPlatform) { "clang++.exe" } else { "clang++" }
$bundledClang = Join-Path $repoRoot ("clang\bin\" + $clangName)
$clangCxx = if (Test-Path -LiteralPath $bundledClang -PathType Leaf) {
    (Resolve-Path -LiteralPath $bundledClang).Path
} else {
    (Get-Command $clangName -ErrorAction Stop).Source
}
$cmakeExe = (Get-Command cmake -ErrorAction Stop).Source
$ninjaExe = (Get-Command ninja -ErrorAction Stop).Source
Push-Location $validOut
try {
    & $cmakeExe --preset ninja-release `
        "-DCMAKE_CXX_COMPILER:FILEPATH=$clangCxx" `
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninjaExe"
    if ($LASTEXITCODE -ne 0) { throw "MIR2CPP CMake configure failed with exit $LASTEXITCODE" }
    & $cmakeExe --build --preset ninja-release --parallel 2
    if ($LASTEXITCODE -ne 0) { throw "MIR2CPP CMake build failed with exit $LASTEXITCODE" }
} finally {
    Pop-Location
}
$generatedName = if ($isWindowsPlatform) { "vyx_main.exe" } else { "vyx_main" }
$generatedExe = Join-Path $validOut ("build\ninja-release\" + $generatedName)
if (-not (Test-Path -LiteralPath $generatedExe -PathType Leaf)) {
    throw "MIR2CPP executable was not produced: $generatedExe"
}
& $generatedExe
if ($LASTEXITCODE -ne 0) { throw "MIR2CPP layout ABI validation failed with exit $LASTEXITCODE" }

Invoke-ExpectedFailure `
    -Name "missing_type_arg" `
    -Source (Join-Path $projectRoot "src\missing_type_arg.vyx") `
    -ExpectedDiagnostic "MIR C++ lowering failed: sizeof requires one concrete type argument"

Invoke-ExpectedFailure `
    -Name "ambiguous_type" `
    -Source (Join-Path $projectRoot "src\ambiguous_type.vyx") `
    -ExpectedDiagnostic "MIR C++ lowering failed: ambiguous alignof type ``Base``"

Invoke-ExpectedFailure `
    -Name "void_size" `
    -Source (Join-Path $projectRoot "src\void_size.vyx") `
    -ExpectedDiagnostic "MIR C++ lowering failed: cannot lower sizeof type ``void``"

Invoke-ExpectedFailure `
    -Name "void_align" `
    -Source (Join-Path $projectRoot "src\void_align.vyx") `
    -ExpectedDiagnostic "MIR C++ lowering failed: cannot lower alignof type ``void``"

Invoke-ExpectedFailure `
    -Name "empty_class_size_cpp" `
    -Source (Join-Path $projectRoot "src\empty_class_size.vyx") `
    -ExpectedDiagnostic "MIR C++ lowering failed: cannot lower sizeof type ``EmptyClass``"

Invoke-ExpectedFailure `
    -Name "empty_class_size_llvm" `
    -Source (Join-Path $projectRoot "src\empty_class_size.vyx") `
    -Emit "ir" `
    -ExpectedDiagnostic "cannot lower sizeof type EmptyClass"

Write-Host "mir2cpp_layout_builtin_validation: OK"
exit 0

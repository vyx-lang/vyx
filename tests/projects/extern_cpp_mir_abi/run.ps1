param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [string]$Clang = "",
    [ValidateSet("-O0", "-O1", "-O2", "-O3", "-Os", "-Oz")]
    [string]$OptimizationLevel = "-O0",
    [ValidateRange(1, 600)]
    [int]$TimeoutSec = 600,
    [ValidateRange(256, 8192)]
    [int]$MemoryLimitMB = 8192,
    [switch]$ExperimentalBackends
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "extern_cpp_mir_abi: SKIP (fixture validates the x86_64 MSVC ABI)"
    exit 77
}

$fixtureRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $fixtureRoot "..\..\..")).Path
. (Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1")
$windowsHost = ($env:OS -eq "Windows_NT")
$projectRoot = Join-Path $repoRoot ("tests\.cache\extern_cpp_mir_abi_run_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
New-Item -ItemType Directory -Path $projectRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $fixtureRoot "Vyx.toml") -Destination $projectRoot
foreach ($dir in @("contracts", "native", "src")) {
    Copy-Item -LiteralPath (Join-Path $fixtureRoot $dir) -Destination $projectRoot -Recurse
}

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($Clang)) {
    $Clang = Join-Path $repoRoot "clang\bin\clang++.exe"
}
$Clang = (Resolve-Path $Clang).Path

$ar = Join-Path (Split-Path -Parent $Clang) "llvm-ar.exe"
$ar = (Resolve-Path $ar).Path

if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    foreach ($cand in @(
        (Split-Path -Parent $BootstrapCompiler),
        (Join-Path $repoRoot "bootstrap_compiler\out"),
        (Join-Path $repoRoot "bootstrap_compiler\out\vyx_rt"),
        (Join-Path $repoRoot "build_yolo_vyxcg\vyx_codegen"),
        (Join-Path $repoRoot "build_yolo_vyxcg\vyx_rt")
    )) {
        if ((Test-Path (Join-Path $cand "vyx_compiler_backend.lib")) -or
            (Test-Path (Join-Path $cand "vyx_compiler_backend.dll")) -or
            (Test-Path (Join-Path $cand "libvyx_compiler_backend.so")) -or
            (Test-Path (Join-Path $cand "libvyx_compiler_backend.dylib")) -or
            (Test-Path (Join-Path $cand "vyx_rt.lib")) -or
            (Test-Path (Join-Path $cand "vyx_codegen.lib")) -or
            (Test-Path (Join-Path $cand "vyx_rt.dll"))) {
            $RuntimeDir = $cand
            break
        }
    }
}
if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    throw "No runtime directory found next to the selected bootstrap compiler"
}
$RuntimeDir = (Resolve-Path $RuntimeDir).Path
$env:Path = $RuntimeDir + ";" + $env:Path
$env:VYX_DCI_PYTHON = "__dci_python_must_not_run__"

$cache = Join-Path $projectRoot ".cache"
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$runtimeArtifact = Get-Item -LiteralPath (Join-Path $RuntimeDir "vyx_compiler_backend.dll")
if (-not $runtimeArtifact.PSIsContainer -and $runtimeArtifact.Length -eq 0) {
    throw "Selected directory has an empty compiler backend: $RuntimeDir"
}

function Get-Sha256Hex([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

$toolchainLog = Join-Path $cache "toolchain.txt"
$toolchainLines = @(
    "compiler=$BootstrapCompiler",
    "compiler_sha256=$(Get-Sha256Hex $BootstrapCompiler)",
    "optimization=$OptimizationLevel",
    "runtime=$($runtimeArtifact.FullName)",
    "runtime_sha256=$(Get-Sha256Hex $runtimeArtifact.FullName)"
)
[IO.File]::WriteAllLines($toolchainLog, $toolchainLines, [Text.UTF8Encoding]::new($false))
Write-Host "[toolchain] $($toolchainLines -join ' ')"

$src = Join-Path $projectRoot "src\main.vyx"
$virtualSrc = Join-Path $projectRoot "src\dci_min_virtual_dispatch.vyx"
$temporaryStubSrc = Join-Path $projectRoot "src\dci_stub_temporary_negative.vyx"
$native = Join-Path $projectRoot "native\A.cpp"
$nativeObj = Join-Path $cache "A.obj"
$staticLib = Join-Path $cache "extern_cpp_A.lib"
$dll = Join-Path $cache "extern_cpp_A_dyn.dll"
$ll = Join-Path $cache "extern_cpp.ll"
$itaniumLl = Join-Path $cache "extern_cpp_itanium.ll"
$obj = Join-Path $cache "extern_cpp.obj"
$exe = Join-Path $cache "extern_cpp.exe"
$cppOut = Join-Path $cache "mir2cpp"
$virtualCppOut = Join-Path $cache "mir2cpp_virtual_dispatch"
$jitStubObj = Join-Path $cache "extern_cpp_dci_stubs_jit.obj"
if ($ExperimentalBackends) {
    $cmakeExe = (Get-Command cmake -ErrorAction Stop).Source
    $ninjaExe = (Get-Command ninja -ErrorAction Stop).Source
}

function Invoke-Checked {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList,
        [string]$Name,
        [switch]$ExpectFailure,
        [string]$ExpectedDiagnostic = ""
    )
    $out = Join-Path $cache ($Name + ".out.log")
    $err = Join-Path $cache ($Name + ".err.log")
    $invokeArgs = @{
        FilePath = $FilePath
        ArgumentList = $ArgumentList
        WorkingDirectory = (Get-Location).Path
        StdoutLog = $out
        StderrLog = $err
        DialogLog = (Join-Path $cache ($Name + ".dialog.log"))
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $invokeArgs.MemoryLimitMB = $MemoryLimitMB }
    $results = @(Invoke-VyxProcess @invokeArgs)
    if ($results.Count -eq 0) { throw "$Name returned no process result" }
    $result = $results[-1]
    $stdout = if (Test-Path $out) { Get-Content -Raw -LiteralPath $out } else { "" }
    $stderr = if (Test-Path $err) { Get-Content -Raw -LiteralPath $err } else { "" }
    $combined = $stdout + "`n" + $stderr
    $resourceFailure = $result.TimedOut
    if ($windowsHost) {
        $resourceFailure = $resourceFailure -or $result.MemoryExceeded -or $result.DialogCaught
    }
    if ($resourceFailure) {
        throw "FAILED: $Name exit=$($result.ExitCode) timed_out=$($result.TimedOut)`n$combined"
    }
    if ($ExpectFailure) {
        if ($result.ExitCode -eq 0) {
            throw "FAILED: $Name unexpectedly succeeded`n$combined"
        }
    } elseif ($result.ExitCode -ne 0) {
        throw "FAILED: $Name exit=$($result.ExitCode)`n$combined"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedDiagnostic) -and
        $combined.IndexOf($ExpectedDiagnostic, [StringComparison]::Ordinal) -lt 0) {
        throw "FAILED: $Name is missing diagnostic: $ExpectedDiagnostic`n$combined"
    }
}

function Assert-RuntimeLinkClassification {
    param(
        [string]$Name,
        [string]$LinkName,
        [int]$ExpectedLibraryCount
    )
    $probeExe = Join-Path $cache ($Name + ".exe")
    Invoke-Checked -FilePath $BootstrapCompiler `
        -ArgumentList @("--src=file", $runtimeLinkProbe, "--emit=exe", "-O0",
                        "-o", $probeExe, "-l", $LinkName) `
        -Name $Name -ExpectFailure
    $probeText = (Get-Content -Raw -LiteralPath (Join-Path $cache ($Name + ".out.log"))) +
                 "`n" +
                 (Get-Content -Raw -LiteralPath (Join-Path $cache ($Name + ".err.log")))
    $linkLine = ($probeText -split "`r?`n" |
        Where-Object { $_.StartsWith("[vyxc] link: ", [StringComparison]::Ordinal) } |
        Select-Object -Last 1)
    if ([string]::IsNullOrWhiteSpace($linkLine)) {
        throw "FAILED: $Name did not report its linker command"
    }
    if ($linkLine.IndexOf(" -l " + $LinkName, [StringComparison]::Ordinal) -lt 0) {
        throw "FAILED: $Name linker command omitted explicit library $LinkName`n$linkLine"
    }
    $libraryCount = [regex]::Matches($linkLine, '(?:^|\s)-l\s+\S+').Count
    if ($libraryCount -ne $ExpectedLibraryCount) {
        throw "FAILED: $Name expected $ExpectedLibraryCount linked libraries, got $libraryCount`n$linkLine"
    }
}

$runtimeLinkProbe = Join-Path $cache "runtime_link_probe.vyx"
[IO.File]::WriteAllText(
    $runtimeLinkProbe,
    "fn main() -> i32 { return 0; }`n",
    [Text.UTF8Encoding]::new($false))
$missingDescriptorProbe = Join-Path $cache "missing_dci_descriptor.vyx"
[IO.File]::WriteAllText(
    $missingDescriptorProbe,
    "extern `"dci`" { fn missing_descriptor_probe(value: i32) -> i32; }`n" +
    "fn main() -> i32 { return missing_descriptor_probe(1); }`n",
    [Text.UTF8Encoding]::new($false))
Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $missingDescriptorProbe, "--emit=cpp", "-O0",
                    "-o", (Join-Path $cache "missing_descriptor_cpp")) `
    -Name "missing_dci_descriptor" -ExpectFailure `
    -ExpectedDiagnostic "requires a DCI descriptor"
$originalBootstrapProfile = $env:VYX_BOOTSTRAP_PROFILE
try {
    $env:VYX_BOOTSTRAP_PROFILE = "1"
    Assert-RuntimeLinkClassification -Name "runtime_link_canonical_static" `
        -LinkName "libvyx_runtime.a" -ExpectedLibraryCount 1
    Assert-RuntimeLinkClassification -Name "runtime_link_canonical_backend" `
        -LinkName "libvyx_compiler_backend.dll.a" -ExpectedLibraryCount 1
    Assert-RuntimeLinkClassification -Name "runtime_link_mingw_vyx_rt" `
        -LinkName "libvyx_rt.dll.a" -ExpectedLibraryCount 1
    Assert-RuntimeLinkClassification -Name "runtime_link_mingw_vyx_run_rt" `
        -LinkName "libvyx_run_rt.dll.a" -ExpectedLibraryCount 1
    Assert-RuntimeLinkClassification -Name "runtime_link_versioned_so" `
        -LinkName "libvyx_rt.so.7" -ExpectedLibraryCount 1
    Assert-RuntimeLinkClassification -Name "runtime_link_invalid_so_suffix" `
        -LinkName "libvyx_run_rt.soevil" -ExpectedLibraryCount 2
} finally {
    $env:VYX_BOOTSTRAP_PROFILE = $originalBootstrapProfile
}

Invoke-Checked -FilePath $Clang -ArgumentList @("-c", $native, "-I", (Join-Path $projectRoot "native"), "-O2", "-fms-runtime-lib=static", "-o", $nativeObj) -Name "native_obj"
Invoke-Checked -FilePath $ar -ArgumentList @("rcs", $staticLib, $nativeObj) -Name "native_ar"
Invoke-Checked -FilePath $Clang -ArgumentList @($native, "-I", (Join-Path $projectRoot "native"), "-shared", "-O2", "-fms-runtime-lib=static", "-o", $dll) -Name "native_dll"

Push-Location $projectRoot
try {
    Invoke-Checked -FilePath $BootstrapCompiler `
        -ArgumentList @("build", "-j", "10", $OptimizationLevel) `
        -Name "project_build"
} finally {
    Pop-Location
}

$dciFile = Get-Item -LiteralPath (Join-Path $projectRoot "contracts\A.dcib")
if ($null -eq $dciFile) {
    Write-Host "FAILED: explicit DCI contract is missing"
    exit 1
}
$contractLog = "contract=$($dciFile.FullName)`n" +
    "contract_sha256=$(Get-Sha256Hex $dciFile.FullName)`n"
[IO.File]::AppendAllText($toolchainLog, $contractLog, [Text.UTF8Encoding]::new($false))
$dciArgs = @("--dci", $dciFile.FullName)
Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList (@("--src=file", $temporaryStubSrc, "--emit=cpp") + $dciArgs +
                   @("-o", (Join-Path $cache "temporary_stub_cpp"))) `
    -Name "temporary_stub" -ExpectFailure `
    -ExpectedDiagnostic "DCI stub constructor must initialize exactly one final owning place"
$corruptDci = Join-Path $cache "extern_cpp_corrupt.dcib"
$corruptBytes = [IO.File]::ReadAllBytes($dciFile.FullName)
if ($corruptBytes.Length -le 20) {
    Write-Host "FAILED: generated DCIB payload is unexpectedly empty"
    exit 1
}
$corruptBytes[$corruptBytes.Length - 1] = $corruptBytes[$corruptBytes.Length - 1] -bxor 1
[IO.File]::WriteAllBytes($corruptDci, $corruptBytes)
Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $src, "--emit=ir", "--dci", $corruptDci,
                    "-o", (Join-Path $cache "corrupt_dcib.ll")) `
    -Name "corrupt_dcib" -ExpectFailure `
    -ExpectedDiagnostic "DCIB payload CRC mismatch"
# The stub object file name carries the DCI stub backend id in the middle
# (`<pkg>_src_<file>_vyx_dci_stubs_<backend>_c.obj`), so resolve it by pattern
# instead of pinning a backend name that the manifest is free to change.
$stubObj = Get-ChildItem -LiteralPath $cache -Filter "*_dci_stubs_*_c.obj" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($null -eq $stubObj) {
    Write-Host "FAILED: DCI stub object was not generated"
    exit 1
}
$stubObjPath = $stubObj.FullName

Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList (@("--src=file", $src, "--dump-mir2", $OptimizationLevel) + $dciArgs) -Name "dump_mir2"
$mirDumpText = Get-Content -Raw -LiteralPath (Join-Path $cache "dump_mir2.out.log")
$dceHelperMatch = [regex]::Match(
    $mirDumpText,
    '(?m)^\s+fn #(\d+) .* name=dce_dci_call_and_drop_must_survive ')
if (-not $dceHelperMatch.Success) {
    Write-Host "FAILED: DCE DCI helper is missing from the MIR dump"
    exit 1
}
$dceHelperId = [int]$dceHelperMatch.Groups[1].Value
$dceEvalMatches = [regex]::Matches(
    $mirDumpText,
    '(?m)^\s+instr #\d+ fn=#' + $dceHelperId + '\s+block=#\d+\s+kind=1\s+.*?\svalue=#([1-9]\d*)\s')
if ($dceEvalMatches.Count -lt 1) {
    Write-Host "FAILED: DCE cleared the external C++ DCI eval call"
    exit 1
}
foreach ($dceEval in $dceEvalMatches) {
    $dceValueId = [int]$dceEval.Groups[1].Value
    $callPattern = '(?m)^\s+value #' + $dceValueId + '\s+.*\s+kind=10\s'
    if (-not [regex]::IsMatch($mirDumpText, $callPattern)) {
        Write-Host "FAILED: DCE DCI eval root is no longer a call value"
        exit 1
    }
}
$dceDropPattern = '(?m)^\s+instr #\d+ fn=#' + $dceHelperId + '\s+block=#\d+\s+kind=8\s+'
if ([regex]::Matches($mirDumpText, $dceDropPattern).Count -lt 1) {
    Write-Host "FAILED: DCE removed the external C++ DCI drop"
    exit 1
}
$selectHelperMatch = [regex]::Match(
    $mirDumpText,
    '(?m)^\s+fn #(\d+) .* name=select_dci_call_must_stay_branch ')
if (-not $selectHelperMatch.Success) {
    Write-Host "FAILED: select DCI helper is missing from the MIR dump"
    exit 1
}
$selectHelperId = [int]$selectHelperMatch.Groups[1].Value
$selectControlPattern = '(?m)^\s+value #\d+ fn=#' + $selectHelperId + '\s+kind=14\s+'
if ([regex]::Matches($mirDumpText, $selectControlPattern).Count -ne 0) {
    Write-Host "FAILED: select formation speculatively converted C++ DCI call arms"
    exit 1
}
$selectBranchPattern = '(?m)^\s+block #\d+ fn=#' + $selectHelperId + '\s+removed=0\s+.*\s+term=3\s+'
if ([regex]::Matches($mirDumpText, $selectBranchPattern).Count -lt 1) {
    Write-Host "FAILED: select DCI helper no longer retains an active branch"
    exit 1
}
Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList (@("--src=file", $src, "--emit=ir", $OptimizationLevel) + $dciArgs + @("-o", $ll)) -Name "emit_ir"
Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList (@("--src=file", $src, "--emit=obj", $OptimizationLevel) + $dciArgs + @("-o", $obj)) -Name "emit_obj"
Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList (@("--src=file", $src, "--emit=exe", $OptimizationLevel) + $dciArgs + @("-o", $exe, "--link-obj", $nativeObj, "--link-obj", $stubObj.FullName)) -Name "emit_exe"

$runOut = Join-Path $cache "run_exe.out.log"
$runErr = Join-Path $cache "run_exe.err.log"
Invoke-Checked -FilePath $exe -ArgumentList @() -Name "run_exe"

$expected = "extern_cpp_mir_abi OK"
$actualRun = ((Get-Content $runOut) -join "`n").Trim()
if ($actualRun -ne $expected) {
    Write-Host "FAILED: exe output mismatch"
    Write-Host "expected: $expected"
    Write-Host "actual: $actualRun"
    exit 1
}

if ($ExperimentalBackends) {
    $stubCpp = $stubObj.FullName
    if ($stubCpp.EndsWith("_c.obj", [System.StringComparison]::OrdinalIgnoreCase)) {
        $stubCpp = $stubCpp.Substring(0, $stubCpp.Length - "_c.obj".Length) + ".cpp"
    } else {
        $stubCpp = [System.IO.Path]::ChangeExtension($stubCpp, ".cpp")
    }
    if (-not (Test-Path -LiteralPath $stubCpp)) {
        Write-Host "FAILED: DCI stub source was not generated"
        exit 1
    }
    Invoke-Checked -FilePath $Clang -ArgumentList @("-c", $stubCpp, "-I", (Join-Path $projectRoot "native"), "-O2", "-std=c++17", "-fno-rtti", "-fno-exceptions", "-o", $jitStubObj) -Name "jit_stub_obj"
    Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList (@("--src=file", $src, "--run=jit", $OptimizationLevel) + $dciArgs + @("--link", $dll, "--link-obj", $jitStubObj)) -Name "jit_run"
    Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList (@("--src=file", $src, "--emit=cpp", $OptimizationLevel) + $dciArgs + @("-o", $cppOut)) -Name "emit_cpp"
    Invoke-Checked -FilePath $BootstrapCompiler -ArgumentList (@("--src=file", $virtualSrc, "--emit=cpp", $OptimizationLevel) + $dciArgs + @("-o", $virtualCppOut)) -Name "emit_cpp_virtual_dispatch"
    $cppPreset = if ($OptimizationLevel -ceq "-O0") { "ninja-debug" } else { "ninja-release" }

    $fullStubSource = Join-Path $cppOut "src\vyx_dci_stubs.cpp"
    $fullCmakeFile = Join-Path $cppOut "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $fullStubSource -PathType Leaf)) {
        Write-Host "FAILED: full MIR2CPP project is missing the generated DCI stub TU"
        exit 1
    }
    $fullGeneratedText = (Get-Content -Raw -LiteralPath $fullStubSource) + "`n" +
                         (Get-Content -Raw -LiteralPath (Join-Path $cppOut "src\tu_0001.cpp"))
    $reverseCallbacks = @(
        "__vyx_M_VyxPoly_N_add_R_i32_P_VyxPoly_i32",
        "__vyx_M_VyxPoly_N_add_R_i32_P_VyxPoly_f64",
        "__vyx_M_VyxPoly_N_mul_R_i32_P_VyxPoly_i32",
        "__vyx_M_VyxOp_N_eval_R_i32_P_VyxOp_i32"
    )
    foreach ($needle in @(
        "__dci_stub_factory_VyxPoly",
        "__dci_stub_destroy_VyxPoly",
        "__dci_stub_factory_VyxOp",
        "__dci_stub_destroy_VyxOp",
        $reverseCallbacks[0],
        $reverseCallbacks[1],
        $reverseCallbacks[2],
        $reverseCallbacks[3]
    )) {
        if ($fullGeneratedText.IndexOf($needle, [StringComparison]::Ordinal) -lt 0) {
            Write-Host "FAILED: full MIR2CPP project is missing reverse DCI symbol $needle"
            exit 1
        }
    }
    foreach ($callback in $reverseCallbacks) {
        $pattern = [regex]::Escape($callback) + '\([^;{}]*\) noexcept'
        if ([regex]::Matches($fullGeneratedText, $pattern).Count -lt 2) {
            Write-Host "FAILED: reverse DCI callback declaration/definition is not no-unwind: $callback"
            exit 1
        }
    }
    $noexceptOverrideCount = [regex]::Matches($fullGeneratedText, '\) noexcept override(?: final)? \{').Count
    if ($noexceptOverrideCount -ne 4) {
        Write-Host "FAILED: expected four no-unwind reverse DCI overrides, found $noexceptOverrideCount"
        exit 1
    }
    $fullCmakeText = Get-Content -Raw -LiteralPath $fullCmakeFile
    if ($fullCmakeText.IndexOf('"src/vyx_dci_stubs.cpp"', [StringComparison]::Ordinal) -lt 0) {
        Write-Host "FAILED: full MIR2CPP CMake project does not compile the generated DCI stub TU"
        exit 1
    }

    Push-Location $cppOut
    try {
        Invoke-Checked -FilePath $cmakeExe -ArgumentList @(
            "--preset", $cppPreset,
            "-DCMAKE_CXX_COMPILER:FILEPATH=$Clang",
            "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninjaExe",
            "-DCMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded",
            "-DCMAKE_CXX_FLAGS:STRING=-I$($projectRoot.Replace('\', '/'))/native",
            "-DCMAKE_EXE_LINKER_FLAGS:STRING=$staticLib"
        ) -Name "full_cmake_configure"
        Invoke-Checked -FilePath $cmakeExe -ArgumentList @(
            "--build", "--preset", $cppPreset, "--parallel", "10"
        ) -Name "full_cmake_build"
    } finally {
        Pop-Location
    }

    $fullCppExe = Join-Path $cppOut ("build\" + $cppPreset + "\vyx_main.exe")
    if (-not (Test-Path -LiteralPath $fullCppExe -PathType Leaf)) {
        Write-Host "FAILED: full generated MIR2CPP executable is missing"
        exit 1
    }
    Invoke-Checked -FilePath $fullCppExe -ArgumentList @() -Name "full_run_generated"

    $virtualGeneratedFiles = @(Get-ChildItem -LiteralPath (Join-Path $virtualCppOut "include") -File -Filter "*.hpp") +
                             @(Get-ChildItem -LiteralPath (Join-Path $virtualCppOut "src") -File -Filter "*.cpp")
    $virtualGeneratedText = ($virtualGeneratedFiles | ForEach-Object {
        Get-Content -Raw -LiteralPath $_.FullName
    }) -join "`n"
    if ($virtualGeneratedText.IndexOf('#include "A.hpp"', [StringComparison]::Ordinal) -ge 0) {
        Write-Host "FAILED: virtual DCI MIR2CPP probe depends on the producer C++ header"
        exit 1
    }
    foreach ($slot in @(8, 16, 24)) {
        $slotNeedle = "reinterpret_cast<unsigned char*>(vyx_dci_vtable) + $slot"
        if ($virtualGeneratedText.IndexOf($slotNeedle, [StringComparison]::Ordinal) -lt 0) {
            Write-Host "FAILED: virtual DCI MIR2CPP probe is missing vtable slot offset $slot"
            exit 1
        }
    }

    Push-Location $virtualCppOut
    try {
        Invoke-Checked -FilePath $cmakeExe -ArgumentList @(
            "--preset", $cppPreset,
            "-DCMAKE_CXX_COMPILER:FILEPATH=$Clang",
            "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninjaExe",
            "-DCMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded",
            "-DCMAKE_EXE_LINKER_FLAGS=$staticLib"
        ) -Name "virtual_cmake_configure"
        Invoke-Checked -FilePath $cmakeExe -ArgumentList @(
            "--build", "--preset", $cppPreset, "--parallel", "10"
        ) -Name "virtual_cmake_build"
    } finally {
        Pop-Location
    }

    $virtualExe = Join-Path $virtualCppOut ("build\" + $cppPreset + "\vyx_dci_min_virtual_dispatch.exe")
    if (-not (Test-Path -LiteralPath $virtualExe -PathType Leaf)) {
        Write-Host "FAILED: generated virtual DCI executable is missing"
        exit 1
    }
    Invoke-Checked -FilePath $virtualExe -ArgumentList @() -Name "virtual_run_generated"

    $actualJit = ((Get-Content (Join-Path $cache "jit_run.out.log")) -join "`n").Trim()
    $actualFullCpp = ((Get-Content (Join-Path $cache "full_run_generated.out.log")) -join "`n").Trim()
    if ($actualJit -ne $expected) {
        Write-Host "FAILED: jit output mismatch"
        Write-Host "expected:"
        Write-Host $expected
        Write-Host "actual:"
        Write-Host $actualJit
        exit 1
    }
    if ($actualFullCpp -ne $expected) {
        Write-Host "FAILED: full generated MIR2CPP output mismatch"
        Write-Host "expected:"
        Write-Host $expected
        Write-Host "actual:"
        Write-Host $actualFullCpp
        exit 1
    }
    $actualVirtual = ((Get-Content (Join-Path $cache "virtual_run_generated.out.log")) -join "`n").Trim()
    if ($actualVirtual -ne "dci_virtual_dispatch OK") {
        Write-Host "FAILED: generated virtual DCI output mismatch"
        Write-Host $actualVirtual
        exit 1
    }
} else {
    Write-Host "JIT/MIR2CPP checks deferred (opt in with -ExperimentalBackends)"
}

$irText = Get-Content -Raw -LiteralPath $ll
foreach ($needle in @("??0A@@QEAA@XZ", "??0A@@QEAA@H@Z", "?func@A@@QEAAXXZ", "?add@A@@QEAAHH@Z", "?add@A@@QEAAHN@Z", "?bump@A@@SAHH@Z", "?bump@A@@SAHN@Z", "?free_add@@YAHHH@Z", "?free_add@@YAHNN@Z", "?add@PolyDerived@@UEAAHH@Z")) {
    if ($irText.IndexOf($needle, [StringComparison]::Ordinal) -lt 0) {
        Write-Host "FAILED: missing C++ ABI symbol in LLVM IR: $needle"
        exit 1
    }
}
if (-not [regex]::IsMatch($irText, 'declare win64cc ptr @"\?\?0A@@QEAA@XZ"\(ptr')) {
    Write-Host "FAILED: x86_64 MSVC constructor does not return the owner pointer in LLVM IR"
    exit 1
}

Write-Host "extern_cpp_mir_abi: OK"
Write-Host "artifacts: $projectRoot"

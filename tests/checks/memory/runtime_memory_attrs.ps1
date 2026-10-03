[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [string]$LlvmRoot = "",
    [switch]$IncludeExperimentalMir2Cpp,
    [int]$TimeoutSec = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) {
    throw "TimeoutSec must be between 1 and 600."
}

$testsRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testsRoot "..")).Path
. (Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1")

$windowsHost = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    [bool]$IsWindows
} else {
    $env:OS -eq "Windows_NT"
}
$compilerName = if ($windowsHost) { "boot.exe" } else { "boot" }
$exeSuffix = if ($windowsHost) { ".exe" } else { "" }
$runtimeName = if ($windowsHost) {
    "vyx_compiler_backend.dll"
} elseif ($IsMacOS) {
    "libvyx_compiler_backend.dylib"
} else {
    "libvyx_compiler_backend.so"
}

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = Split-Path -Parent $BootstrapCompiler
}
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$runtimePath = Join-Path $RuntimeDir $runtimeName
if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
    throw "compiler backend paired with selected compiler is missing: $runtimePath"
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
if ([string]::IsNullOrWhiteSpace($LlvmRoot) -and $windowsHost) {
    $LlvmRoot = Join-Path $repoRoot "clang"
}

$runRoot = Join-Path $repoRoot ("tests/.cache/runtime_memory_attrs_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][System.IO.Directory]::CreateDirectory($runRoot)
$source = Join-Path $testsRoot "cases/codegen_runtime_memory_attrs.vyx"
$levels = @("-O0", "-O1", "-O2", "-O3", "-Os", "-Oz")
$evidence = New-Object System.Collections.Generic.List[object]
$originalLlvmRoot = $env:LLVM_ROOT
$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH

try {
    if (-not [string]::IsNullOrWhiteSpace($LlvmRoot)) {
        $env:LLVM_ROOT = (Resolve-Path -LiteralPath $LlvmRoot).Path
    }
    $pathSeparator = if ($windowsHost) { ";" } else { ":" }
    $env:Path = $RuntimeDir + $pathSeparator + $originalPath
    if (-not $windowsHost) {
        $env:LD_LIBRARY_PATH = $RuntimeDir + $pathSeparator + $originalLdLibraryPath
        if (Get-Variable -Name IsMacOS -ErrorAction SilentlyContinue) {
            if ([bool]$IsMacOS) {
                $env:DYLD_LIBRARY_PATH = $RuntimeDir + $pathSeparator + $originalDyldLibraryPath
            }
        }
    }

    foreach ($level in $levels) {
        $stem = $level.Substring(1).ToLowerInvariant()
        $exe = Join-Path $runRoot ("runtime_memory_attrs_" + $stem + $exeSuffix)
        $compileOut = Join-Path $runRoot ($stem + ".compile.stdout.log")
        $compileErr = Join-Path $runRoot ($stem + ".compile.stderr.log")
        $compileArgs = @{
            FilePath = $BootstrapCompiler
            ArgumentList = @("--src=file", $source, "--emit=exe", $level, "-o", $exe,
                             "-L", $RuntimeDir, "-l", "vyx_compiler_backend")
            WorkingDirectory = $repoRoot
            StdoutLog = $compileOut
            StderrLog = $compileErr
            DialogLog = (Join-Path $runRoot ($stem + ".compile.dialog.log"))
            TimeoutSec = $TimeoutSec
        }
        if ($windowsHost) { $compileArgs.MemoryLimitMB = 4096 }
        $compile = Invoke-VyxProcess @compileArgs
        if ($compile.ExitCode -ne 0 -or $compile.TimedOut -or $compile.DialogCaught -or $compile.MemoryExceeded) {
            throw "$level compile failed: exit=$($compile.ExitCode) timeout=$($compile.TimedOut) memory=$($compile.MemoryExceeded)"
        }
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            throw "$level compiler did not produce executable: $exe"
        }

        $runOut = Join-Path $runRoot ($stem + ".run.stdout.log")
        $runErr = Join-Path $runRoot ($stem + ".run.stderr.log")
        $runArgs = @{
            FilePath = $exe
            WorkingDirectory = $runRoot
            StdoutLog = $runOut
            StderrLog = $runErr
            DialogLog = (Join-Path $runRoot ($stem + ".run.dialog.log"))
            TimeoutSec = 30
        }
        if ($windowsHost) { $runArgs.MemoryLimitMB = 512 }
        $run = Invoke-VyxProcess @runArgs
        $actual = if (Test-Path -LiteralPath $runOut) {
            [IO.File]::ReadAllText($runOut).Trim()
        } else {
            ""
        }
        if ($run.ExitCode -ne 0 -or $run.TimedOut -or $actual -cne "codegen_runtime_memory_attrs OK") {
            throw "$level execution failed: exit=$($run.ExitCode) output=$actual"
        }

        $jitOut = Join-Path $runRoot ($stem + ".jit.stdout.log")
        $jitErr = Join-Path $runRoot ($stem + ".jit.stderr.log")
        $jitArgs = @{
            FilePath = $BootstrapCompiler
            ArgumentList = @("--src=file", $source, "--run=jit", $level,
                             "-L", $RuntimeDir, "-l", "vyx_compiler_backend")
            WorkingDirectory = $repoRoot
            StdoutLog = $jitOut
            StderrLog = $jitErr
            DialogLog = (Join-Path $runRoot ($stem + ".jit.dialog.log"))
            TimeoutSec = $TimeoutSec
        }
        if ($windowsHost) { $jitArgs.MemoryLimitMB = 4096 }
        $jit = Invoke-VyxProcess @jitArgs
        $jitActual = if (Test-Path -LiteralPath $jitOut) {
            [IO.File]::ReadAllText($jitOut).Trim()
        } else {
            ""
        }
        if ($jit.ExitCode -ne 0 -or $jit.TimedOut -or
            $jitActual -cne "codegen_runtime_memory_attrs OK") {
            throw "$level JIT failed: exit=$($jit.ExitCode) output=$jitActual"
        }

        $evidence.Add([ordered]@{
            optimization = $level
            compile_exit_code = [int]$compile.ExitCode
            compile_peak_working_set_mb = $compile.PeakWorkingSetMB
            run_exit_code = [int]$run.ExitCode
            output = $actual
            jit_exit_code = [int]$jit.ExitCode
            jit_peak_working_set_mb = $jit.PeakWorkingSetMB
            jit_output = $jitActual
        })
        Write-Host "runtime_memory_attrs ${level}: OK"
    }

    $ownershipSource = Join-Path $testsRoot "cases/codegen_string_temp_ownership.vyx"
    foreach ($level in @("-O0", "-O2")) {
        $stem = "ownership_" + $level.Substring(1).ToLowerInvariant()
        $exe = Join-Path $runRoot ($stem + $exeSuffix)
        $stdout = Join-Path $runRoot ($stem + ".stdout.log")
        $stderr = Join-Path $runRoot ($stem + ".stderr.log")
        $ownershipArgs = @{
            FilePath = $BootstrapCompiler
            ArgumentList = @("--src=file", $ownershipSource, "--run=aot", $level,
                             "-o", $exe)
            WorkingDirectory = $repoRoot
            StdoutLog = $stdout
            StderrLog = $stderr
            DialogLog = (Join-Path $runRoot ($stem + ".dialog.log"))
            TimeoutSec = $TimeoutSec
        }
        if ($windowsHost) { $ownershipArgs.MemoryLimitMB = 4096 }
        $ownership = Invoke-VyxProcess @ownershipArgs
        $actual = if (Test-Path -LiteralPath $stdout) {
            [IO.File]::ReadAllText($stdout).Trim()
        } else {
            ""
        }
        if ($ownership.ExitCode -ne 0 -or $ownership.TimedOut -or
            $actual.IndexOf("codegen_string_temp_ownership OK", [StringComparison]::Ordinal) -lt 0) {
            throw "$level string ownership execution failed: exit=$($ownership.ExitCode) output=$actual"
        }
        $jitStdout = Join-Path $runRoot ($stem + ".jit.stdout.log")
        $jitArgs = @{
            FilePath = $BootstrapCompiler
            ArgumentList = @("--src=file", $ownershipSource, "--run=jit", $level,
                             "-L", $RuntimeDir, "-l", "vyx_compiler_backend")
            WorkingDirectory = $repoRoot
            StdoutLog = $jitStdout
            StderrLog = Join-Path $runRoot ($stem + ".jit.stderr.log")
            DialogLog = Join-Path $runRoot ($stem + ".jit.dialog.log")
            TimeoutSec = $TimeoutSec
        }
        if ($windowsHost) { $jitArgs.MemoryLimitMB = 4096 }
        $jit = Invoke-VyxProcess @jitArgs
        $jitActual = if (Test-Path -LiteralPath $jitStdout) {
            [IO.File]::ReadAllText($jitStdout).Trim()
        } else {
            ""
        }
        if ($jit.ExitCode -ne 0 -or $jit.TimedOut -or
            $jitActual.IndexOf("codegen_string_temp_ownership OK", [StringComparison]::Ordinal) -lt 0) {
            throw "$level string ownership JIT failed: exit=$($jit.ExitCode) output=$jitActual"
        }
        $evidence.Add([ordered]@{
            optimization = "string-ownership-" + $level
            compile_exit_code = [int]$ownership.ExitCode
            compile_peak_working_set_mb = $ownership.PeakWorkingSetMB
            run_exit_code = [int]$ownership.ExitCode
            output = $actual
            jit_exit_code = [int]$jit.ExitCode
            jit_peak_working_set_mb = $jit.PeakWorkingSetMB
            jit_output = $jitActual
        })
        Write-Host "string_temp_ownership ${level} AOT/JIT: OK"
    }

    $cloneSource = Join-Path $testsRoot "cases/codegen_string_clone_abi.vyx"
    foreach ($level in @("-O0", "-O2")) {
        $stem = "string_clone_" + $level.Substring(1).ToLowerInvariant()
        $cloneAotStdout = Join-Path $runRoot ($stem + ".aot.stdout.log")
        $cloneAotArgs = @{
            FilePath = $BootstrapCompiler
            ArgumentList = @("--src=file", $cloneSource, "--run=aot", $level,
                             "-L", $RuntimeDir, "-l", "vyx_compiler_backend")
            WorkingDirectory = $repoRoot
            StdoutLog = $cloneAotStdout
            StderrLog = Join-Path $runRoot ($stem + ".aot.stderr.log")
            DialogLog = Join-Path $runRoot ($stem + ".aot.dialog.log")
            TimeoutSec = $TimeoutSec
        }
        if ($windowsHost) { $cloneAotArgs.MemoryLimitMB = 4096 }
        $cloneAot = Invoke-VyxProcess @cloneAotArgs
        $cloneAotOutput = if (Test-Path -LiteralPath $cloneAotStdout) {
            [IO.File]::ReadAllText($cloneAotStdout).Trim()
        } else {
            ""
        }
        if ($cloneAot.ExitCode -ne 0 -or $cloneAot.TimedOut -or
            $cloneAotOutput -cne "codegen_string_clone_abi OK") {
            throw "$level string clone AOT failed: exit=$($cloneAot.ExitCode) output=$cloneAotOutput"
        }

        $cloneJitStdout = Join-Path $runRoot ($stem + ".jit.stdout.log")
        $cloneJitArgs = @{
            FilePath = $BootstrapCompiler
            ArgumentList = @("--src=file", $cloneSource, "--run=jit", $level,
                             "-L", $RuntimeDir, "-l", "vyx_compiler_backend")
            WorkingDirectory = $repoRoot
            StdoutLog = $cloneJitStdout
            StderrLog = Join-Path $runRoot ($stem + ".jit.stderr.log")
            DialogLog = Join-Path $runRoot ($stem + ".jit.dialog.log")
            TimeoutSec = $TimeoutSec
        }
        if ($windowsHost) { $cloneJitArgs.MemoryLimitMB = 4096 }
        $cloneJit = Invoke-VyxProcess @cloneJitArgs
        $cloneJitOutput = if (Test-Path -LiteralPath $cloneJitStdout) {
            [IO.File]::ReadAllText($cloneJitStdout).Trim()
        } else {
            ""
        }
        if ($cloneJit.ExitCode -ne 0 -or $cloneJit.TimedOut -or
            $cloneJitOutput -cne "codegen_string_clone_abi OK") {
            throw "$level string clone JIT failed: exit=$($cloneJit.ExitCode) output=$cloneJitOutput"
        }
        $evidence.Add([ordered]@{
            optimization = "string-clone-" + $level
            aot_exit_code = [int]$cloneAot.ExitCode
            aot_output = $cloneAotOutput
            jit_exit_code = [int]$cloneJit.ExitCode
            jit_output = $cloneJitOutput
        })
        Write-Host "string_clone ${level} AOT/JIT: OK"
    }

    $stdStringSource = Join-Path $testsRoot "cases/std_string_sso_regression.vyx"
    foreach ($level in @("-O0", "-O2")) {
        $stem = "std_string_" + $level.Substring(1).ToLowerInvariant()
        $exe = Join-Path $runRoot ($stem + $exeSuffix)
        $stdout = Join-Path $runRoot ($stem + ".stdout.log")
        $stdStringArgs = @{
            FilePath = $BootstrapCompiler
            ArgumentList = @("--src=file", $stdStringSource, "--run=aot", $level,
                             "-o", $exe)
            WorkingDirectory = $repoRoot
            StdoutLog = $stdout
            StderrLog = Join-Path $runRoot ($stem + ".stderr.log")
            DialogLog = Join-Path $runRoot ($stem + ".dialog.log")
            TimeoutSec = $TimeoutSec
        }
        if ($windowsHost) { $stdStringArgs.MemoryLimitMB = 4096 }
        $stdString = Invoke-VyxProcess @stdStringArgs
        $actual = if (Test-Path -LiteralPath $stdout) {
            [IO.File]::ReadAllText($stdout).Trim()
        } else {
            ""
        }
        if ($stdString.ExitCode -ne 0 -or $stdString.TimedOut -or
            $actual -cne "std_string_sso_regression OK") {
            throw "$level std String SSO regression failed: exit=$($stdString.ExitCode) output=$actual"
        }
        $evidence.Add([ordered]@{
            optimization = "std-string-sso-" + $level
            compile_exit_code = [int]$stdString.ExitCode
            compile_peak_working_set_mb = $stdString.PeakWorkingSetMB
            run_exit_code = [int]$stdString.ExitCode
            output = $actual
        })
        Write-Host "std_string_sso ${level} AOT: OK"
    }

    $ownershipIr = Join-Path $runRoot "string_ownership.o0.ll"
    $irOut = Join-Path $runRoot "string_ownership.ir.stdout.log"
    $irErr = Join-Path $runRoot "string_ownership.ir.stderr.log"
    $irArgs = @{
        FilePath = $BootstrapCompiler
        ArgumentList = @("--src=file", $ownershipSource, "--emit=ir", "-O0",
                         "-o", $ownershipIr)
        WorkingDirectory = $repoRoot
        StdoutLog = $irOut
        StderrLog = $irErr
        DialogLog = (Join-Path $runRoot "string_ownership.ir.dialog.log")
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $irArgs.MemoryLimitMB = 4096 }
    $irResult = Invoke-VyxProcess @irArgs
    if ($irResult.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $ownershipIr -PathType Leaf)) {
        throw "string ownership IR emission failed: exit=$($irResult.ExitCode)"
    }
    $irText = [IO.File]::ReadAllText($ownershipIr)
    $appendMemmoveCalls = [regex]::Matches(
        $irText,
        '(?m)\bcall\b[^\r\n]*@memmove\('
    ).Count
    $localFreeCalls = [regex]::Matches($irText, '(?m)^\s*call\s+void\s+@free\(').Count
    $runtimeFreeCalls = [regex]::Matches(
        $irText,
        '(?m)^\s*call\s+void\s+@vyx_free_runtime_string\('
    ).Count
    $strDropCalls = [regex]::Matches(
        $irText,
        '(?m)^\s*call\s+void\s+@__vyx_F_str_5Fdrop_R_void_P_str\(').Count
    if ($appendMemmoveCalls -lt 1) {
        throw "self-append must use overlap-safe in-module memmove; calls=$appendMemmoveCalls"
    }
    $cloneAbiCalls = [regex]::Matches(
        $irText,
        '(?m)\bcall\b[^\r\n]*@vyx_string_clone_len_abi\('
    ).Count
    if ($cloneAbiCalls -lt 1) {
        throw "primitive str clone must use the versioned runtime-owned clone ABI; calls=$cloneAbiCalls"
    }
    if ($localFreeCalls -lt 3 -or $runtimeFreeCalls -lt 3 -or $strDropCalls -lt 12) {
        throw "string temporaries, including loop-scoped primitive str locals, must release buffers in their allocator domain; local=$localFreeCalls runtime=$runtimeFreeCalls str_drop=$strDropCalls"
    }
    Write-Host "string_temp_ownership IR clone_abi=${cloneAbiCalls} frees local=${localFreeCalls} runtime=${runtimeFreeCalls} str_drop=${strDropCalls}: OK"

    if ($IncludeExperimentalMir2Cpp) {
    $cppOwnershipSource = Join-Path $testsRoot "cases/mir2cpp_string_ownership.vyx"
    $cppOwnershipOut = Join-Path $runRoot "mir2cpp_string_ownership_cpp"
    $cppEmitArgs = @{
        FilePath = $BootstrapCompiler
        ArgumentList = @("--src=file", $cppOwnershipSource, "--emit=cpp", "-O0",
                         "-o", $cppOwnershipOut, "-L$RuntimeDir", "-lvyx_compiler_backend", "--",
                         "-L", "must_not_leak_after_double_dash",
                         "-l", "must_not_leak_after_double_dash")
        WorkingDirectory = $repoRoot
        StdoutLog = Join-Path $runRoot "mir2cpp_string_ownership.emit.stdout.log"
        StderrLog = Join-Path $runRoot "mir2cpp_string_ownership.emit.stderr.log"
        DialogLog = Join-Path $runRoot "mir2cpp_string_ownership.emit.dialog.log"
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $cppEmitArgs.MemoryLimitMB = 4096 }
    $cppEmit = Invoke-VyxProcess @cppEmitArgs
    if ($cppEmit.ExitCode -ne 0 -or $cppEmit.TimedOut -or $cppEmit.DialogCaught -or
        $cppEmit.MemoryExceeded -or -not (Test-Path -LiteralPath $cppOwnershipOut -PathType Container)) {
        throw "MIR2CPP ownership emission failed: exit=$($cppEmit.ExitCode) timeout=$($cppEmit.TimedOut) memory=$($cppEmit.MemoryExceeded)"
    }
    $cppCmakeText = Get-Content -Raw -LiteralPath (Join-Path $cppOwnershipOut "CMakeLists.txt")
    if ($cppCmakeText.IndexOf("vyx_compiler_backend", [StringComparison]::Ordinal) -lt 0 -or
        $cppCmakeText.IndexOf("VYX_LINKED_RUNTIME_STRING_FREE=1", [StringComparison]::Ordinal) -lt 0 -or
        $cppCmakeText.IndexOf('$ORIGIN', [StringComparison]::Ordinal) -lt 0 -or
        $cppCmakeText.IndexOf('@loader_path', [StringComparison]::Ordinal) -lt 0 -or
        $cppCmakeText.IndexOf("must_not_leak_after_double_dash", [StringComparison]::Ordinal) -ge 0) {
        throw "MIR2CPP runtime linkage, RPATH, compact link options, or -- argument boundary were not preserved"
    }
    $cppRuntimeHeader = Get-Content -Raw -LiteralPath (Join-Path $cppOwnershipOut "include/vyx_runtime.hpp")
    if ($cppRuntimeHeader.IndexOf('::vyx_free_runtime_string', [StringComparison]::Ordinal) -lt 0 -or
        $cppRuntimeHeader.IndexOf('vyx_generated_resolve_runtime_symbol', [StringComparison]::Ordinal) -ge 0 -or
        $cppRuntimeHeader.IndexOf('dlsym', [StringComparison]::Ordinal) -ge 0) {
        throw "MIR2CPP runtime strings must use the explicitly linked allocator module"
    }

    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    $cppPreset = "ninja-debug"
    $cppConfigureArgs = @{
        FilePath = $cmake
        ArgumentList = @("--preset", $cppPreset)
        WorkingDirectory = $cppOwnershipOut
        StdoutLog = Join-Path $runRoot "mir2cpp_string_ownership.configure.stdout.log"
        StderrLog = Join-Path $runRoot "mir2cpp_string_ownership.configure.stderr.log"
        DialogLog = Join-Path $runRoot "mir2cpp_string_ownership.configure.dialog.log"
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $cppConfigureArgs.MemoryLimitMB = 4096 }
    $cppConfigure = Invoke-VyxProcess @cppConfigureArgs
    if ($cppConfigure.ExitCode -ne 0 -or $cppConfigure.TimedOut -or
        $cppConfigure.DialogCaught -or $cppConfigure.MemoryExceeded) {
        throw "MIR2CPP ownership configure failed: exit=$($cppConfigure.ExitCode) timeout=$($cppConfigure.TimedOut) memory=$($cppConfigure.MemoryExceeded)"
    }

    $cppBuildArgs = @{
        FilePath = $cmake
        ArgumentList = @("--build", "--preset", $cppPreset, "--parallel", "3")
        WorkingDirectory = $cppOwnershipOut
        StdoutLog = Join-Path $runRoot "mir2cpp_string_ownership.build.stdout.log"
        StderrLog = Join-Path $runRoot "mir2cpp_string_ownership.build.stderr.log"
        DialogLog = Join-Path $runRoot "mir2cpp_string_ownership.build.dialog.log"
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $cppBuildArgs.MemoryLimitMB = 8192 }
    $cppBuild = Invoke-VyxProcess @cppBuildArgs
    if ($cppBuild.ExitCode -ne 0 -or $cppBuild.TimedOut -or
        $cppBuild.DialogCaught -or $cppBuild.MemoryExceeded) {
        throw "MIR2CPP ownership build failed: exit=$($cppBuild.ExitCode) timeout=$($cppBuild.TimedOut) memory=$($cppBuild.MemoryExceeded)"
    }

    $cppOwnershipExe = Join-Path $cppOwnershipOut (
        "build/" + $cppPreset + "/vyx_mir2cpp_string_ownership" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppOwnershipExe -PathType Leaf)) {
        throw "MIR2CPP ownership build did not produce executable: $cppOwnershipExe"
    }
    $cppRunOut = Join-Path $runRoot "mir2cpp_string_ownership.run.stdout.log"
    $cppRunArgs = @{
        FilePath = $cppOwnershipExe
        WorkingDirectory = Split-Path -Parent $cppOwnershipExe
        StdoutLog = $cppRunOut
        StderrLog = Join-Path $runRoot "mir2cpp_string_ownership.run.stderr.log"
        DialogLog = Join-Path $runRoot "mir2cpp_string_ownership.run.dialog.log"
        TimeoutSec = 30
    }
    if ($windowsHost) { $cppRunArgs.MemoryLimitMB = 512 }
    $cppRun = Invoke-VyxProcess @cppRunArgs
    $cppRunActual = if (Test-Path -LiteralPath $cppRunOut) {
        [IO.File]::ReadAllText($cppRunOut).Trim()
    } else {
        ""
    }
    if ($cppRun.ExitCode -ne 0 -or $cppRun.TimedOut -or
        $cppRunActual -cne "mir2cpp_string_ownership OK") {
        throw "MIR2CPP ownership execution failed: exit=$($cppRun.ExitCode) output=$cppRunActual"
    }
    $evidence.Add([ordered]@{
        optimization = "mir2cpp-string-ownership-O0"
        emit_exit_code = [int]$cppEmit.ExitCode
        configure_exit_code = [int]$cppConfigure.ExitCode
        build_exit_code = [int]$cppBuild.ExitCode
        run_exit_code = [int]$cppRun.ExitCode
        output = $cppRunActual
    })
    Write-Host "mir2cpp_string_ownership emit/configure/build/run: OK"
    } else {
        Write-Host "mir2cpp_string_ownership: SKIP (experimental backend; pass -IncludeExperimentalMir2Cpp to run)"
    }

    [ordered]@{
        compiler_path = $BootstrapCompiler
        compiler_sha256 = Get-Sha256Hex $BootstrapCompiler
        runtime_path = $runtimePath
        runtime_sha256 = Get-Sha256Hex $runtimePath
        cases = $evidence
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot "evidence.json") -Encoding utf8
    Write-Host "runtime_memory_attrs: OK"
    Write-Host "runtime_memory_attrs evidence=$runRoot"
} finally {
    $env:LLVM_ROOT = $originalLlvmRoot
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [int]$TimeoutSec = 600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) {
    throw "TimeoutSec must be between 1 and 600."
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "..\..\..")).Path
. (Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1")
$windowsHost = ($env:OS -eq "Windows_NT")

function Resolve-LlvmTool {
    param([Parameter(Mandatory = $true)][string]$Name)

    $fileName = if ($windowsHost) { $Name + ".exe" } else { $Name }
    $bundled = Join-Path $repoRoot ("clang\bin\" + $fileName)
    if (Test-Path -LiteralPath $bundled -PathType Leaf) {
        return (Resolve-Path -LiteralPath $bundled).Path
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LLVM_ROOT)) {
        $fromEnvironment = Join-Path $env:LLVM_ROOT ("bin\" + $fileName)
        if (Test-Path -LiteralPath $fromEnvironment -PathType Leaf) {
            return (Resolve-Path -LiteralPath $fromEnvironment).Path
        }
    }
    return (Get-Command $fileName -ErrorAction Stop).Source
}

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path

$runRoot = Join-Path $projectRoot (".cache\run_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
$logsRoot = Join-Path $runRoot "logs"
$irRoot = Join-Path $runRoot "ir"
$objRoot = Join-Path $runRoot "obj"
$cppRoot = Join-Path $runRoot "cpp"
foreach ($dir in @($logsRoot, $irRoot, $objRoot, $cppRoot)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

function Invoke-Compiler {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int]$ExpectedExit = 0,
        [string]$WorkingDirectory = $projectRoot
    )

    $stdoutLog = Join-Path $logsRoot ($Name + ".stdout.log")
    $stderrLog = Join-Path $logsRoot ($Name + ".stderr.log")
    $dialogLog = Join-Path $logsRoot ($Name + ".dialog.log")
    $invokeArgs = @{
        FilePath = $BootstrapCompiler
        ArgumentList = $Arguments
        WorkingDirectory = $WorkingDirectory
        StdoutLog = $stdoutLog
        StderrLog = $stderrLog
        DialogLog = $dialogLog
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $invokeArgs.MemoryLimitMB = 8192 }
    $results = @(Invoke-VyxProcess @invokeArgs)
    if ($results.Count -eq 0) { throw "$Name returned no process result" }
    $result = $results[-1]
    $stdout = if (Test-Path -LiteralPath $stdoutLog) { Get-Content -Raw -LiteralPath $stdoutLog } else { "" }
    $stderr = if (Test-Path -LiteralPath $stderrLog) { Get-Content -Raw -LiteralPath $stderrLog } else { "" }
    $combined = $stdout + "`n" + $stderr
    if ($result.TimedOut) { throw "$Name timed out after $TimeoutSec seconds" }
    if ($result.ExitCode -ne $ExpectedExit) {
        throw "$Name expected exit $ExpectedExit, got $($result.ExitCode).`n$combined"
    }
    return $combined
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Context
    )
    if ($Text.IndexOf($Expected, [StringComparison]::Ordinal) -lt 0) {
        throw "$Context is missing: $Expected"
    }
}

$simpleSource = Join-Path $projectRoot "src\simple.vyx"
$platformSource = Join-Path $projectRoot "src\platform.vyx"

# These are exactly the target backends compiled into the bundled LLVM SDK.
$irTargets = @(
    @{ Name = "x86_64_windows_msvc"; Requested = "x86_64-windows-msvc"; Canonical = "x86_64-unknown-windows-msvc" },
    @{ Name = "aarch64_linux_gnu"; Requested = "aarch64-linux-gnu"; Canonical = "aarch64-unknown-linux-gnu" },
    @{ Name = "armv7_linux_gnueabihf"; Requested = "armv7-linux-gnueabihf"; Canonical = "armv7-unknown-linux-gnueabihf" },
    @{ Name = "bpfel_unknown_none"; Requested = "bpfel-unknown-none"; Canonical = "bpfel-unknown-none" },
    @{ Name = "wasm32_unknown_unknown"; Requested = "wasm32-unknown-unknown"; Canonical = "wasm32-unknown-unknown" },
    @{ Name = "riscv64_linux_gnu"; Requested = "riscv64-linux-gnu"; Canonical = "riscv64-unknown-linux-gnu" },
    @{ Name = "nvptx64_nvidia_cuda"; Requested = "nvptx64-nvidia-cuda"; Canonical = "nvptx64-nvidia-cuda" }
)

foreach ($target in $irTargets) {
    $out = Join-Path $irRoot ($target.Name + ".ll")
    $log = Invoke-Compiler -Name ("ir_" + $target.Name) -Arguments @(
        "--src=file", $simpleSource,
        "--emit=ir",
        "-O2",
        ("--target=" + $target.Requested),
        "-o", $out
    )
    if (-not (Test-Path -LiteralPath $out -PathType Leaf) -or (Get-Item -LiteralPath $out).Length -eq 0) {
        throw "$($target.Requested) did not produce LLVM IR"
    }
    $ir = Get-Content -Raw -LiteralPath $out
    Assert-Contains -Text $ir -Expected ('target triple = "' + $target.Canonical + '"') -Context $target.Requested
    if ($ir -notmatch 'target datalayout = "[^\"]+"') {
        throw "$($target.Requested) emitted an empty LLVM data layout"
    }
    if ($log.IndexOf("not a recognized processor", [StringComparison]::Ordinal) -ge 0) {
        throw "$($target.Requested) selected an invalid default processor"
    }
    Write-Host "[ir] $($target.Requested) -> $($target.Canonical)"
}

$fsSource = Join-Path $repoRoot "tests\cases\smoke\common\common_fs_paths.vyx"
$linuxFsIr = Join-Path $irRoot "fs_linux.ll"
[void](Invoke-Compiler -Name "fs_linux" -Arguments @(
    "--src=file", $fsSource,
    "--emit=ir",
    "-O2",
    "--target=x86_64-linux-gnu",
    "-o", $linuxFsIr
))
$linuxFsText = Get-Content -Raw -LiteralPath $linuxFsIr
if ($linuxFsText -match 'call\s+i32\s+@GetFileAttributesA') {
    throw "Linux filesystem lowering called the Windows GetFileAttributesA API"
}

$windowsFsIr = Join-Path $irRoot "fs_windows.ll"
[void](Invoke-Compiler -Name "fs_windows" -Arguments @(
    "--src=file", $fsSource,
    "--emit=ir",
    "-O2",
    "--target=x86_64-windows-msvc",
    "-o", $windowsFsIr
))
$windowsFsText = Get-Content -Raw -LiteralPath $windowsFsIr
if ($windowsFsText -notmatch 'call\s+i32\s+@GetFileAttributesA') {
    throw "Windows filesystem lowering did not call GetFileAttributesA"
}
Write-Host "[ir-fs] target-specific filesystem ABI"

# NVPTX emits PTX assembly rather than a conventional object, while the BPF
# backend rejects Vyx's hosted String runtime helpers. The remaining bundled
# backends must produce native object files from the same source.
$objectTargets = @(
    @{ Name = "x86_64_windows_msvc"; Requested = "x86_64-windows-msvc"; Ext = ".obj" },
    @{ Name = "aarch64_linux_gnu"; Requested = "aarch64-linux-gnu"; Ext = ".o" },
    @{ Name = "armv7_linux_gnueabihf"; Requested = "armv7-linux-gnueabihf"; Ext = ".o" },
    @{ Name = "wasm32_unknown_unknown"; Requested = "wasm32-unknown-unknown"; Ext = ".o" },
    @{ Name = "riscv64_linux_gnu"; Requested = "riscv64-linux-gnu"; Ext = ".o" }
)
foreach ($target in $objectTargets) {
    $out = Join-Path $objRoot ($target.Name + $target.Ext)
    [void](Invoke-Compiler -Name ("obj_" + $target.Name) -Arguments @(
        "--src=file", $simpleSource,
        "--emit=obj",
        "-O2",
        ("--target=" + $target.Requested),
        "-o", $out
    ))
    if (-not (Test-Path -LiteralPath $out -PathType Leaf) -or (Get-Item -LiteralPath $out).Length -eq 0) {
        throw "$($target.Requested) did not produce an object file"
    }
    Write-Host "[obj] $($target.Requested)"
}

$linuxSysroot = Join-Path $runRoot "sdk\linux sysroot"
$androidSysroot = Join-Path $runRoot "sdk\android sysroot"
$platformTargets = @(
    @{ Name = "aarch64_windows_msvc"; Requested = "aarch64-windows-msvc"; Canonical = "aarch64-unknown-windows-msvc"; System = "Windows"; Processor = "aarch64"; Value = 1; Abi = ""; Sysroot = "" },
    @{ Name = "aarch64_linux_gnu"; Requested = "aarch64-linux-gnu"; Canonical = "aarch64-unknown-linux-gnu"; System = "Linux"; Processor = "aarch64"; Value = 2; Abi = ""; Sysroot = $linuxSysroot },
    @{ Name = "aarch64_linux_android23"; Requested = "aarch64-linux-android23"; Canonical = "aarch64-unknown-linux-android23"; System = "Android"; Processor = "aarch64"; Value = 3; Abi = "arm64-v8a"; Sysroot = $androidSysroot },
    @{ Name = "aarch64_apple_darwin"; Requested = "aarch64-apple-darwin"; Canonical = "aarch64-apple-darwin"; System = "Darwin"; Processor = "aarch64"; Value = 4; Abi = ""; Sysroot = "" }
)

foreach ($target in $platformTargets) {
    $outDir = Join-Path $cppRoot $target.Name
    $args = @(
        "--src=file", $platformSource,
        "--emit=cpp",
        "-O2",
        ("--target=" + $target.Requested),
        "-o", $outDir
    )
    if (-not [string]::IsNullOrWhiteSpace($target.Sysroot)) {
        $args += ("--sysroot=" + $target.Sysroot)
    }
    [void](Invoke-Compiler -Name ("cpp_" + $target.Name) -Arguments $args)

    $toolchainPath = Join-Path $outDir "cmake\vyx-target.cmake"
    $toolchain = Get-Content -Raw -LiteralPath $toolchainPath
    Assert-Contains -Text $toolchain -Expected ('set(VYX_TARGET_TRIPLE "' + $target.Canonical + '"') -Context $target.Requested
    Assert-Contains -Text $toolchain -Expected ('set(CMAKE_SYSTEM_NAME "' + $target.System + '")') -Context $target.Requested
    Assert-Contains -Text $toolchain -Expected ('set(CMAKE_SYSTEM_PROCESSOR "' + $target.Processor + '")') -Context $target.Requested
    if (-not [string]::IsNullOrWhiteSpace($target.Abi)) {
        Assert-Contains -Text $toolchain -Expected ('set(CMAKE_ANDROID_ARCH_ABI "' + $target.Abi + '")') -Context $target.Requested
    }
    if (-not [string]::IsNullOrWhiteSpace($target.Sysroot)) {
        $normalizedToolchain = $toolchain.Replace('\', '/')
        $normalizedSysroot = ([IO.Path]::GetFullPath($target.Sysroot)).Replace('\', '/')
        Assert-Contains -Text $normalizedToolchain -Expected ('set(CMAKE_SYSROOT "' + $normalizedSysroot + '"') -Context $target.Requested
    }

    $generatedCpp = (Get-ChildItem -LiteralPath (Join-Path $outDir "src") -Filter "*.cpp" -File |
        ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
    $match = [regex]::Match($generatedCpp, 'int32_t vyx_fn_platform_value_\d+\(\)\s*\{(?<body>[\s\S]*?)\n\}')
    if (-not $match.Success -or $match.Groups['body'].Value -notmatch ('=\s*' + $target.Value + '\s*;')) {
        throw "$($target.Requested) selected the wrong @[platform] branch"
    }
    $posixMatch = [regex]::Match($generatedCpp, 'int32_t vyx_fn_posix_value_\d+\(\)\s*\{(?<body>[\s\S]*?)\n\}')
    $expectedPosixValue = if ($target.System -eq "Windows") { 0 } else { 10 }
    if (-not $posixMatch.Success -or
        $posixMatch.Groups['body'].Value -notmatch ('=\s*' + $expectedPosixValue + '\s*;')) {
        throw "$($target.Requested) selected the wrong @[platform(``posix``)] branch"
    }
    $presets = Get-Content -Raw -LiteralPath (Join-Path $outDir "CMakePresets.json")
    Assert-Contains -Text $presets -Expected 'CMAKE_TOOLCHAIN_FILE' -Context $target.Requested
    $readme = Get-Content -Raw -LiteralPath (Join-Path $outDir "README.md")
    Assert-Contains -Text $readme -Expected $target.Canonical -Context $target.Requested
    Write-Host "[cpp] $($target.Requested) -> $($target.System)/$($target.Processor)"
}

# Project selection is independent from LLVM target selection: --target names
# the manifest target, while --triplet selects the backend triple.
$projectOut = Join-Path $cppRoot "project_android"
[void](Invoke-Compiler -Name "cpp_project_android" -Arguments @(
    "--src=project", $projectRoot,
    "--emit=cpp",
    "-O2",
    "--target=aarch64-linux-android23",
    ("--sysroot=" + $androidSysroot),
    "-o", $projectOut
))
$projectToolchain = Get-Content -Raw -LiteralPath (Join-Path $projectOut "cmake\vyx-target.cmake")
Assert-Contains -Text $projectToolchain -Expected 'set(VYX_TARGET_TRIPLE "aarch64-unknown-linux-android23"' -Context "project target"
Assert-Contains -Text $projectToolchain -Expected 'set(CMAKE_SYSTEM_NAME "Android")' -Context "project target"
$projectCmake = Get-Content -Raw -LiteralPath (Join-Path $projectOut "CMakeLists.txt")
$repoPathNeedle = $repoRoot.Replace('\', '/').TrimEnd('/') + '/'
if ($projectCmake.IndexOf($repoPathNeedle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
    throw "cross-target MIR2CPP project contains a host-absolute repository path"
}
Assert-Contains -Text $projectCmake -Expected 'native/src/0_native.c' -Context "project native source bundle"
Assert-Contains -Text $projectCmake -Expected 'native/include/0' -Context "project include bundle"
if (-not (Test-Path -LiteralPath (Join-Path $projectOut "native\src\0_native.c")) -or
    -not (Test-Path -LiteralPath (Join-Path $projectOut "native\include\0\target_matrix.h"))) {
    throw "cross-target MIR2CPP project did not bundle native source/include files"
}
Write-Host "[cpp-project] aarch64-linux-android23"

$hostOut = Join-Path $cppRoot "host_default"
[void](Invoke-Compiler -Name "cpp_host_default" -Arguments @(
    "--src=file", $simpleSource,
    "--emit=cpp",
    "-O2",
    "-o", $hostOut
))
if (Test-Path -LiteralPath (Join-Path $hostOut "cmake\vyx-target.cmake")) {
    throw "host MIR2CPP export unexpectedly emitted a target toolchain"
}
$hostPresets = Get-Content -Raw -LiteralPath (Join-Path $hostOut "CMakePresets.json")
if ($hostPresets.IndexOf("CMAKE_TOOLCHAIN_FILE", [StringComparison]::Ordinal) -ge 0) {
    throw "host MIR2CPP preset unexpectedly references a target toolchain"
}

# A static target exercises the manifest build path without requiring the
# target platform's C runtime or linker SDK on the host. Build directly in the
# project directory so the artifact lands where the manifest build puts it.
$projectTarget = Join-Path $projectRoot "target"
if (Test-Path -LiteralPath $projectTarget) {
    Remove-Item -LiteralPath $projectTarget -Recurse -Force
}
[void](Invoke-Compiler -Name "build_aarch64_linux" -WorkingDirectory $projectRoot -Arguments @(
    "build",
    "-j", "2",
    "-O2",
    "--triplet=aarch64-linux-gnu"
))
$crossLibrary = Join-Path $projectTarget "libllvm_target_matrix.a"
if (-not (Test-Path -LiteralPath $crossLibrary -PathType Leaf) -or
    (Get-Item -LiteralPath $crossLibrary).Length -eq 0) {
    throw "project build did not produce the AArch64 Linux static library"
}
$llvmReadObj = Resolve-LlvmTool "llvm-readobj"
$crossHeader = (& $llvmReadObj --file-headers $crossLibrary 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "llvm-readobj could not inspect the cross-target library" }
Assert-Contains -Text $crossHeader -Expected "Format: elf64-littleaarch64" -Context "project build artifact"
Assert-Contains -Text $crossHeader -Expected "Machine: EM_AARCH64" -Context "project build artifact"
$llvmAr = Resolve-LlvmTool "llvm-ar"
$archiveMembers = (& $llvmAr t $crossLibrary 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "llvm-ar could not inspect the cross-target library" }
Assert-Contains -Text $archiveMembers -Expected "llvm_target_matrix_crate_llvm_target_matrix.o" -Context "project build archive"
Assert-Contains -Text $archiveMembers -Expected "llvm_target_matrix_src_native_c_c.o" -Context "project build archive"
Write-Host "[build] aarch64-linux-gnu"

$invalidLog = Invoke-Compiler -Name "invalid_target" -ExpectedExit 1 -Arguments @(
    "--src=file", $simpleSource,
    "--emit=ir",
    "--target=not-a-real-backend",
    "-o", (Join-Path $irRoot "invalid.ll")
)
Assert-Contains -Text $invalidLog -Expected "unsupported LLVM target triple" -Context "invalid target diagnostic"

$invalidCppLog = Invoke-Compiler -Name "invalid_target_cpp" -ExpectedExit 1 -Arguments @(
    "--src=file", $simpleSource,
    "--emit=cpp",
    "--target=not-a-real-backend",
    "-o", (Join-Path $cppRoot "invalid")
)
Assert-Contains -Text $invalidCppLog -Expected "unsupported LLVM target triple" -Context "invalid MIR2CPP target diagnostic"

$invalidBuildLog = Invoke-Compiler -Name "invalid_target_build" -ExpectedExit 1 -Arguments @(
    "build",
    "--triplet=not-a-real-backend"
)
Assert-Contains -Text $invalidBuildLog -Expected "unsupported LLVM target triple" -Context "invalid build target diagnostic"

Write-Host "llvm_target_matrix: OK"
exit 0
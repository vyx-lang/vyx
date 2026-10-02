param(
    [string]$BootstrapRoot = "",
    [string]$Destination = "",
    [switch]$BundleLlvm,
    [switch]$BundleLldb,
    [string]$LldbBundleRoot = "",
    [switch]$Archive,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$scriptRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $scriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapRoot)) {
    $BootstrapRoot = Join-Path $repoRoot "bootstrap_compiler"
}
$bootstrap = (Resolve-Path -LiteralPath $BootstrapRoot).Path
$out = Join-Path $bootstrap "out"
$runningOnWindows = $env:OS -eq "Windows_NT"
if ($BundleLlvm -and [string]::IsNullOrWhiteSpace($env:LLVM_ROOT)) {
    throw "-BundleLlvm requires LLVM_ROOT"
}
# Windows packages keep the real bin/vyxc.exe as their entry point: the
# bootstrapped compiler locates its toolchain from its own directory, so no
# wrapper is needed.  Hiding the payload behind a renamed binary is a POSIX-only
# convention -- it exists purely so a shell launcher can export LLVM_ROOT/PATH.
$useLauncherLayout = $BundleLlvm -and -not $runningOnWindows

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $platform = if ($runningOnWindows) { "windows-x86_64" } else { "linux-x86_64" }
    $Destination = Join-Path (Join-Path $repoRoot "dist") ("vyx-sdk-{0}-llvm22" -f $platform)
}
$destinationPath = [System.IO.Path]::GetFullPath($Destination)
$destinationParent = Split-Path -Parent $destinationPath
$archivePath = if ($runningOnWindows) { "$destinationPath.zip" } else { "$destinationPath.tar.gz" }
if (-not (Test-Path -LiteralPath $destinationParent -PathType Container)) {
    throw "SDK destination parent does not exist: $destinationParent"
}
if (Test-Path -LiteralPath $destinationPath) {
    if (-not $Force) {
        throw "SDK destination already exists; pass -Force to replace it: $destinationPath"
    }
    Remove-Item -LiteralPath $destinationPath -Recurse -Force
}
if ($Archive -and (Test-Path -LiteralPath $archivePath)) {
    if (-not $Force) {
        throw "SDK archive already exists; pass -Force to replace it: $archivePath"
    }
    Remove-Item -LiteralPath $archivePath -Force
}

$stage = "$destinationPath.stage.$PID"
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}

$compilerName = if ($runningOnWindows) { "vyxc.exe" } else { "vyxc" }
$backendName = if ($runningOnWindows) {
    "vyx_compiler_backend.dll"
} elseif ($PSVersionTable.OS -match "Darwin") {
    "libvyx_compiler_backend.dylib"
} else {
    "libvyx_compiler_backend.so"
}
$runtimeName = if ($runningOnWindows) { "vyx_runtime.lib" } else { "libvyx_runtime.a" }

$required = [ordered]@{
    Compiler = Join-Path $out $compilerName
    Backend = Join-Path $out $backendName
    Runtime = Join-Path $out $runtimeName
}
foreach ($entry in $required.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
        throw "missing $($entry.Key) artifact: $($entry.Value)"
    }
}

function Copy-CleanTree([string]$Source, [string]$DestinationPath) {
    New-Item -ItemType Directory -Force -Path $DestinationPath | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        if ($item.PSIsContainer) {
            if ($item.Name -in @(".cache", "out", "__pycache__")) { continue }
            Copy-CleanTree -Source $item.FullName -DestinationPath (Join-Path $DestinationPath $item.Name)
        } else {
            Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $DestinationPath $item.Name)
        }
    }
}

function Copy-ResolvedFile([string]$Source, [string]$DestinationPath) {
    $item = Get-Item -LiteralPath $Source
    if (-not [string]::IsNullOrWhiteSpace($item.LinkType)) {
        $item = $item.ResolveLinkTarget($true)
    }
    Copy-Item -LiteralPath $item.FullName -Destination $DestinationPath
}

function Add-RelativeLink([string]$LinkPath, [string]$Target) {
    if (Test-Path -LiteralPath $LinkPath) { return }
    New-Item -ItemType SymbolicLink -Path $LinkPath -Target $Target | Out-Null
}

try {
    $bin = New-Item -ItemType Directory -Force -Path (Join-Path $stage "bin")
    $lib = New-Item -ItemType Directory -Force -Path (Join-Path $stage "lib")
    $share = New-Item -ItemType Directory -Force -Path (Join-Path $stage "share\vyx")

    $compilerPayloadName = if ($useLauncherLayout) { ".vyxc-bin" } else { $compilerName }
    Copy-Item -LiteralPath $required.Compiler -Destination (Join-Path $bin.FullName $compilerPayloadName)
    Copy-Item -LiteralPath $required.Backend -Destination (Join-Path $bin.FullName $backendName)
    Copy-Item -LiteralPath $required.Runtime -Destination (Join-Path $lib.FullName $runtimeName)

    $packagedTools = New-Object System.Collections.Generic.List[string]
    foreach ($toolName in @(
        $(if ($runningOnWindows) { "vyxc-lsp.exe" } else { "vyxc-lsp" }),
        $(if ($runningOnWindows) { "vyxc-dap.exe" } else { "vyxc-dap" })
    )) {
        $tool = Join-Path $out $toolName
        if (Test-Path -LiteralPath $tool -PathType Leaf) {
            $payloadName = if ($useLauncherLayout) { "." + $toolName + "-bin" } else { $toolName }
            Copy-Item -LiteralPath $tool -Destination (Join-Path $bin.FullName $payloadName)
            $packagedTools.Add($toolName)
        }
    }

    foreach ($directory in @("std", "std_packages", "tools")) {
        $source = Join-Path $bootstrap $directory
        if (Test-Path -LiteralPath $source -PathType Container) {
            Copy-CleanTree -Source $source -DestinationPath (Join-Path $stage $directory)
        }
    }
    Copy-CleanTree -Source (Join-Path (Join-Path $repoRoot "runtime") "src") `
        -DestinationPath (Join-Path $share.FullName "runtime")

    # Bundle the oneTBB runtime required by std.sync.thread_pool
    # (build_apply_std_auto_libs adds -ltbb12/-ltbb automatically; these
    # copies make the link resolve out of the SDK's own lib/ directory).
    if ($runningOnWindows) {
        $tbbDll = Join-Path $out "tbb12.dll"
        $tbbLib = Join-Path $out "tbb12.lib"
        if (Test-Path -LiteralPath $tbbDll -PathType Leaf) {
            Copy-Item -LiteralPath $tbbDll -Destination (Join-Path $lib.FullName "tbb12.dll")
            # The packaged compiler (bin/vyxc.exe) itself imports tbb12.dll
            # statically.  Windows resolves imports from the executable's own
            # directory, and the Windows layout has no launcher to widen the
            # search path (unlike the POSIX layout, whose launcher points
            # LD_LIBRARY_PATH at ../lib) -- so the DLL must also sit next to
            # the compiler.  Without this the SDK's own vyxc.exe fails to start
            # with "tbb12.dll: cannot open shared object file".
            Copy-Item -LiteralPath $tbbDll -Destination (Join-Path $bin.FullName "tbb12.dll")
        }
        if (Test-Path -LiteralPath $tbbLib -PathType Leaf) {
            Copy-Item -LiteralPath $tbbLib -Destination (Join-Path $lib.FullName "tbb12.lib")
        }
    } else {
        Get-ChildItem -LiteralPath $out -Filter "libtbb.so*" -File -ErrorAction SilentlyContinue |
            ForEach-Object {
                Copy-ResolvedFile -Source $_.FullName -Destination (Join-Path $lib.FullName $_.Name)
            }
        $tbbSo = Get-ChildItem -LiteralPath $lib.FullName -Filter "libtbb.so.12*" -File -ErrorAction SilentlyContinue |
            Sort-Object Name | Select-Object -Last 1
        if ($null -ne $tbbSo) {
            Add-RelativeLink -LinkPath (Join-Path $lib.FullName "libtbb.so") -Target $tbbSo.Name
        }
    }

    $dciSource = Join-Path (Join-Path $repoRoot "tools") "dci"
    if (Test-Path -LiteralPath $dciSource -PathType Container) {
        Copy-CleanTree -Source $dciSource -DestinationPath (Join-Path (Join-Path $stage "tools") "dci")
    }
    $toolsInit = Join-Path (Join-Path $repoRoot "tools") "__init__.py"
    if (Test-Path -LiteralPath $toolsInit -PathType Leaf) {
        $toolsDestination = Join-Path $stage "tools"
        New-Item -ItemType Directory -Force -Path $toolsDestination | Out-Null
        Copy-Item -LiteralPath $toolsInit -Destination (Join-Path $toolsDestination "__init__.py")
    }

    if ($BundleLlvm -and $runningOnWindows) {
        # The Windows SDK keeps bin/vyxc.exe as its entry point and relies on the
        # bootstrapped compiler's own toolchain discovery instead of a launcher
        # script: build_env_value("LLVM_ROOT") falls back to
        # build_default_llvm_root(), whose first candidate is <exe_dir>/clang,
        # and build_tool_path() probes <exe_dir>/clang/bin.  Shipping the
        # toolchain at bin/clang/ therefore makes the package self-contained
        # with no environment variables at all.  Every link goes through
        # `-fuse-ld=lld`, and clang resolves that lld-link.exe from its own bin
        # directory, so no PATH entry is needed either.
        #
        # Note this bundles the *LLVM* toolchain only.  clang still consumes the
        # host MSVC STL / Windows SDK headers and CRT, exactly as it does when
        # driven from the build tree; the official LLVM Windows build ships no
        # libc++, so a fully self-contained C++ standard library is not
        # available on this platform.
        $llvmRoot = (Resolve-Path -LiteralPath $env:LLVM_ROOT).Path
        $llvmBinSource = Join-Path $llvmRoot "bin"
        $clangRoot = New-Item -ItemType Directory -Force -Path (Join-Path (Join-Path $stage "bin") "clang")
        $clangBinDir = New-Item -ItemType Directory -Force -Path (Join-Path $clangRoot.FullName "bin")
        $clangLibDir = New-Item -ItemType Directory -Force -Path (Join-Path $clangRoot.FullName "lib")

        foreach ($toolName in @("clang.exe", "lld-link.exe", "llvm-ar.exe", "llvm-nm.exe", "llvm-strip.exe")) {
            $source = Join-Path $llvmBinSource $toolName
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                throw "missing bundled LLVM tool: $source"
            }
            Copy-Item -LiteralPath $source -Destination (Join-Path $clangBinDir.FullName $toolName)
        }

        # clang++ is byte-identical to clang (same driver, g++ defaults).  Link
        # it so a local SDK checkout does not store the ~100MB payload twice.
        $clangExe = Join-Path $clangBinDir.FullName "clang.exe"
        $clangxxExe = Join-Path $clangBinDir.FullName "clang++.exe"
        try {
            New-Item -ItemType HardLink -Path $clangxxExe -Target $clangExe -ErrorAction Stop | Out-Null
        } catch {
            Copy-Item -LiteralPath $clangExe -Destination $clangxxExe
        }

        # LLVM-C.dll is the runtime half of the LLVM-C binding; the compiler
        # copies it next to any program whose IR references the LLVM-C API.
        $llvmCDll = Join-Path $llvmBinSource "LLVM-C.dll"
        if (Test-Path -LiteralPath $llvmCDll -PathType Leaf) {
            Copy-Item -LiteralPath $llvmCDll -Destination (Join-Path $clangBinDir.FullName "LLVM-C.dll")
        }
        $llvmCLib = Join-Path (Join-Path $llvmRoot "lib") "LLVM-C.lib"
        if (Test-Path -LiteralPath $llvmCLib -PathType Leaf) {
            Copy-Item -LiteralPath $llvmCLib -Destination (Join-Path $clangLibDir.FullName "LLVM-C.lib")
        }

        Copy-CleanTree -Source (Join-Path $llvmRoot "include") `
            -DestinationPath (Join-Path $clangRoot.FullName "include")

        # clang's own resource directory (builtin headers + clang_rt libraries).
        $clangResourceDir = Get-ChildItem -Path (Join-Path (Join-Path $llvmRoot "lib") "clang") `
            -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name | Select-Object -Last 1
        if ($null -eq $clangResourceDir) {
            throw "no clang resource directory under $llvmRoot\lib\clang"
        }
        Copy-CleanTree -Source $clangResourceDir.FullName `
            -DestinationPath (Join-Path (Join-Path $clangLibDir.FullName "clang") $clangResourceDir.Name)

        # LLDB debug chain (-BundleLldb).  Sourced from a prepared bundle
        # directory (bootstrap_compiler/out/_lldb_bundle) rather than the LLVM
        # dev tree, which ships no LLDB.  Contents:
        #   bin/  : lldb.exe lldb-dap.exe lldb-server.exe lldb-argdumper.exe
        #           liblldb.dll + the official python-3.11.9 embeddable runtime
        #           (python311.dll / python311.zip / python311._pth, extension
        #           .pyd modules, libcrypto/libssl/sqlite3/ffi, vcruntime)
        #   site-packages/lldb : the LLDB Python module (its native/ copy of
        #           liblldb.dll removed; the CLI binds liblldb.dll directly)
        # liblldb.dll statically imports python311.dll, so the Python runtime
        # is not optional: without it every LLDB binary dies before main().
        # Verified on this bundle layout: lldb.exe --version, `script`
        # evaluation with the lldb module, and full breakpoint / frame-variable
        # debugging of a clang -gdwarf binary.
        if ($BundleLldb) {
            $lldbBundle = if ([string]::IsNullOrWhiteSpace($LldbBundleRoot)) {
                Join-Path $out "_lldb_bundle"
            } else {
                (Resolve-Path -LiteralPath $LldbBundleRoot).Path
            }
            $lldbBinSource = Join-Path $lldbBundle "bin"
            if (-not (Test-Path -LiteralPath $lldbBinSource -PathType Container)) {
                throw "-BundleLldb requires the lldb bundle directory: $lldbBinSource"
            }
            foreach ($item in (Get-ChildItem -LiteralPath $lldbBinSource -File)) {
                Copy-Item -LiteralPath $item.FullName -Destination $clangBinDir.FullName
            }
            $lldbSiteSource = Join-Path $lldbBundle "site-packages"
            if (Test-Path -LiteralPath $lldbSiteSource -PathType Container) {
                Copy-CleanTree -Source $lldbSiteSource `
                    -DestinationPath (Join-Path $clangBinDir.FullName "site-packages")
            }
        }
    }

    if ($BundleLlvm -and -not $runningOnWindows) {
        $llvmRoot = (Resolve-Path -LiteralPath $env:LLVM_ROOT).Path
        $llvmBin = Join-Path $llvmRoot "bin"
        $toolchain = New-Item -ItemType Directory -Force -Path (Join-Path $stage "toolchain")
        $toolchainBin = New-Item -ItemType Directory -Force -Path (Join-Path $toolchain.FullName "bin")
        $toolchainLib = New-Item -ItemType Directory -Force -Path (Join-Path $toolchain.FullName "lib")
        $lddInputs = New-Object System.Collections.Generic.List[string]
        $lddInputs.Add($required.Backend)

        foreach ($toolName in @("clang", "llvm-ar", "llvm-nm", "llvm-readobj", "llvm-strip", "lld")) {
            $source = Join-Path $llvmBin $toolName
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                if ($toolName -eq "llvm-strip") { continue }
                throw "missing bundled LLVM tool: $source"
            }
            Copy-ResolvedFile -Source $source -DestinationPath (Join-Path $toolchainBin.FullName $toolName)
            $lddInputs.Add($source)
        }
        Add-RelativeLink -LinkPath (Join-Path $toolchainBin.FullName "clang++") -Target "clang"
        Add-RelativeLink -LinkPath (Join-Path $toolchainBin.FullName "llvm-ranlib") -Target "llvm-ar"
        Add-RelativeLink -LinkPath (Join-Path $toolchainBin.FullName "ld.lld") -Target "lld"
        Add-RelativeLink -LinkPath (Join-Path $toolchainBin.FullName "ld") -Target "lld"

        Copy-CleanTree -Source (Join-Path $llvmRoot "include") `
            -DestinationPath (Join-Path $toolchain.FullName "include")
        $clangBuiltinRoot = Join-Path (Join-Path (Join-Path $llvmRoot "lib") "clang") "22"
        Copy-CleanTree -Source (Join-Path $clangBuiltinRoot "include") `
            -DestinationPath (Join-Path (Join-Path (Join-Path $toolchainLib.FullName "clang") "22") "include")

        $skipLibraries = @("libc.so.6", "libm.so.6", "ld-linux-x86-64.so.2")
        foreach ($inputPath in $lddInputs) {
            foreach ($line in (& ldd $inputPath)) {
                if ($line -notmatch '^\s*(\S+)\s+=>\s+(\S+)\s+\(') { continue }
                $soname = $Matches[1]
                $resolvedLibrary = $Matches[2]
                if ($soname -in $skipLibraries -or $resolvedLibrary -eq "not") { continue }
                $destinationLibrary = Join-Path $lib.FullName $soname
                if (-not (Test-Path -LiteralPath $destinationLibrary)) {
                    Copy-ResolvedFile -Source $resolvedLibrary -DestinationPath $destinationLibrary
                }
            }
        }

        foreach ($linkSpec in @(
            @("libLLVM.so", "libLLVM.so*"),
            @("libz.so", "libz.so.*"),
            @("libzstd.so", "libzstd.so.*")
        )) {
            $targetLibrary = Get-ChildItem -LiteralPath $lib.FullName -Filter $linkSpec[1] -File |
                Sort-Object Name |
                Select-Object -First 1
            if ($null -ne $targetLibrary) {
                Add-RelativeLink -LinkPath (Join-Path $toolchainLib.FullName $linkSpec[0]) `
                    -Target ("../../lib/" + $targetLibrary.Name)
            }
        }

        $launcher = @'
#!/usr/bin/env sh
set -eu
VYX_HOME=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export VYX_HOME
export LLVM_ROOT=${LLVM_ROOT:-"$VYX_HOME/toolchain"}
export PATH="$VYX_HOME/toolchain/bin:$PATH"
export LD_LIBRARY_PATH="$VYX_HOME/bin:$VYX_HOME/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBRARY_PATH="$VYX_HOME/lib:$VYX_HOME/toolchain/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
case "$(basename -- "$0")" in
  vyxc|boot) executable=.vyxc-bin ;;
  vyxc-lsp) executable=.vyxc-lsp-bin ;;
  vyxc-dap) executable=.vyxc-dap-bin ;;
  *) echo "unknown Vyx launcher: $0" >&2; exit 2 ;;
esac
exec "$VYX_HOME/bin/$executable" "$@"
'@
        $launcher = $launcher.Replace("`r`n", "`n")
        $launchers = @("vyxc", "boot") + @($packagedTools)
        foreach ($launcherName in $launchers) {
            $launcherPath = Join-Path $bin.FullName $launcherName
            Set-Content -LiteralPath $launcherPath -Value $launcher -Encoding utf8NoBOM -NoNewline
            & chmod +x $launcherPath
            if ($LASTEXITCODE -ne 0) { throw "chmod failed for $launcherPath" }
        }
        & chmod +x (Join-Path $bin.FullName ".vyxc-bin")
        foreach ($toolName in $packagedTools) {
            & chmod +x (Join-Path $bin.FullName ("." + $toolName + "-bin"))
        }
        Get-ChildItem -LiteralPath $toolchainBin.FullName -File |
            ForEach-Object { & chmod +x $_.FullName }
    }

    $manifest = [ordered]@{
        schema = 1
        compiler = "bin/$compilerName"
        compiler_backend = "bin/$backendName"
        application_runtime = "lib/$runtimeName"
        application_runtime_linkage = "static"
        application_runtime_language = "Vyx"
        llvm_required_by_applications = $false
        bundled_compiler_llvm_toolchain = [bool]$BundleLlvm
        bundled_lldb = [bool]$BundleLldb
    }
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $stage "SDK-MANIFEST.json") -Encoding utf8NoBOM

    $hashLines = Get-ChildItem -LiteralPath $stage -File -Recurse |
        Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
        Sort-Object FullName |
        ForEach-Object {
            $relative = [System.IO.Path]::GetRelativePath($stage, $_.FullName).Replace("\", "/")
            "{0}  {1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
        }
    $hashLines | Set-Content -LiteralPath (Join-Path $stage "SHA256SUMS.txt") -Encoding ascii

    Move-Item -LiteralPath $stage -Destination $destinationPath
    Write-Host "SDK package: $destinationPath"
    if ($Archive) {
        if ($runningOnWindows) {
            Compress-Archive -LiteralPath $destinationPath -DestinationPath $archivePath `
                -CompressionLevel Optimal
        } else {
            $leaf = Split-Path -Leaf $destinationPath
            & tar -czf $archivePath -C $destinationParent $leaf
            if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }
        }
        Write-Host "SDK archive: $archivePath"
    }
} catch {
    # Report the real failure first: the stage cleanup below can itself fail
    # (e.g. ERROR_NO_SYSTEM_RESOURCES on a large tree), and a catching block
    # that throws before rethrowing hides the original cause entirely.
    [Console]::Error.WriteLine("package_sdk: staging failed: " + $_.Exception.ToString())
    if (Test-Path -LiteralPath $stage) {
        try {
            Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
        } catch {
            [Console]::Error.WriteLine("package_sdk: stage cleanup failed (left in place): " + $_.Exception.Message)
        }
    }
    throw
}

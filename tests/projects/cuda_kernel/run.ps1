# End-to-end gate for "Vyx directly on CUDA":
#
# 1. `vyx build` lowers the manifest device source directly to PTX, embeds
#    that PTX plus its kernel ABI, and links the CUDA driver import library.
# 2. The gate asserts that the cached PTX has one `.visible .entry` and the
#    requested architecture, then cross-checks it with toolkit `ptxas`.
# 3. The host executable is launched from the project root. It must not need
#    `.cache/device.ptx` or `device_entry.txt` in its working directory.
# 4. The host loads the embedded PTX, launches the kernel, and verifies the
#    verifies the GPU-computed results.
#
# SKIP (exit 77) when this is not Windows, no CUDA toolkit/driver library is
# present. A driver that cannot launch the kernel is a real failure, not a skip.

param(
    [string]$BootstrapCompiler = "",
    [string]$DevArch = "sm_120"
)

$ErrorActionPreference = "Stop"

if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    if (-not $IsWindows) {
        Write-Host "cuda_kernel: SKIP (Vyx CUDA kernels target the Windows/NVIDIA driver stack)"
        exit 77
    }
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
$projectRoot = $PSScriptRoot
$cache = Join-Path $projectRoot ".cache"
$targetDir = Join-Path $projectRoot "target"

function Fail-Test([string]$Reason) {
    Write-Host "cuda_kernel: FAIL ($Reason)"
    exit 1
}

function Write-Skip([string]$Reason) {
    Write-Host "cuda_kernel: SKIP ($Reason)"
    exit 77
}

if (-not (Test-Path -LiteralPath $cache -PathType Container)) {
    New-Item -ItemType Directory -Path $cache | Out-Null
}

# --- toolchain locations -----------------------------------------------------

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
if (-not (Test-Path -LiteralPath $BootstrapCompiler -PathType Leaf)) {
    Write-Skip "bootstrap compiler not found"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path

# CUDA toolkit: override with VYX_CUDA_TOOLKIT_ROOT; otherwise pick the
# highest installed toolkit that ships cuda.lib.
$cudaRoot = $env:VYX_CUDA_TOOLKIT_ROOT
if ([string]::IsNullOrWhiteSpace($cudaRoot)) {
    $toolkitRoot = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA"
    if (Test-Path -LiteralPath $toolkitRoot) {
        foreach ($dir in (Get-ChildItem -LiteralPath $toolkitRoot -Directory | Sort-Object Name -Descending)) {
            if (Test-Path -LiteralPath (Join-Path $dir.FullName "lib\x64\cuda.lib")) {
                $cudaRoot = $dir.FullName
                break
            }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($cudaRoot)) {
    Write-Skip "no CUDA toolkit with lib\x64\cuda.lib found"
}
$cudaLib = Join-Path $cudaRoot "lib\x64\cuda.lib"
if (-not (Test-Path -LiteralPath $cudaLib -PathType Leaf)) {
    Write-Skip "cuda.lib not found under $cudaRoot"
}

Push-Location $projectRoot
try {
    $buildOut = Join-Path $cache "host_build.out.log"
    $buildErr = Join-Path $cache "host_build.err.log"
    & $BootstrapCompiler build -j 10 > $buildOut 2> $buildErr
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $buildErr -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        Fail-Test "project build failed"
    }
    $devicePtx = Join-Path $cache "cuda_cuda_kernel.ptx"
    $ptxText = Get-Content -Raw -LiteralPath $devicePtx
    if ($ptxText.IndexOf(".visible .entry", [StringComparison]::Ordinal) -lt 0) {
        Fail-Test "embedded build did not emit a .visible .entry"
    }
    if ($ptxText.IndexOf(".target " + $DevArch, [StringComparison]::Ordinal) -lt 0) {
        Fail-Test "PTX targets a different architecture than $DevArch"
    }
    if ($ptxText.IndexOf("vyx_rt_set_args", [StringComparison]::Ordinal) -ge 0 -or
        $ptxText.IndexOf("_vyx_argv", [StringComparison]::Ordinal) -ge 0) {
        Fail-Test "PTX still contains host argument runtime symbols"
    }
    $entry = [regex]::Match($ptxText, '(?m)^\.visible \.entry\s+([A-Za-z0-9_]+)\(')
    if (-not $entry.Success) { Fail-Test "PTX kernel entry symbol is missing" }
    $embedSource = Get-Content -Raw -LiteralPath (Join-Path $cache 'cuda_cuda_kernel_embed.vyx')
    $embeddedSymbol = [regex]::Match($embedSource, 'vyx_cuda_kernel_scale_kernel_symbol\(\) -> string \{ return "([A-Za-z0-9_]+)"; \}')
    if (-not $embeddedSymbol.Success -or $embeddedSymbol.Groups[1].Value -ne $entry.Groups[1].Value) {
        Fail-Test "embedded CUDA symbol differs from the PTX entry"
    }

    # ptxas cross-check (evidence the PTX is valid SASS-source for this arch);
    # the driver JIT in cuModuleLoadData is the path that actually compiles it.
    $ptxas = Join-Path $cudaRoot "bin\ptxas.exe"
    if (Test-Path -LiteralPath $ptxas -PathType Leaf) {
        $cubin = Join-Path $cache "device.cubin"
        $ptxasLog = Join-Path $cache "device_ptxas.out.log"
        & $ptxas "-arch=$DevArch" -o $cubin $devicePtx > $ptxasLog 2>&1
        if ($LASTEXITCODE -ne 0) {
            Get-Content -LiteralPath $ptxasLog | ForEach-Object { Write-Host $_ }
            Fail-Test "ptxas rejected the Vyx-generated PTX"
        }
        Write-Host "[device] ptxas accepted PTX"
    }
    $hostExe = Join-Path $targetDir "cuda_kernel.exe"
    if (-not (Test-Path -LiteralPath $hostExe -PathType Leaf)) {
        Fail-Test "host executable missing after build"
    }
    Write-Host "[host] built"

    # The executable carries its own PTX and kernel metadata. Running from the
    # project root proves it no longer depends on the cache working directory.

    $runOut = Join-Path $cache "host_run.out.log"
    $runErr = Join-Path $cache "host_run.err.log"
    & $hostExe > $runOut 2> $runErr
    $runRc = $LASTEXITCODE
    $runText = ""
    if (Test-Path -LiteralPath $runOut) { $runText = Get-Content -Raw -LiteralPath $runOut }
    Write-Host $runText.TrimEnd()
    if ($runRc -ne 0) {
        if (Test-Path -LiteralPath $runErr) {
            Get-Content -LiteralPath $runErr | ForEach-Object { Write-Host $_ }
        }
        Fail-Test "host run exited with $runRc"
    }
    if ($runText.IndexOf("cuda_kernel: OK", [StringComparison]::Ordinal) -lt 0) {
        Fail-Test "host run did not report cuda_kernel: OK"
    }
} finally {
    Pop-Location
}

Write-Host "cuda_kernel: OK"
exit 0

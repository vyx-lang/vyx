$ErrorActionPreference = "Stop"

# ── Windows crash-popup suppression ────────────────────────────────────────────
# `vyxc` and any child it spawns occasionally hit access violations (0xC0000005)
# while the bootstrap pipeline is still partial. Without this block, Windows
# pops the "<process> has stopped working" dialog and the script blocks
# until the user clicks "Close program", which kills CI throughput.
#
# SetErrorMode flags (winbase.h):
#   SEM_FAILCRITICALERRORS  = 0x0001  → no critical-error message box
#   SEM_NOGPFAULTERRORBOX   = 0x0002  → no general-protection-fault dialog
#   SEM_NOOPENFILEERRORBOX  = 0x8000  → no "file not found" dialog
# We additionally export `SEM_NOGPFAULTERRORBOX=1` so child processes inherit
# the same intent through the legacy CRT path. Both calls are wrapped in
# try/catch because Add-Type fails the second time the script is dot-sourced
# in the same session ("Win32.Sem already exists").
$env:SEM_NOGPFAULTERRORBOX = '1'
try {
    Add-Type -Namespace Win32 -Name Sem -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);' -ErrorAction SilentlyContinue
} catch {}
try {
    [Win32.Sem]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x8000) | Out-Null
} catch {}
. (Join-Path $PSScriptRoot "VyxTestProcess.ps1")

# Build the Phase-0 bootstrap project.
# Spec form: `vyxc bootstrap_compiler/Vyx.toml -o boot_a.exe`. The current
# host driver does not yet accept a manifest path as input, so until the
# project-mode CLI lands we forward to single-file mode from the workspace
# root (where `std/` resolves automatically). The output binary still ends
# up at `bootstrap_compiler/boot_a.exe` either way.

$root = Split-Path -Parent $PSScriptRoot
$ws   = Split-Path -Parent $root
$entry = Join-Path $root "src/core/main.vyx"
$out   = Join-Path $root "boot_a.exe"

if (-not (Test-Path $entry)) {
    Write-Error "entry $entry missing"
    exit 2
}

$candidates = @(
    (Join-Path $root "out/vyxc.exe"),
    "vyxc",
    (Join-Path $ws "build_yolo_vyxcg/vyxc.exe"),
    (Join-Path $ws "build_vyxcg/vyxc.exe"),
    (Join-Path $ws "build/vyxc.exe"),
    (Join-Path $ws "cmake-build-debug/vyxc.exe")
)
$vyxc = $null
foreach ($c in $candidates) {
    if ($c -eq "vyxc") {
        if (Get-Command vyxc -ErrorAction SilentlyContinue) { $vyxc = "vyxc"; break }
    } elseif (Test-Path $c) { $vyxc = $c; break }
}
if (-not $vyxc) {
    Write-Error "vyxc not found in PATH, build_yolo_vyxcg/, build_vyxcg/, build/, or cmake-build-debug/"
    exit 3
}

# Detect the private compiler backend.
# When present, append its link directory/name so main.vyx's reference to
# `bootstrap.codegen` (which transitively pulls in `vyx_rt_*` extern decls)
# resolves at link time. Both Windows (.lib) and POSIX (.dll.a / .so /
# .dylib symlink) layouts are checked.
$rtCandidates = @(
    [pscustomobject]@{ Dir = (Join-Path $root "out"); Lib = "vyx_compiler_backend" },
    [pscustomobject]@{ Dir = (Join-Path $ws "build_yolo_vyxcg/vyx_codegen"); Lib = "vyx_rt" },
    [pscustomobject]@{ Dir = (Join-Path $ws "build_vyxcg/vyx_codegen"); Lib = "vyx_rt" },
    [pscustomobject]@{ Dir = (Join-Path $ws "build/vyx_codegen"); Lib = "vyx_rt" },
    [pscustomobject]@{ Dir = (Join-Path $ws "cmake-build-debug/vyx_codegen"); Lib = "vyx_rt" }
)
$rtArgs = @()
foreach ($candidate in $rtCandidates) {
    $d = $candidate.Dir
    $libName = $candidate.Lib
    if ((Test-Path (Join-Path $d ($libName + ".lib"))) -or
        (Test-Path (Join-Path $d ("lib" + $libName + ".dll.a"))) -or
        (Test-Path (Join-Path $d ("lib" + $libName + ".so"))) -or
        (Test-Path (Join-Path $d ("lib" + $libName + ".dylib")))) {
        $rtArgs = @("-L", $d, "-l", $libName)
        Write-Host "[bootstrap] linking compiler backend from $d"
        break
    }
}

Push-Location $ws
try {
    $rtArgsStr = ($rtArgs -join ' ')
    Write-Host "[bootstrap] (cwd=$ws) $vyxc --src=file bootstrap_compiler/src/core/main.vyx -o $out $rtArgsStr"
    $logDir = Join-Path $root "out/build_logs"
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
    $buildArgs = @("--src=file", "bootstrap_compiler/src/core/main.vyx", "-o", $out) + $rtArgs
    $run = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList $buildArgs `
        -WorkingDirectory $ws `
        -StdoutLog (Join-Path $logDir "boot_a.build.out.log") `
        -StderrLog (Join-Path $logDir "boot_a.build.err.log") `
        -DialogLog (Join-Path $logDir "boot_a.build.dialog.log") `
        -TimeoutSec 120
    $code = $run.ExitCode
    if ($run.DialogCaught -or $run.TimedOut) {
        Write-Host "[bootstrap] captured dialog/timeout log=$($run.DialogLog)"
    }
    if ($code -eq 0 -and $rtArgs.Count -gt 0) {
        $linkedName = $rtArgs[3]
        $runtimeDll = Join-Path $rtArgs[1] ($linkedName + ".dll")
        if (Test-Path $runtimeDll) {
            Copy-Item -LiteralPath $runtimeDll -Destination (Join-Path $root ($linkedName + ".dll")) -Force
        }
        $runtimeLib = Join-Path $rtArgs[1] ($linkedName + ".lib")
        if (Test-Path $runtimeLib) {
            Copy-Item -LiteralPath $runtimeLib -Destination (Join-Path $root ($linkedName + ".lib")) -Force
        }
        $runtimeDllA = Join-Path $rtArgs[1] ("lib" + $linkedName + ".dll.a")
        if (Test-Path $runtimeDllA) {
            Copy-Item -LiteralPath $runtimeDllA -Destination (Join-Path $root ("lib" + $linkedName + ".dll.a")) -Force
        }
    }
} finally {
    Pop-Location
}
exit $code

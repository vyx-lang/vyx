# Build four per-stage probe executables. All of them link the private
# compiler backend.

$ErrorActionPreference = "Stop"
$env:SEM_NOGPFAULTERRORBOX = "1"
try {
    Add-Type -Namespace Win32 -Name Sem -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);' -ErrorAction SilentlyContinue
} catch {}
try { [Win32.Sem]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x8000) | Out-Null } catch {}
. (Join-Path $PSScriptRoot "VyxTestProcess.ps1")

$root = Split-Path -Parent $PSScriptRoot
$ws   = Split-Path -Parent $root
$out  = Join-Path $root "out"
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out | Out-Null }

$candidates = @()
if ($env:VYXC) { $candidates += $env:VYXC }
$candidates += @(
    (Join-Path $out "vyxc.exe"),
    (Join-Path $ws "cmake-build-debug/vyxc.exe"),
    (Join-Path $ws "build/vyxc.exe"),
    (Join-Path $ws "build_vyxcg/vyxc.exe"),
    "vyxc"
)
$vyxc = $null
foreach ($c in $candidates) {
    if ($c -eq "vyxc") {
        if (Get-Command vyxc -ErrorAction SilentlyContinue) { $vyxc = "vyxc"; break }
    } elseif (Test-Path $c) { $vyxc = $c; break }
}
if (-not $vyxc) {
    Write-Error "vyxc not found"
    exit 3
}

$rtCandidates = @(
    $out,
    (Join-Path $ws "build_vyxcg/vyx_rt"),
    (Join-Path $ws "build/vyx_rt"),
    (Join-Path $ws "cmake-build-debug/vyx_codegen")
)
$rtArgs = @()
foreach ($d in $rtCandidates) {
    if ((Test-Path (Join-Path $d "vyx_compiler_backend.lib")) -or
        (Test-Path (Join-Path $d "libvyx_compiler_backend.dll.a")) -or
        (Test-Path (Join-Path $d "libvyx_compiler_backend.so")) -or
        (Test-Path (Join-Path $d "libvyx_compiler_backend.dylib"))) {
        $rtArgs = @("-L", $d, "-l", "vyx_compiler_backend")
        Write-Host "[probes] linking compiler backend from $d"
        break
    }
    if ((Test-Path (Join-Path $d "vyx_rt.lib")) -or
        (Test-Path (Join-Path $d "libvyx_rt.dll.a")) -or
        (Test-Path (Join-Path $d "libvyx_rt.so")) -or
        (Test-Path (Join-Path $d "libvyx_rt.dylib"))) {
        $rtArgs = @("-L", $d, "-l", "vyx_rt")
        Write-Host "[probes] linking legacy compiler backend from $d"
        break
    }
}

Push-Location $ws
try {
    $lex = Join-Path $root "tools/probe_lex.vyx"
    $par = Join-Path $root "tools/probe_parse.vyx"
    $sem = Join-Path $root "tools/probe_sema.vyx"
    $emi = Join-Path $root "tools/probe_emit.vyx"

    $logDir = Join-Path $out "build_probe_logs"
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }

    $lexArgs = @("--src=file", $lex, "-o", (Join-Path $out "probe_lex.exe")) + $rtArgs
    $runLex = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList $lexArgs `
        -WorkingDirectory $ws `
        -StdoutLog (Join-Path $logDir "probe_lex.build.out.log") `
        -StderrLog (Join-Path $logDir "probe_lex.build.err.log") `
        -DialogLog (Join-Path $logDir "probe_lex.build.dialog.log") `
        -TimeoutSec 120
    if ($runLex.ExitCode -ne 0) { exit $runLex.ExitCode }

    $parseArgs = @("--src=file", $par, "-o", (Join-Path $out "probe_parse.exe")) + $rtArgs
    $runParse = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList $parseArgs `
        -WorkingDirectory $ws `
        -StdoutLog (Join-Path $logDir "probe_parse.build.out.log") `
        -StderrLog (Join-Path $logDir "probe_parse.build.err.log") `
        -DialogLog (Join-Path $logDir "probe_parse.build.dialog.log") `
        -TimeoutSec 120
    if ($runParse.ExitCode -ne 0) { exit $runParse.ExitCode }

    $semaArgs = @("--src=file", $sem, "-o", (Join-Path $out "probe_sema.exe")) + $rtArgs
    $runSema = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList $semaArgs `
        -WorkingDirectory $ws `
        -StdoutLog (Join-Path $logDir "probe_sema.build.out.log") `
        -StderrLog (Join-Path $logDir "probe_sema.build.err.log") `
        -DialogLog (Join-Path $logDir "probe_sema.build.dialog.log") `
        -TimeoutSec 120
    if ($runSema.ExitCode -ne 0) { exit $runSema.ExitCode }

    $emitOut = Join-Path $out "probe_emit.exe"
    $emitCmd = "$vyxc --src=file $emi -o $emitOut $($rtArgs -join ' ')"
    Write-Host "[probes] $emitCmd"
    $emitArgs = @("--src=file", $emi, "-o", $emitOut) + $rtArgs
    $runEmit = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList $emitArgs `
        -WorkingDirectory $ws `
        -StdoutLog (Join-Path $logDir "probe_emit.build.out.log") `
        -StderrLog (Join-Path $logDir "probe_emit.build.err.log") `
        -DialogLog (Join-Path $logDir "probe_emit.build.dialog.log") `
        -TimeoutSec 120
    if ($runEmit.ExitCode -ne 0) {
        Write-Host "[probes] WARNING: probe_emit needs the private compiler backend (see build.ps1)"
        if ($runEmit.DialogCaught -or $runEmit.TimedOut) {
            Write-Host "[probes] captured dialog/timeout log=$($runEmit.DialogLog)"
        }
        exit $runEmit.ExitCode
    }
    Write-Host "[probes] wrote probe_lex.exe, probe_parse.exe, probe_sema.exe, probe_emit.exe -> $out"
} finally {
    Pop-Location
}
exit 0

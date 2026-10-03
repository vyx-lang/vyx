$ErrorActionPreference = "Stop"

# Per-stage probes on samples/bootstrap/*.vyx
# Lex/parse/sema probes do not link vyx_codegen.
#
# Host parity gates (vyxc vs probes): see
#   tests/bootstrap/lexer_conformance/run.ps1
#   tests/bootstrap/parse_conformance/run.ps1
#   tests/bootstrap/sema_conformance/run.ps1
#   scripts/test_parse_sema_conformance.ps1

$root = Split-Path -Parent $PSScriptRoot
$ws   = Split-Path -Parent $root
$samplesDir = Join-Path $ws "samples/bootstrap"
$outDir = Join-Path $root "out"
$helper = Join-Path $PSScriptRoot "VyxTestProcess.ps1"
. $helper

$probeLex   = Join-Path $outDir "probe_lex.exe"
$probeParse = Join-Path $outDir "probe_parse.exe"
$probeSema  = Join-Path $outDir "probe_sema.exe"
$probeEmit  = Join-Path $outDir "probe_emit.exe"

foreach ($p in @($probeLex, $probeParse, $probeSema)) {
    if (-not (Test-Path $p)) {
        Write-Host "Run bootstrap_compiler/scripts/build_probes.ps1 first (missing $p)"
        exit 2
    }
}

$samples = Get-ChildItem -Path $samplesDir -Filter "*.vyx" -File | Sort-Object Name
if ($samples.Count -eq 0) {
    Write-Host "No samples in $samplesDir"
    exit 2
}

$rtCandidates = @(
    (Join-Path $ws "bootstrap_compiler/out"),
    (Join-Path $ws "bootstrap_compiler"),
    (Join-Path $ws "cmake-build-debug/vyx_codegen"),
    (Join-Path $ws "build_vyxcg/vyx_rt"),
    (Join-Path $ws "build/vyx_rt")
)
foreach ($d in $rtCandidates) {
    if ((Test-Path (Join-Path $d "vyx_compiler_backend.dll")) -or
        (Test-Path (Join-Path $d "libvyx_compiler_backend.so")) -or
        (Test-Path (Join-Path $d "libvyx_compiler_backend.dylib")) -or
        (Test-Path (Join-Path $d "vyx_rt.dll")) -or
        (Test-Path (Join-Path $d "libvyx_rt.so")) -or
        (Test-Path (Join-Path $d "libvyx_rt.dylib"))) {
        $env:Path = $d + ";" + $env:Path
        break
    }
}

$fail = $false
$logDir = Join-Path $outDir "pipeline_logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
foreach ($s in $samples) {
    Write-Host "=== $($s.Name) ==="
    foreach ($pair in @(
        @{ Name = "lex";   Exe = $probeLex },
        @{ Name = "parse"; Exe = $probeParse },
        @{ Name = "sema";  Exe = $probeSema }
    )) {
        $safe = $s.BaseName + "." + $pair.Name
        $run = Invoke-VyxProcess -FilePath $pair.Exe `
            -ArgumentList @($s.FullName) `
            -StdoutLog (Join-Path $logDir ($safe + ".out.log")) `
            -StderrLog (Join-Path $logDir ($safe + ".err.log")) `
            -DialogLog (Join-Path $logDir ($safe + ".dialog.log")) `
            -TimeoutSec 25
        $code = $run.ExitCode
        $ok = ($code -eq 0)
        if (-not $ok) { $fail = $true }
        $sym = if ($ok) { "PASS" } else { "FAIL" }
        Write-Host ("  {0,-8} {1} exit={2}" -f $pair.Name, $sym, $code)
        if ($run.DialogCaught -or $run.TimedOut) {
            Write-Host ("           captured dialog/timeout log={0}" -f $run.DialogLog)
        }
    }
    if (Test-Path $probeEmit) {
        $tmpLl = Join-Path $outDir ($s.BaseName + ".probe.ll")
        $run = Invoke-VyxProcess -FilePath $probeEmit `
            -ArgumentList @($s.FullName, $tmpLl) `
            -StdoutLog (Join-Path $logDir ($s.BaseName + ".emit.out.log")) `
            -StderrLog (Join-Path $logDir ($s.BaseName + ".emit.err.log")) `
            -DialogLog (Join-Path $logDir ($s.BaseName + ".emit.dialog.log")) `
            -TimeoutSec 30
        $code = $run.ExitCode
        $ok = ($code -eq 0)
        if (-not $ok) { $fail = $true }
        $sym = if ($ok) { "PASS" } else { "FAIL" }
        Write-Host ("  {0,-8} {1} exit={2}" -f "emit", $sym, $code)
        if ($run.DialogCaught -or $run.TimedOut) {
            Write-Host ("           captured dialog/timeout log={0}" -f $run.DialogLog)
        }
    } else {
        Write-Host "  emit     SKIP (probe_emit.exe missing — run build_probes.ps1 with vyx_rt)"
    }
}

if ($fail) {
    Write-Host "`nOne or more stages FAILED."
    exit 1
}
Write-Host "`nAll probe stages PASS."
Write-Host "artifacts: $logDir"
exit 0

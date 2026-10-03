# Compare bootstrap `probe_parse` to host `vyxc --stop-after-parse` (same exit semantics:
# 0 = parse succeeded, non-zero = lex/parse failure).
#
# Usage:
#   pwsh tests/bootstrap/parse_conformance/run.ps1
#   pwsh .../run.ps1 -Path "e:\repo\tests\cases"
#   pwsh .../run.ps1 -Samples
#
# Requires: rebuilt host `vyxc` with `--stop-after-parse` (see `src/main.cpp`),
#           `bootstrap_compiler/scripts/build_probes.ps1` for `probe_parse.exe`.

param(
    [string]$Path = "",
    [switch]$Samples
)

$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$testsRoot = (Resolve-Path (Join-Path $here "..\..")).Path
$ws = Split-Path -Parent $testsRoot
$bootstrapRoot = Join-Path $ws "bootstrap_compiler"
$outDir = Join-Path $bootstrapRoot "out"
$helper = Join-Path $bootstrapRoot "scripts/VyxTestProcess.ps1"
. $helper
$probe = Join-Path $outDir "probe_parse.exe"
$samplesDir = Join-Path $ws "samples/bootstrap"
$defaultTests = Join-Path $testsRoot "cases"

$vyxc = $env:VYXC
if (-not $vyxc) {
    # Prefer workspace build so `--stop-after-parse` exists (PATH vyxc may be stale).
    foreach ($c in @(
            (Join-Path $ws "build_vyxcg/vyxc.exe"),
            (Join-Path $ws "build/vyxc.exe"),
            (Join-Path $ws "cmake-build-debug/vyxc.exe"),
            "vyxc"
        )) {
        if ($c -eq "vyxc") {
            if (Get-Command vyxc -ErrorAction SilentlyContinue) { $vyxc = "vyxc"; break }
        } elseif (Test-Path $c) { $vyxc = $c; break }
    }
}

if (-not (Test-Path $probe)) {
    Write-Error "Missing $probe — run bootstrap_compiler/scripts/build_probes.ps1"
    exit 2
}
if (-not $vyxc) {
    Write-Error "No host vyxc (set VYXC or place vyxc.exe under build/)"
    exit 2
}

if ($Samples) {
    $scanRoot = $samplesDir
    $files = Get-ChildItem -Path $scanRoot -Filter "*.vyx" -File -ErrorAction SilentlyContinue | Sort-Object Name
} elseif ($Path) {
    $scanRoot = (Resolve-Path $Path).Path
    $files = Get-ChildItem -Path $scanRoot -Recurse -Filter "*.vyx" -File -ErrorAction SilentlyContinue | Sort-Object FullName
} else {
    $scanRoot = $defaultTests
    if (-not (Test-Path $scanRoot)) {
        Write-Error "Tests directory missing: $scanRoot"
        exit 2
    }
    $files = Get-ChildItem -Path $scanRoot -Recurse -Filter "*.vyx" -File | Sort-Object FullName
}

if ($files.Count -eq 0) {
    Write-Error "No .vyx files under $scanRoot"
    exit 2
}

$ok = 0
$diff = 0
$probeFail = 0
$crash = 0
$n = 0
$logDir = Join-Path $outDir "parse_conformance_logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
foreach ($f in $files) {
    $n++
    if (($n % 50) -eq 1) {
        Write-Host "[parse] $n / $($files.Count) ..."
    }

    $full = $f.FullName
    $rel = $full.Substring($scanRoot.Length).TrimStart("\", "/")
    $safe = ($rel -replace '[\\/:*?"<>|]', '_')
    if ([string]::IsNullOrWhiteSpace($safe)) { $safe = $f.Name }

    $parent = Split-Path $full
    $stash = Join-Path $parent (Join-Path ".cache" ($f.BaseName + ".vyx.cache"))
    if (Test-Path $stash) { Remove-Item -Force $stash }

    $hostRun = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList @("--stop-after-parse", "--src=file", $full) `
        -StdoutLog (Join-Path $logDir ($safe + ".host.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".host.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".host.dialog.log")) `
        -TimeoutSec 20
    $hostOk = ($hostRun.ExitCode -eq 0)

    $probeRun = Invoke-VyxProcess -FilePath $probe `
        -ArgumentList @($full) `
        -StdoutLog (Join-Path $logDir ($safe + ".probe.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".probe.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".probe.dialog.log")) `
        -TimeoutSec 20
    if ($probeRun.ExitCode -ne 0 -and $probeRun.ExitCode -ne 1) { $probeFail++ }
    if ($hostRun.DialogCaught -or $probeRun.DialogCaught -or $hostRun.TimedOut -or $probeRun.TimedOut) { $crash++ }
    $probeOk = ($probeRun.ExitCode -eq 0)

    if ($hostOk -eq $probeOk) {
        $ok++
    } else {
        Write-Host "DIFF $rel  host_parse_ok=$hostOk probe_ok=$probeOk"
        $diff++
    }
}

Write-Host ""
Write-Host "── parse conformance summary ──"
Write-Host "  scope:      $scanRoot"
Write-Host "  files:      $($files.Count)"
Write-Host "  OK:         $ok"
Write-Host "  DIFF:       $diff"
Write-Host "  probe_weird:$probeFail (unexpected exit code not 0/1)"
Write-Host "  crash/timeout:$crash"
Write-Host "  artifacts: $logDir"
Write-Host ""
Write-Host "Note: DIFF often means the bootstrap parser accepts a smaller grammar than host,"
Write-Host "      or import/std resolution differs (host stops after parse; probe has no imports)."

if ($diff -gt 0 -or $probeFail -gt 0 -or $crash -gt 0) {
    exit 1
}
exit 0

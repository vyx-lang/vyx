# Compare bootstrap `probe_lex --dump-tokens` to host `vyxc --dump-tokens` on
# the same file path. Line format: `path:line:col kind 'lexeme'` (see
# `src/main.cpp` and `bootstrap_compiler/src/lexer.vyx` `dump_tokens_host_lines`).
#
# Usage:
#   pwsh tests/bootstrap/lexer_conformance/run.ps1
#     → all loose `tests/cases/**/*.vyx` under the repo root (default).
#   pwsh .../run.ps1 -Path "e:\repo\tests\cases"
#   pwsh .../run.ps1 -Samples
#     → only `samples/bootstrap/*.vyx` (small sanity check).
#
# Requirements: build_probes.ps1, host vyxc (PATH or $env:VYXC).

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
$probe = Join-Path $outDir "probe_lex.exe"
$samplesDir = Join-Path $ws "samples/bootstrap"
$defaultTests = Join-Path $testsRoot "cases"

$vyxc = $env:VYXC
if (-not $vyxc) {
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

$tmp = Join-Path $outDir "lexcmp"
if (-not (Test-Path $tmp)) { New-Item -ItemType Directory -Path $tmp | Out-Null }

function Clear-HostCacheForFile {
    param([System.IO.FileInfo]$File)
    $parent = Split-Path $File.FullName
    $stem = $File.BaseName
    foreach ($p in @(
            (Join-Path $parent (Join-Path ".cache" ($stem + ".vyx.cache"))),
            (Join-Path $parent (Join-Path ".cache" ($stem + ".ll"))),
            (Join-Path $parent (Join-Path ".cache" ($stem + ".obj"))),
            (Join-Path $parent (Join-Path ".cache" ($stem + ".exe"))),
            (Join-Path $parent (Join-Path "target" ($stem + ".exe")))
        )) {
        if (Test-Path $p) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue }
    }
}

$ok = 0
$diff = 0
$hostFail = 0
$probeFail = 0
$crash = 0
$n = 0
foreach ($f in $files) {
    $n++
    if (($n % 50) -eq 1) {
        Write-Host "[lex] $n / $($files.Count) ..."
    }

    $full = $f.FullName
    $rel = $full.Substring($scanRoot.Length).TrimStart("\", "/")
    $safe = ($rel -replace '[\\/:*?"<>|]', '_')
    if ([string]::IsNullOrWhiteSpace($safe)) { $safe = $f.Name }
    $hostOut = Join-Path $tmp ($safe + ".host.txt")
    $bootOut = Join-Path $tmp ($safe + ".boot.txt")

    Clear-HostCacheForFile -File $f
    $hostRun = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList @("--dump-tokens", "--src=file", $full) `
        -StdoutLog (Join-Path $tmp ($safe + ".host.raw.out.log")) `
        -StderrLog (Join-Path $tmp ($safe + ".host.raw.err.log")) `
        -DialogLog (Join-Path $tmp ($safe + ".host.dialog.log")) `
        -TimeoutSec 20
    $hostLines = @()
    foreach ($lp in @($hostRun.StdoutLog, $hostRun.StderrLog)) {
        if (Test-Path $lp) { $hostLines += Get-Content $lp -ErrorAction SilentlyContinue }
    }
    $hostLines |
        Where-Object { $_ -match ':\d+:\d+ \d+ ''' } |
        Set-Content -Encoding utf8 $hostOut
    if ($hostRun.ExitCode -ne 0) {
        Write-Host "HOST_FAIL $rel (exit $($hostRun.ExitCode))"
        $hostFail++
        if ($hostRun.DialogCaught -or $hostRun.TimedOut) { $crash++ }
        Clear-HostCacheForFile -File $f
        continue
    }
    Clear-HostCacheForFile -File $f

    $probeRun = Invoke-VyxProcess -FilePath $probe `
        -ArgumentList @("--dump-tokens", $full) `
        -StdoutLog (Join-Path $tmp ($safe + ".probe.raw.out.log")) `
        -StderrLog (Join-Path $tmp ($safe + ".probe.raw.err.log")) `
        -DialogLog (Join-Path $tmp ($safe + ".probe.dialog.log")) `
        -TimeoutSec 20
    if (Test-Path $probeRun.StdoutLog) {
        Get-Content $probeRun.StdoutLog -ErrorAction SilentlyContinue | Set-Content -Encoding utf8 $bootOut
    } else {
        "" | Set-Content -Encoding utf8 $bootOut
    }
    if ($probeRun.ExitCode -ne 0) {
        Write-Host "PROBE_FAIL $rel (exit $($probeRun.ExitCode))"
        $probeFail++
        if ($probeRun.DialogCaught -or $probeRun.TimedOut) { $crash++ }
        continue
    }

    $h = Get-Content -Raw $hostOut
    $b = Get-Content -Raw $bootOut
    if ($h -ne $b) {
        Write-Host "DIFF $rel"
        $diff++
    } else {
        $ok++
    }
}

Write-Host ""
Write-Host "── lexer conformance summary ──"
Write-Host "  scope:     $scanRoot"
Write-Host "  files:     $($files.Count)"
Write-Host "  OK:        $ok"
Write-Host "  DIFF:      $diff"
Write-Host "  host_fail: $hostFail"
Write-Host "  probe_fail:$probeFail"
Write-Host "  crash/timeout:$crash"
Write-Host "  artifacts: $tmp"

# Lexer-only gate: host may exit 1 on negative tests before a token dump exists;
# those files are skipped (not a bootstrap/host lexeme mismatch).
$failed = ($diff -gt 0) -or ($probeFail -gt 0) -or ($crash -gt 0)
if ($failed) {
    exit 1
}
exit 0

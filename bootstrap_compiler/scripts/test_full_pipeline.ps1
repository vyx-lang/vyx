param(
    [string]$Path = "",
    [switch]$Samples,
    [switch]$MirCodegen,
    [int]$TimeoutSec = 30
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$ws = Split-Path -Parent $root
$outDir = Join-Path $root "out"
$helper = Join-Path $PSScriptRoot "VyxTestProcess.ps1"
. $helper

$probeLex = Join-Path $outDir "probe_lex.exe"
$probeParse = Join-Path $outDir "probe_parse.exe"
$probeSema = Join-Path $outDir "probe_sema.exe"
$probeEmit = Join-Path $outDir "probe_emit.exe"

foreach ($p in @($probeLex, $probeParse, $probeSema, $probeEmit)) {
    if (-not (Test-Path $p)) {
        Write-Error "Missing $p - run bootstrap_compiler/scripts/build_probes.ps1 first"
        exit 2
    }
}

$vyxc = $env:VYXC
if (-not $vyxc) {
    foreach ($c in @(
            (Join-Path $outDir "vyxc.exe"),
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
if (-not $vyxc) {
    Write-Error "No host vyxc (set VYXC or place vyxc.exe under build/)"
    exit 2
}

if ($Samples) {
    $scanRoot = Join-Path $ws "samples/bootstrap"
    $files = Get-ChildItem -Path $scanRoot -Filter "*.vyx" -File | Sort-Object Name
} elseif ($Path) {
    $scanRoot = (Resolve-Path $Path).Path
    $files = Get-ChildItem -Path $scanRoot -Recurse -Filter "*.vyx" -File | Sort-Object FullName
} else {
    $scanRoot = Join-Path $ws "tests"
    $files = Get-ChildItem -Path $scanRoot -Recurse -Filter "*.vyx" -File | Sort-Object FullName
}

if ($files.Count -eq 0) {
    Write-Error "No .vyx files under $scanRoot"
    exit 2
}

$env:Path = $outDir + ";" + (Join-Path $ws "build_vyxcg/vyx_codegen") + ";" + $env:Path
$logDir = Join-Path $outDir "full_pipeline"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }

function Test-ExcludedRelative {
    param([string]$Rel)
    $r = $Rel.Replace('/', '\')
    if ($r -like '.cache\*') { return $true }
    if ($r -like 'tests\.cache\*') { return $true }
    if ($r -match '\\target\\') { return $true }
    return $false
}

function Test-ProjectBuildOnly {
    param([string]$FilePath)
    foreach ($line in [System.IO.File]::ReadLines($FilePath)) {
        if ($line.IndexOf("PROJECT-BUILD-ONLY", [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }
    return $false
}

function Safe-Name {
    param([string]$Rel, [string]$Fallback)
    $s = ($Rel -replace '[\\/:*?"<>|]', '_')
    if ([string]::IsNullOrWhiteSpace($s)) { return $Fallback }
    return $s
}

function Read-AllLines {
    param([string[]]$Paths)
    $lines = @()
    foreach ($p in $Paths) {
        if (Test-Path $p) { $lines += Get-Content $p -ErrorAction SilentlyContinue }
    }
    return $lines
}

function Test-IrShape {
    param([string]$LlPath)
    if (-not (Test-Path $LlPath)) { return $false }
    $txt = Get-Content $LlPath -Raw -ErrorAction SilentlyContinue
    if ($null -eq $txt) { $txt = "" }
    return ($txt.Contains("define") -and $txt.Contains("@main"))
}

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

$summary = [ordered]@{
    lex_ok = 0; lex_diff = 0; lex_host_fail = 0; lex_probe_fail = 0
    parse_ok = 0; parse_diff = 0; parse_weird = 0
    sema_ok = 0; sema_diff = 0; sema_weird = 0
    codegen_ok = 0; codegen_diff = 0; codegen_host_pass = 0; codegen_probe_pass = 0
    crash_timeout = 0
}

$n = 0
$skipped = 0
foreach ($f in $files) {
    $full = $f.FullName
    $rel = $full.Substring($scanRoot.Length).TrimStart("\", "/")
    if ((Test-ExcludedRelative $rel) -or (Test-ProjectBuildOnly $full)) {
        $skipped++
        continue
    }
    $n++
    if (($n % 25) -eq 1) {
        Write-Host "[full-pipeline] $n / $($files.Count) ..."
    }

    $safe = Safe-Name -Rel $rel -Fallback $f.Name

    # Stage 1: lex
    Clear-HostCacheForFile -File $f
    $hostLex = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList @("--dump-tokens", "--src=file", $full) `
        -StdoutLog (Join-Path $logDir ($safe + ".lex.host.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".lex.host.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".lex.host.dialog.log")) `
        -TimeoutSec $TimeoutSec
    $probeLexRun = Invoke-VyxProcess -FilePath $probeLex `
        -ArgumentList @("--dump-tokens", $full) `
        -StdoutLog (Join-Path $logDir ($safe + ".lex.probe.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".lex.probe.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".lex.probe.dialog.log")) `
        -TimeoutSec $TimeoutSec
    if ($hostLex.DialogCaught -or $hostLex.TimedOut -or $probeLexRun.DialogCaught -or $probeLexRun.TimedOut) { $summary.crash_timeout++ }
    if ($hostLex.ExitCode -ne 0) {
        $summary.lex_host_fail++
    } elseif ($probeLexRun.ExitCode -ne 0) {
        $summary.lex_probe_fail++
        Write-Host "LEX_PROBE_FAIL $rel exit=$($probeLexRun.ExitCode)"
    } else {
        $hLines = Read-AllLines -Paths @($hostLex.StdoutLog, $hostLex.StderrLog) | Where-Object { $_ -match ':\d+:\d+ \d+ ''' }
        $bLines = Read-AllLines -Paths @($probeLexRun.StdoutLog)
        if (($hLines -join "`n") -eq ($bLines -join "`n")) {
            $summary.lex_ok++
        } else {
            $summary.lex_diff++
            Write-Host "LEX_DIFF $rel"
        }
    }

    # Stage 2: lex -> parse
    Clear-HostCacheForFile -File $f
    $hostParse = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList @("--stop-after-parse", "--src=file", $full) `
        -StdoutLog (Join-Path $logDir ($safe + ".parse.host.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".parse.host.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".parse.host.dialog.log")) `
        -TimeoutSec $TimeoutSec
    $probeParseRun = Invoke-VyxProcess -FilePath $probeParse `
        -ArgumentList @($full) `
        -StdoutLog (Join-Path $logDir ($safe + ".parse.probe.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".parse.probe.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".parse.probe.dialog.log")) `
        -TimeoutSec $TimeoutSec
    if ($hostParse.DialogCaught -or $hostParse.TimedOut -or $probeParseRun.DialogCaught -or $probeParseRun.TimedOut) { $summary.crash_timeout++ }
    if ($probeParseRun.ExitCode -ne 0 -and $probeParseRun.ExitCode -ne 1) { $summary.parse_weird++ }
    if (($hostParse.ExitCode -eq 0) -eq ($probeParseRun.ExitCode -eq 0)) {
        $summary.parse_ok++
    } else {
        $summary.parse_diff++
        Write-Host "PARSE_DIFF $rel host=$($hostParse.ExitCode) probe=$($probeParseRun.ExitCode)"
    }

    # Stage 3: lex -> parse -> sema
    Clear-HostCacheForFile -File $f
    $hostSema = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList @("--stop-after-sema", "--src=file", $full) `
        -StdoutLog (Join-Path $logDir ($safe + ".sema.host.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".sema.host.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".sema.host.dialog.log")) `
        -TimeoutSec $TimeoutSec
    $probeSemaRun = Invoke-VyxProcess -FilePath $probeSema `
        -ArgumentList @($full) `
        -StdoutLog (Join-Path $logDir ($safe + ".sema.probe.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".sema.probe.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".sema.probe.dialog.log")) `
        -TimeoutSec $TimeoutSec
    if ($hostSema.DialogCaught -or $hostSema.TimedOut -or $probeSemaRun.DialogCaught -or $probeSemaRun.TimedOut) { $summary.crash_timeout++ }
    if ($probeSemaRun.ExitCode -ne 0 -and $probeSemaRun.ExitCode -ne 1) { $summary.sema_weird++ }
    if (($hostSema.ExitCode -eq 0) -eq ($probeSemaRun.ExitCode -eq 0)) {
        $summary.sema_ok++
    } else {
        $summary.sema_diff++
        Write-Host "SEMA_DIFF $rel host=$($hostSema.ExitCode) probe=$($probeSemaRun.ExitCode)"
    }

    # Stage 4: lex -> parse -> sema -> codegen
    $hostLl = Join-Path $logDir ($safe + ".host.ll")
    $probeLl = Join-Path $logDir ($safe + ".probe.ll")
    Remove-Item -LiteralPath $hostLl,$probeLl -Force -ErrorAction SilentlyContinue
    Clear-HostCacheForFile -File $f
    $hostEmit = Invoke-VyxProcess -FilePath $vyxc `
        -ArgumentList @("--emit=ir", "--src=file", $full, "-o", $hostLl) `
        -StdoutLog (Join-Path $logDir ($safe + ".emit.host.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".emit.host.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".emit.host.dialog.log")) `
        -TimeoutSec $TimeoutSec
    $probeEmitRun = Invoke-VyxProcess -FilePath $probeEmit `
        -ArgumentList @($full, $probeLl) `
        -StdoutLog (Join-Path $logDir ($safe + ".emit.probe.out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".emit.probe.err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".emit.probe.dialog.log")) `
        -TimeoutSec $TimeoutSec
    if ($hostEmit.DialogCaught -or $hostEmit.TimedOut -or $probeEmitRun.DialogCaught -or $probeEmitRun.TimedOut) { $summary.crash_timeout++ }
    $hostEmitOk = ($hostEmit.ExitCode -eq 0)
    $probeEmitOk = ($probeEmitRun.ExitCode -eq 0)
    if ($hostEmitOk) { $summary.codegen_host_pass++ }
    if ($probeEmitOk) { $summary.codegen_probe_pass++ }
    $hostShape = Test-IrShape -LlPath $hostLl
    $probeShape = Test-IrShape -LlPath $probeLl
    if (($hostEmitOk -eq $probeEmitOk) -and ((-not $hostEmitOk) -or ($hostShape -eq $probeShape))) {
        $summary.codegen_ok++
    } else {
        $summary.codegen_diff++
        Write-Host "CODEGEN_DIFF $rel host=$($hostEmit.ExitCode) hshape=$hostShape probe=$($probeEmitRun.ExitCode) pshape=$probeShape"
    }
}

Write-Host ""
Write-Host "========== FULL PIPELINE SUMMARY =========="
Write-Host "scope:              $scanRoot"
Write-Host "files:              $($files.Count)"
Write-Host "tested:             $n"
Write-Host "skipped:            $skipped"
Write-Host "lex:                OK=$($summary.lex_ok) DIFF=$($summary.lex_diff) host_fail=$($summary.lex_host_fail) probe_fail=$($summary.lex_probe_fail)"
Write-Host "lex->parse:         OK=$($summary.parse_ok) DIFF=$($summary.parse_diff) weird=$($summary.parse_weird)"
Write-Host "lex->parse->sema:   OK=$($summary.sema_ok) DIFF=$($summary.sema_diff) weird=$($summary.sema_weird)"
Write-Host "lex->parse->sema->codegen: OK=$($summary.codegen_ok) DIFF=$($summary.codegen_diff) host_pass=$($summary.codegen_host_pass) probe_pass=$($summary.codegen_probe_pass)"
Write-Host "crash/timeout caught:$($summary.crash_timeout)"
Write-Host "artifacts:          $logDir"

$failed = ($summary.lex_diff -gt 0) -or ($summary.lex_probe_fail -gt 0) -or
          ($summary.parse_diff -gt 0) -or ($summary.parse_weird -gt 0) -or
          ($summary.sema_diff -gt 0) -or ($summary.sema_weird -gt 0) -or
          ($summary.codegen_diff -gt 0) -or ($summary.crash_timeout -gt 0)
if ($failed) { exit 1 }

if ($MirCodegen) {
    $mirScript = Join-Path $PSScriptRoot "test_mir_codegen_backend.ps1"
    if (-not (Test-Path $mirScript)) {
        Write-Error "Missing MIR codegen backend test script: $mirScript"
        exit 2
    }
    $mirCompiler = $vyxc
    if ($mirCompiler -eq "vyxc") {
        $cmd = Get-Command vyxc -ErrorAction SilentlyContinue
        if ($cmd) { $mirCompiler = $cmd.Source }
    }
    Write-Host ""
    Write-Host "========== MIR CODEGEN BACKEND =========="
    & powershell -NoProfile -ExecutionPolicy Bypass -File $mirScript `
        -Compiler $mirCompiler `
        -RuntimeDir $outDir `
        -RuntimeLib "vyx_rt" `
        -TimeoutSec $TimeoutSec
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
exit 0

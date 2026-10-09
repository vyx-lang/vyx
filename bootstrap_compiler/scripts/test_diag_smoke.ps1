$ErrorActionPreference = "Stop"

# Smoke harness — verify the diagnostics engine emits stable Vyx codes on
# `tests/diag_smoke.vyx` with proper `<file>:<line>:<col>:` prefixes.
#
# The sample under `samples/bootstrap/diag_smoke.vyx` is a
# CLEAN-COMPILING placeholder so the fixpoint sample sweep does not
# regress; the actual error-triggering source lives at
# `tests/bootstrap/diag_smoke.vyx`. This script exercises the
# latter through `boot_a --stop-after-sema` and grep-asserts each of
# the five expected codes is present, with a file path and line:col
# pair.

$root = Split-Path -Parent $PSScriptRoot
$ws   = Split-Path -Parent $root
. (Join-Path $PSScriptRoot "VyxTestProcess.ps1")

$logDir = Join-Path $root "out/diag_smoke"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }

$bootA = Join-Path $root "out\boot.exe"
if (-not (Test-Path $bootA)) {
    $bootA = Join-Path $root "out\vyxc.exe"
}
if (-not (Test-Path $bootA)) {
    $bootA = Join-Path $root "boot_a.exe"
}
if (-not (Test-Path $bootA)) {
    Write-Host "[diag-smoke] FAIL compiler missing — run build first"
    exit 1
}

$rtCandidates = @(
    (Join-Path $ws "bootstrap_compiler/out"),
    (Join-Path $ws "build_yolo_vyxcg/vyx_rt"),
    (Join-Path $ws "build_vyxcg/vyx_rt"),
    (Join-Path $ws "build/vyx_rt"),
    (Join-Path $ws "cmake-build-debug/vyx_codegen")
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

$src = Join-Path $ws "tests/bootstrap/diag_smoke.vyx"
if (-not (Test-Path $src)) {
    Write-Host "[diag-smoke] FAIL tests/bootstrap/diag_smoke.vyx missing"
    exit 2
}

$outLog = Join-Path $logDir "diag_smoke.out.log"
$errLog = Join-Path $logDir "diag_smoke.err.log"
$dlgLog = Join-Path $logDir "diag_smoke.dialog.log"

$run = Invoke-VyxProcess -FilePath $bootA `
    -ArgumentList @("--stop-after-sema", "--src=file", $src) `
    -WorkingDirectory $ws `
    -StdoutLog $outLog `
    -StderrLog $errLog `
    -DialogLog $dlgLog `
    -TimeoutSec 30

# We EXPECT a non-zero exit (sema rejected the input); the actual
# assertion is on the diagnostic text in stdout.
$captured = ""
if (Test-Path $outLog) { $captured = Get-Content $outLog -Raw }
if ([string]::IsNullOrEmpty($captured) -and (Test-Path $errLog)) {
    $captured = Get-Content $errLog -Raw
}

if ([string]::IsNullOrEmpty($captured)) {
    Write-Host "[diag-smoke] FAIL no output captured (exit=$($run.ExitCode))"
    exit 3
}

$expected = @(
    'tests[\\/]bootstrap[\\/]diag_smoke\.vyx:\d+:\d+: (error|错误): E2100:',
    'tests[\\/]bootstrap[\\/]diag_smoke\.vyx:\d+:\d+: (error|错误): E1000:',
    'tests[\\/]bootstrap[\\/]diag_smoke\.vyx:\d+:\d+: (error|错误): E2000:',
    'tests[\\/]bootstrap[\\/]diag_smoke\.vyx:\d+:\d+: (error|错误): E1200:'
)

$missing = @()
foreach ($rx in $expected) {
    if ($captured -notmatch $rx) {
        $missing += $rx
    }
}

if ($missing.Count -gt 0) {
    Write-Host "[diag-smoke] FAIL missing expected codes:"
    foreach ($m in $missing) { Write-Host "  - $m" }
    Write-Host "----- captured output -----"
    Write-Host $captured
    exit 4
}

function Invoke-DiagCase {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Source,
        [Parameter(Mandatory=$true)][string[]]$Expected,
        [string[]]$Forbidden = @()
    )
    $caseDir = Join-Path $logDir "cases"
    if (-not (Test-Path $caseDir)) { New-Item -ItemType Directory -Force -Path $caseDir | Out-Null }
    $safe = ($Name -replace '[^A-Za-z0-9_.-]', '_')
    $caseSrc = Join-Path $caseDir ($safe + ".vyx")
    Set-Content -LiteralPath $caseSrc -Encoding ascii -Value $Source
    $caseOut = Join-Path $caseDir ($safe + ".out.log")
    $caseErr = Join-Path $caseDir ($safe + ".err.log")
    $caseDlg = Join-Path $caseDir ($safe + ".dialog.log")
    $caseRun = Invoke-VyxProcess -FilePath $bootA `
        -ArgumentList @("--stop-after-parse", "--src=file", $caseSrc) `
        -WorkingDirectory $ws `
        -StdoutLog $caseOut `
        -StderrLog $caseErr `
        -DialogLog $caseDlg `
        -TimeoutSec 30
    $caseText = ""
    if (Test-Path $caseOut) { $caseText += Get-Content $caseOut -Raw }
    if (Test-Path $caseErr) { $caseText += Get-Content $caseErr -Raw }
    if ($caseRun.DialogCaught -or $caseRun.TimedOut) {
        Write-Host "[diag-smoke] FAIL $Name timeout/dialog"
        Write-Host $caseText
        exit 5
    }
    foreach ($needle in $Expected) {
        if (-not $caseText.Contains($needle)) {
            Write-Host "[diag-smoke] FAIL $Name missing '$needle'"
            Write-Host $caseText
            exit 6
        }
    }
    foreach ($bad in $Forbidden) {
        if ($caseText.Contains($bad)) {
            Write-Host "[diag-smoke] FAIL $Name contained forbidden '$bad'"
            Write-Host $caseText
            exit 7
        }
    }
}

Invoke-DiagCase -Name "lexer_invalid_character" `
    -Expected @("E0104", "unexpected character") `
    -Forbidden @("lexer error", "parse error", "[boot_a]", "[codegen]") `
    -Source @'
module tests.diag_lex;
fn main() -> i32 {
    `
    return 0;
}
'@

Invoke-DiagCase -Name "parser_missing_expression" `
    -Expected @("E000", "got") `
    -Forbidden @("lexer error", "parse error", "[boot_a]", "[codegen]") `
    -Source @'
module tests.diag_parse;
fn main() -> i32 {
    let x: i32 = ;
    return 0;
}
'@

# MIR diagnostics must never bypass bootstrap.diag with raw backend text or
# lose their source anchor to 0:0. Keep the audit broad across every MIR phase.
# The dynamic probe also guarantees that a source-level call mismatch is
# rejected by overload resolution before malformed MIR reaches a backend.
$mirPhaseSources = @(Get-ChildItem -LiteralPath (Join-Path $root "src/mir") -File -Recurse -Filter '*.vyx')
if ($mirPhaseSources.Count -eq 0) { throw 'MIR diagnostic audit found no source files' }
$mirPhaseSources += Get-Item -LiteralPath (Join-Path $root 'src/codegen/cpp/mir_cpp_lower.vyx')
foreach ($source in $mirPhaseSources) {
    $name = $source.FullName
    $text = [System.IO.File]::ReadAllText($name)
    if ($text -match 'diag_emit(_at)?\s*\(') {
        Write-Host "[diag-smoke] FAIL $name bypasses unified MIR diagnostic entry"
        exit 8
    }
    if ($text -match 'print\s*\(\s*"(?:mir2cpp:|mir-builder:|codegen:)') {
        Write-Host "[diag-smoke] FAIL $name still emits a raw MIR error"
        exit 9
    }
}

$mirCaseDir = Join-Path $logDir "mir_project"
$mirCaseSrcDir = Join-Path $mirCaseDir "src"
if (-not (Test-Path $mirCaseSrcDir)) {
    New-Item -ItemType Directory -Force -Path $mirCaseSrcDir | Out-Null
}
Set-Content -LiteralPath (Join-Path $mirCaseDir "Vyx.toml") -Encoding ascii -Value @'
[package]
name = "mir_diag_probe"
version = "0.1.0"
entry = "src/main.vyx"

[build]
output_dir = "out"
cache_dir = ".cache"

[target.mir_diag_probe]
type = "executable"
entry = "src/main.vyx"
'@
Set-Content -LiteralPath (Join-Path $mirCaseSrcDir "main.vyx") -Encoding ascii -Value @'
fn func(a:i32) -> i32 {
    return a + 1;
}

fn main() -> i32 {
    func();
    return 0;
}
'@

$mirOut = Join-Path $mirCaseDir "mir.out.log"
$mirErr = Join-Path $mirCaseDir "mir.err.log"
$mirDlg = Join-Path $mirCaseDir "mir.dialog.log"
$mirRun = Invoke-VyxProcess -FilePath $bootA `
    -ArgumentList @("--src=project", $mirCaseDir, "--run=aot") `
    -WorkingDirectory $ws `
    -StdoutLog $mirOut `
    -StderrLog $mirErr `
    -DialogLog $mirDlg `
    -TimeoutSec 30
$mirText = ""
if (Test-Path $mirOut) { $mirText += Get-Content $mirOut -Raw }
if (Test-Path $mirErr) { $mirText += Get-Content $mirErr -Raw }
if ($mirRun.DialogCaught -or $mirRun.TimedOut -or $mirRun.ExitCode -eq 0) {
    Write-Host "[diag-smoke] FAIL MIR diagnostic process state"
    Write-Host $mirText
    exit 10
}
$mirExpected = @(
    "src[\\/]main\.vyx:6:\d+: (error|错误): E2200: no matching overload for call to 'func': 0 arguments provided; the only candidate requires 1 argument",
    '--> .*src[\\/]main\.vyx:6:\d+',
    '6 \|\s+func\(\);',
    '\|\s+\^',
    'N2200: candidate defined here: fn func\(a: i32\) -> i32'
)
foreach ($rx in $mirExpected) {
    if ($mirText -notmatch $rx) {
        Write-Host "[diag-smoke] FAIL MIR diagnostic missing $rx"
        Write-Host $mirText
        exit 11
    }
}
if (($mirText -match ':0:0:') -or
    ($mirText -match 'mir2cpp:') -or
    ($mirText -match 'mir-builder:') -or
    ($mirText -match 'cannot build direct call') -or
    ($mirText -match 'runtime=build_call') -or
    ($mirText -match 'value #\d+ target #\d+')) {
    Write-Host "[diag-smoke] FAIL MIR diagnostic retained raw/zero location output"
    Write-Host $mirText
    exit 12
}

Write-Host "[diag-smoke] PASS — standardized codes emitted with file:line:col"
$captured.TrimEnd().Split([Environment]::NewLine) | ForEach-Object { Write-Host "  $_" }
exit 0

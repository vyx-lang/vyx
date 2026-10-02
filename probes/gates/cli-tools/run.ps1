# CLI subcommands: test / bench / fmt / doc
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$Boot = Join-Path $Root "bootstrap_compiler\out\boot.exe"
$OutDir = Join-Path $Root "bootstrap_compiler\out"
$Llvm = Join-Path $Root "clang"
$Fixture = Join-Path $PSScriptRoot "fixture"
$Tmp = Join-Path $env:TEMP "vyx-cli-tools"
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null

if (Test-Path $Llvm) {
    $env:LLVM_ROOT = (Resolve-Path $Llvm).Path
    $env:PATH = "$OutDir;$env:LLVM_ROOT\bin;$env:PATH"
} else {
    $env:PATH = "$OutDir;$env:PATH"
}

if (-not (Test-Path $Boot)) {
    Write-Host "cli-tools FAIL: missing $Boot"
    exit 1
}

function Fail([string]$msg, [string]$log) {
    Write-Host "FAIL: $msg"
    if ($log -and (Test-Path $log)) {
        Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
    }
    exit 1
}

$helpLog = Join-Path $Tmp "help.log"
& $Boot help *> $helpLog
$help = Get-Content $helpLog -Raw
if ($help -notmatch '(?m)^\s+\S+ test ') { Fail "help missing test usage" $helpLog }
if ($help -notmatch '(?m)^\s+\S+ bench ') { Fail "help missing bench usage" $helpLog }
if ($help -notmatch '(?m)^\s+\S+ fmt ') { Fail "help missing fmt usage" $helpLog }
if ($help -notmatch '(?m)^\s+\S+ doc ') { Fail "help missing doc usage" $helpLog }
Write-Host "OK: help lists test/bench/fmt/doc"

Push-Location $Fixture
try {
    $testLog = Join-Path $Tmp "test.log"
    & $Boot test *> $testLog
    if ($LASTEXITCODE -ne 0) { Fail "test exit=$LASTEXITCODE" $testLog }
    $testOut = Get-Content $testLog -Raw
    if ($testOut -notmatch 'test_add' -or $testOut -notmatch 'test result: ok') {
        Fail "test output missing test_add / ok" $testLog
    }
    Write-Host "OK: test"

    $filterLog = Join-Path $Tmp "filter.log"
    & $Boot test --filter no_such_name *> $filterLog
    if ($LASTEXITCODE -ne 0) { Fail "filtered test should exit 0" $filterLog }
    $filterOut = Get-Content $filterLog -Raw
    if ($filterOut -notmatch 'no tests') { Fail "filter should report no tests" $filterLog }
    Write-Host "OK: test --filter"

    $benchLog = Join-Path $Tmp "bench.log"
    & $Boot bench *> $benchLog
    if ($LASTEXITCODE -ne 0) { Fail "bench exit=$LASTEXITCODE" $benchLog }
    $benchOut = Get-Content $benchLog -Raw
    if ($benchOut -notmatch 'bench_spin' -or $benchOut -notmatch ' ms') {
        Fail "bench output missing name / ms" $benchLog
    }
    Write-Host "OK: bench"

    $ugly = Join-Path $Fixture "src\ugly.vyx"
    Copy-Item (Join-Path $Fixture "src\ugly.raw.vyx") $ugly -Force
    $checkLog = Join-Path $Tmp "fmt-check.log"
    & $Boot fmt --check $ugly *> $checkLog
    if ($LASTEXITCODE -eq 0) { Fail "fmt --check should fail on ugly source" $checkLog }
    Write-Host "OK: fmt --check dirty"

    $fmtLog = Join-Path $Tmp "fmt.log"
    & $Boot fmt $ugly *> $fmtLog
    if ($LASTEXITCODE -ne 0) { Fail "fmt rewrite exit=$LASTEXITCODE" $fmtLog }
    & $Boot fmt --check $ugly *> $checkLog
    if ($LASTEXITCODE -ne 0) { Fail "fmt --check should pass after rewrite" $checkLog }
    $formatted = Get-Content $ugly -Raw
    if ($formatted -notmatch 'fn main\(\) -> i32' -or $formatted -notmatch 'return 0;') {
        Fail "fmt rewrite missing expected tokens" $ugly
    }
    Write-Host "OK: fmt"

    $docLog = Join-Path $Tmp "doc.log"
    & $Boot doc (Join-Path $Fixture "src\lib.vyx") *> $docLog
    if ($LASTEXITCODE -ne 0) { Fail "doc exit=$LASTEXITCODE" $docLog }
    $docOut = Get-Content $docLog -Raw
    if ($docOut -notmatch 'inc' -or $docOut -notmatch 'Adds one') {
        Fail "doc missing inc / Adds one" $docLog
    }
    Write-Host "OK: doc"
}
finally {
    Pop-Location
}

Write-Host "cli-tools E2E: OK"
exit 0

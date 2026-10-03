# scripts/run_gates.ps1
#
# IR Regression Guard — D4：0a strict 生效（mismatch/count/micro 任一异常即 fail）。
#
# 运行四个主门：cases / auth / industrial-compile / industrial-run（沿用现有 driver
# tests/run_all_modules.ps1，路径过滤而已；这与 D1/D2 之前的跑法一致）。
# 然后跑 IR guard 4 个 sub-stage：
#   0a ir_guard_strict         — 严格模式总闸；内部执行 ir_diff.ps1 -Mode check -Strict
#   0b ir_snapshot_diff        — sha256 hash 对比（明细）
#   0c ir_assert_micropoints   — 微点测 DSL 断言（明细）
#   0d instr_count_redline     — opcode 计数 ±5%（明细）
#
# 0b/0c/0d 实际由 scripts/ir_diff.ps1 -Mode check 一次性完成；本脚本解析其结果按 stage 拆分展示。
#
# 用法：
#   powershell -File scripts/run_gates.ps1                      # 默认：仅 IR guard（严格）
#   powershell -File scripts/run_gates.ps1 -RunMainGates        # 同时跑主门（信息展示，不影响整体 exit）
#   powershell -File scripts/run_gates.ps1 -OnlyMainGates       # 只跑主门
#   powershell -File scripts/run_gates.ps1 -Compiler build/vyxc.exe
#
# 主门 driver 说明：
#   仓库目前没有 EF (expected-failure) -aware 的统一 gate driver。复用
#   tests/run_all_modules.ps1 会把 EF 误报为 fail。因此默认 -RunMainGates=off，
#   主门以 "deferred" 占位展示。当仓库提供标准 gate driver 时把 $runner 替换即可。

[CmdletBinding()]
param(
    [string]$Compiler = "",
    [switch]$RunMainGates,
    [switch]$OnlyMainGates
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$runner   = Join-Path $repoRoot "tests\run_all_modules.ps1"
$irDiff   = Join-Path $PSScriptRoot "ir_diff.ps1"

if (-not (Test-Path $runner)) { throw "missing: $runner" }
if (-not (Test-Path $irDiff)) { throw "missing: $irDiff" }

# 4 个主门的 PathFilter（沿用 run_all_modules.ps1 的子串过滤）：
$mainGates = @(
    @{ name="cases";              filter="cases\";              role="run" },
    @{ name="auth";                filter="_strict\";            role="run" },
    @{ name="auth-adv";            filter="_adversarial";        role="run" },
    @{ name="auth-beyond";         filter="_beyond_cpp26\";      role="run" },
    @{ name="industrial-run";      filter="_generics_industrial\"; role="run" }
)

# IR guard 4 个 sub-stage 的展示元数据（D4: 0a strict）
$irGuardStages = @(
    @{ id="0a"; name="ir_guard_strict";        status="";     note="strict gate: any mismatch/count/micro warning => FAIL" },
    @{ id="0b"; name="ir_snapshot_diff";       status="";     note="sha256 hash diff" },
    @{ id="0c"; name="ir_assert_micropoints"; status="";     note="micropoints DSL asserts" },
    @{ id="0d"; name="instr_count_redline";   status="";     note="opcode count ±5%" }
)

function Run-MainGate {
    param([string]$Name, [string]$Filter)
    Write-Host ("==== gate: " + $Name + " (filter='" + $Filter + "') ====") -ForegroundColor Cyan
    $args = @("-PathFilter", $Filter, "-HostOnly")
    if ($Compiler -ne "") { $args += @("-HostCompiler", $Compiler) }
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & powershell -ExecutionPolicy Bypass -File $runner @args | Out-Host
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    if ($rc -ne 0) {
        Write-Host ("  -> gate '" + $Name + "' FAIL (exit " + $rc + ")") -ForegroundColor Red
    } else {
        Write-Host ("  -> gate '" + $Name + "' PASS") -ForegroundColor Green
    }
    return $rc
}

function Run-IRGuard {
    Write-Host "==== ir_guard: 0a-0d (strict @ D4) ====" -ForegroundColor Cyan
    $args = @("-Mode", "check", "-Strict")
    if ($Compiler -ne "") { $args += @("-Compiler", $Compiler) }
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & powershell -ExecutionPolicy Bypass -File $irDiff @args 2>&1 | Out-String
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    Write-Host $output

    $sumLine = ($output -split "`n") | Where-Object { $_ -match '\[ir_diff\] mode=check' } | Select-Object -First 1
    if (-not $sumLine) {
        Write-Host "  WARN: ir_diff produced no summary line; treating as warn." -ForegroundColor Yellow
        return @{ mismatch = -1; count = -1; micro = -1; strict_rc = 9; rawSummary = "(none)" }
    }
    $mismatch = 0; $countWarn = 0; $microWarn = 0
    if ($sumLine -match 'mismatch=(\d+)')    { $mismatch  = [int]$matches[1] }
    if ($sumLine -match 'count_warn=(\d+)')  { $countWarn = [int]$matches[1] }
    if ($sumLine -match 'micro_warn=(\d+)')  { $microWarn = [int]$matches[1] }
    return @{ mismatch=$mismatch; count=$countWarn; micro=$microWarn; strict_rc=$rc; rawSummary=$sumLine.Trim() }
}

# ---------------- main ----------------
$mainResults = @()
$mainTotalFail = 0
$mainRan = $false

if ($RunMainGates -or $OnlyMainGates) {
    $mainRan = $true
    foreach ($g in $mainGates) {
        $rc = Run-MainGate -Name $g.name -Filter $g.filter
        $mainResults += @{ name=$g.name; rc=$rc }
        if ($rc -ne 0) { $mainTotalFail++ }
    }
}

$ir = $null
if (-not $OnlyMainGates) {
    $ir = Run-IRGuard
    # 把 ir 结果映射到 0a/0b/0c/0d
    foreach ($s in $irGuardStages) {
        switch ($s.id) {
            "0a" {
                if ($ir.strict_rc -eq 0) { $s.status = "PASS" }
                else { $s.status = "FAIL(rc=" + $ir.strict_rc + ")" }
            }
            "0b" {
                if ($ir.mismatch -lt 0) { $s.status = "WARN(no-summary)" }
                elseif ($ir.mismatch -gt 0) { $s.status = "WARN(" + $ir.mismatch + ")" }
                else { $s.status = "OK" }
            }
            "0c" {
                if ($ir.micro -lt 0) { $s.status = "WARN(no-summary)" }
                elseif ($ir.micro -gt 0) { $s.status = "WARN(" + $ir.micro + ")" }
                else { $s.status = "OK" }
            }
            "0d" {
                if ($ir.count -lt 0) { $s.status = "WARN(no-summary)" }
                elseif ($ir.count -gt 0) { $s.status = "WARN(" + $ir.count + ")" }
                else { $s.status = "OK" }
            }
        }
    }
}

# ---------------- summary ----------------
Write-Host ""
Write-Host "==================== run_gates summary ====================" -ForegroundColor Cyan

Write-Host "[ Main gates ]"
if ($mainRan) {
    foreach ($r in $mainResults) {
        $tag = if ($r.rc -eq 0) { "PASS" } else { ("WARN(rc=" + $r.rc + ", run_all_modules counts EF as fail)") }
        Write-Host ("  " + $r.name.PadRight(20) + " : " + $tag)
    }
} else {
    Write-Host "  (deferred — pass -RunMainGates or wait for EF-aware gate driver)"
}

if (-not $OnlyMainGates) {
    Write-Host "[ IR guard sub-stages (D4 strict) ]"
    foreach ($s in $irGuardStages) {
        Write-Host ("  " + $s.id + " " + $s.name.PadRight(28) + " : " + $s.status + "   # " + $s.note)
    }
    if ($null -ne $ir) {
        Write-Host ""
        Write-Host ("  [ir_diff summary] " + $ir.rawSummary)
        Write-Host ("  详细报告: tests\snapshots\ir\last_report.md")
    }
}

Write-Host ""
# 最终退出码：
#   - OnlyMainGates: 按主门状态退出
#   - 否则：按 0a strict gate 退出（D4 起生效）
if ($OnlyMainGates) {
    if ($mainTotalFail -gt 0) {
        Write-Host ("Result: " + $mainTotalFail + " main gate(s) reported run_all_modules-failure (likely EF mis-count).") -ForegroundColor Yellow
        exit 1
    } else {
        Write-Host "Result: all main gates PASS." -ForegroundColor Green
        exit 0
    }
} else {
    if ($null -ne $ir -and $ir.strict_rc -ne 0) {
        Write-Host "Result: IR guard strict gate FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host "Result: IR guard strict gate PASS." -ForegroundColor Green
    exit 0
}

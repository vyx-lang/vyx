# scripts/ir_diff.ps1
#
# IR Regression Guard — D1 + D2-D3 (PLAN_IR_REGRESSION_GUARD.md)
#
# Modes:
#   -Mode check     默认。读 manifest，重生 IR：1) hash diff  2) instr count diff(±5%)  3) micropoints
#                   warn 模式（exit 0 即便 mismatch；mismatch 走 WARN 行 + 写 last_report.md）
#   -Mode generate  首次生成基准（仅当 baseline 不存在时落盘；已存在的 case 跳过）
#   -Mode bless     刷新 baseline（覆盖）。必须显式 -ConfirmBless 或交互输入 BLESS
#
# 落地范围：
#   - tests/snapshots/ir/{O0,O2}/<case-id>.ll.norm     规范化后的 IR 文本
#   - tests/snapshots/ir/{O0,O2}/<case-id>.sha256      校验和
#   - tests/snapshots/ir/{O0,O2}/<case-id>.counts.json D2: 关键 opcode 计数基线
#   - tests/snapshots/ir/micropoints/<case-id>.txt     D2: 微点测 DSL（可选；缺则跳过）
#   - tests/snapshots/ir/meta.json                     LLVM/vyxc 版本与生成时间
#   - tests/snapshots/ir/last_report.md                D2: 最近一次 check 的可读报告
#
# Normalize 规则（保守，D1+D2 持平；如发现噪声再扩展并刷基线）：
#   - 删除头部 4 行：ModuleID / source_filename / target datalayout / target triple
#   - 把 \r\n 统一为 \n
#   - 路径中的盘符段（如 e:\Dev\C++\VyxLan\）替换为 <REPO>
#
# Micropoints DSL（每行一条，# 注释）：
#   must_have      <regex>          必须命中
#   must_not_have  <regex>          必须不命中
#   count          <regex> >= <N>   命中行数下限
#   count          <regex> <= <N>   命中行数上限
#   count          <regex> == <N>   等于
#
# 用法举例：
#   powershell -File scripts/ir_diff.ps1                     # check (warn-only)
#   powershell -File scripts/ir_diff.ps1 -Mode generate      # 首次生成基准
#   powershell -File scripts/ir_diff.ps1 -Mode bless         # 刷新基准（交互确认）
#   powershell -File scripts/ir_diff.ps1 -Mode bless -ConfirmBless  # 非交互（评审 commit 用）

[CmdletBinding()]
param(
    [ValidateSet("check","generate","bless")]
    [string]$Mode = "check",

    [string]$Compiler = "",
    [string]$ManifestPath = "",
    [string[]]$Levels = @("O0","O2"),

    # bless 模式必需显式确认；否则进入交互式 prompt
    [switch]$ConfirmBless,

    # 强失败模式（D4 阶段才默认开启；目前仅供调试）
    [switch]$Strict,

    # instr count ±tolerance；默认 5%。允许 0 → 严格相等。
    [double]$CountTolerance = 0.05
)

$ErrorActionPreference = "Stop"

# ---------- 路径解析 ----------
$repoRoot   = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$snapRoot   = Join-Path $repoRoot "tests\snapshots\ir"
if ($ManifestPath -eq "") { $ManifestPath = Join-Path $snapRoot "manifest.txt" }
if (-not (Test-Path $ManifestPath)) { throw "manifest not found: $ManifestPath" }

function Resolve-Compiler {
    param([string]$Explicit)
    if ($Explicit -ne "") {
        if (-not (Test-Path $Explicit)) { throw "vyxc not found at: $Explicit" }
        return (Resolve-Path $Explicit).Path
    }
    if ($env:VYX_HOST_VYXC -and (Test-Path $env:VYX_HOST_VYXC)) {
        return (Resolve-Path $env:VYX_HOST_VYXC).Path
    }
    foreach ($cand in @(
        (Join-Path $repoRoot "cmake-build-debug\vyxc.exe"),
        (Join-Path $repoRoot "build\vyxc.exe")
    )) {
        if (Test-Path $cand) { return (Resolve-Path $cand).Path }
    }
    throw "host vyxc not found; pass -Compiler or set VYX_HOST_VYXC"
}

$vyxc = Resolve-Compiler $Compiler

function Resolve-IrEmissionArgs {
    param([string]$CompilerPath)

    # Bootstrap/current vyxc uses --emit=ir, while older host builds exposed
    # the legacy --emit-ir switch. Probe once so the guard keeps working across
    # both compiler generations.
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $help = (& $CompilerPath --help 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($help -match '(?m)^\s*--emit=<kind>') {
        return @("--emit=ir")
    }
    return @("--emit-ir")
}

$script:IrEmissionArgs = @(Resolve-IrEmissionArgs $vyxc)

function Resolve-IrSourceArgs {
    param([string]$CompilerPath)
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $help = (& $CompilerPath --help 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($help -match '(?m)--src=<kind>') {
        return @("--src=file")
    }
    return @()
}

$script:IrSourceArgs = @(Resolve-IrSourceArgs $vyxc)

# ---------- manifest ----------
function Read-Manifest {
    param([string]$Path)
    $items = @()
    foreach ($raw in [System.IO.File]::ReadAllLines($Path)) {
        $line = $raw.Trim()
        if ($line -eq "" -or $line.StartsWith("#")) { continue }
        # `snapshot-id | source/path.vyx` keeps golden filenames stable when a
        # source suite is reorganized. Legacy path-only rows remain valid and
        # derive their ID from the path below.
        $separator = $line.IndexOf("|")
        if ($separator -ge 0) {
            $snapshotId = $line.Substring(0, $separator).Trim()
            $sourcePath = $line.Substring($separator + 1).Trim()
            if ($snapshotId.Length -eq 0 -or $sourcePath.Length -eq 0) {
                throw "invalid manifest mapping: $raw"
            }
            $items += ,[pscustomobject]@{ SnapshotId = $snapshotId; SourcePath = $sourcePath }
        } else {
            $items += ,[pscustomobject]@{ SnapshotId = ""; SourcePath = $line }
        }
    }
    return ,$items
}

$cases = Read-Manifest -Path $ManifestPath
if ($cases.Count -eq 0) { throw "manifest empty: $ManifestPath" }

# ---------- normalize ----------
function Get-CaseId {
    param([string]$RelPath)
    # tests/cases/_rc_smoke.vyx -> tests__cases___rc_smoke (no extension)
    $id = $RelPath -replace "[\\/]", "__"
    if ($id.ToLower().EndsWith(".vyx")) { $id = $id.Substring(0, $id.Length - 4) }
    return $id
}

function Normalize-IR {
    param([string]$Text)
    if ($null -eq $Text) { return "" }
    $text = $Text -replace "`r`n", "`n"
    $lines = $text.Split("`n")
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($l in $lines) {
        if ($l -match "^\s*;\s*ModuleID\b") { continue }
        if ($l -match "^\s*source_filename\b")  { continue }
        if ($l -match "^\s*target\s+datalayout\b") { continue }
        if ($l -match "^\s*target\s+triple\b")     { continue }
        $ll = $l -replace [regex]::Escape($repoRoot), "<REPO>"
        $ll = $ll -replace [regex]::Escape($repoRoot.Replace("\","/")), "<REPO>"
        $out.Add($ll)
    }
    # 收尾空行去重
    while ($out.Count -gt 0 -and $out[$out.Count-1] -eq "") { $out.RemoveAt($out.Count-1) }
    return ($out -join "`n") + "`n"
}

function Sha256-OfText {
    param([string]$Text)
    $bytes  = [System.Text.Encoding]::UTF8.GetBytes($Text)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $hasher.ComputeHash($bytes)
        return ([System.BitConverter]::ToString($hash) -replace "-","").ToLower()
    } finally {
        $hasher.Dispose()
    }
}

# ---------- D2: instr count ----------
# 统计 IR 文本里关键 opcode 出现次数。
# 选 6 大类作为骨架（可扩展，但扩展后需一并刷基线）：
#   call / load / store / alloca / br / ret
# 实现方式：行内匹配 "  <opcode> "（前导两空格 + opcode + 空格）。LLVM 文本 IR
# 默认每条 instr 都有缩进；这避免 type-name 中的子串误中。
function CountOpcodes-OfText {
    param([string]$Text)
    $opcodes = @("call","load","store","alloca","br","ret")
    $result = @{}
    foreach ($op in $opcodes) {
        # 匹配 "  call " 或 "  call." 或赋值形式 "%foo = call"
        # 行级 regex：(^|=\s*)<op>\s
        $rx = [regex]::new("(?m)^\s+(?:%[\w\.]+\s*=\s*)?(?:tail\s+|notail\s+|musttail\s+)?$op(\s|$)")
        $matches = $rx.Matches($Text)
        $result[$op] = $matches.Count
    }
    return $result
}

# D5: counts.json 支持可选阈值字段：
#   _tol_default : 该用例的默认 ±tol（覆盖全局 -CountTolerance）
#   _tol         : { "<op>": <tol> }，单 opcode 阈值（覆盖 _tol_default）
# 旧格式（仅 opcode → count）仍兼容；缺失阈值字段时回退到全局。
function CountsToJson {
    param($Counts, $InheritFrom = $null)
    # InheritFrom 是已有 baseline（PSCustomObject）。若提供且包含 _tol_default / _tol，
    # 把它们合并进来，保证 generate/bless 写出时不会丢失人工配置。
    $obj = [ordered]@{}
    if ($null -ne $InheritFrom) {
        $tolDefault = $InheritFrom.PSObject.Properties['_tol_default']
        if ($null -ne $tolDefault) { $obj['_tol_default'] = [double]$tolDefault.Value }
        $tolMap = $InheritFrom.PSObject.Properties['_tol']
        if ($null -ne $tolMap -and $null -ne $tolMap.Value) {
            $tolHt = [ordered]@{}
            foreach ($p in $tolMap.Value.PSObject.Properties) {
                $tolHt[$p.Name] = [double]$p.Value
            }
            $obj['_tol'] = $tolHt
        }
    }
    foreach ($op in @("call","load","store","alloca","br","ret")) {
        $obj[$op] = [int]$Counts[$op]
    }
    return ($obj | ConvertTo-Json -Compress -Depth 4)
}

function ReadCountsFromFile {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    $raw = [System.IO.File]::ReadAllText($Path)
    return ($raw | ConvertFrom-Json)
}

# 解析 baseline 中的有效阈值（per-opcode → _tol_default → 全局 fallback）
function Resolve-OpTolerance {
    param($Baseline, [string]$Op, [double]$GlobalTol)
    if ($null -eq $Baseline) { return $GlobalTol }
    $tolMap = $Baseline.PSObject.Properties['_tol']
    if ($null -ne $tolMap -and $null -ne $tolMap.Value) {
        $perOp = $tolMap.Value.PSObject.Properties[$Op]
        if ($null -ne $perOp) { return [double]$perOp.Value }
    }
    $tolDefault = $Baseline.PSObject.Properties['_tol_default']
    if ($null -ne $tolDefault) { return [double]$tolDefault.Value }
    return $GlobalTol
}

# 返回 hashtable：超阈值的 opcode 列表（带百分比与 tol 来源标签）
function Diff-Counts {
    param($Baseline, $Current, [double]$Tol)
    $diffs = @()
    foreach ($op in @("call","load","store","alloca","br","ret")) {
        $b = [int]($Baseline.$op)
        $c = [int]$Current[$op]
        if ($b -eq 0 -and $c -eq 0) { continue }
        $opTol = Resolve-OpTolerance -Baseline $Baseline -Op $op -GlobalTol $Tol
        if ($b -eq 0) {
            # 0 → 非 0 视作 100% 增长
            $diffs += @{ op=$op; baseline=$b; current=$c; pct=999.0; tol=[Math]::Round($opTol*100,2) }
            continue
        }
        $pct = [Math]::Abs($c - $b) / [double]$b
        if ($pct -gt $opTol) {
            $diffs += @{ op=$op; baseline=$b; current=$c; pct=[Math]::Round($pct*100, 2); tol=[Math]::Round($opTol*100,2) }
        }
    }
    return $diffs
}

# ---------- D2: micropoints DSL ----------
# 解析单个 .txt 文件，对当前 IR 文本逐行执行断言；返回字符串数组（命中失败的行）。
function Run-Micropoints {
    param([string]$DslPath, [string]$Text)
    $errors = @()
    if (-not (Test-Path $DslPath)) { return ,$errors }
    foreach ($raw in [System.IO.File]::ReadAllLines($DslPath)) {
        $line = $raw.Trim()
        if ($line -eq "" -or $line.StartsWith("#")) { continue }
        # tokenize loosely
        if ($line -match '^must_have\s+(.+)$') {
            $rx = $matches[1]
            if (-not ([regex]::IsMatch($Text, $rx))) {
                $errors += "must_have FAIL: /$rx/"
            }
        } elseif ($line -match '^must_not_have\s+(.+)$') {
            $rx = $matches[1]
            if ([regex]::IsMatch($Text, $rx)) {
                $errors += "must_not_have FAIL: /$rx/"
            }
        } elseif ($line -match '^count\s+(.+?)\s+(>=|<=|==)\s+(\d+)\s*$') {
            $rx = $matches[1]
            $cmp = $matches[2]
            $n = [int]$matches[3]
            $cnt = ([regex]::Matches($Text, $rx)).Count
            $ok = $false
            switch ($cmp) {
                ">=" { $ok = ($cnt -ge $n) }
                "<=" { $ok = ($cnt -le $n) }
                "==" { $ok = ($cnt -eq $n) }
            }
            if (-not $ok) {
                $errors += "count FAIL: /$rx/ $cmp $n  (got $cnt)"
            }
        } else {
            $errors += "DSL parse error: '$line'"
        }
    }
    return ,$errors
}

# ---------- 编译并取 IR ----------
function Emit-IR {
    param([string]$VyxRel, [string]$Level)
    $tmpDir = Join-Path $env:TEMP ("vyx_ir_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $tmpDir | Out-Null
    try {
        $out = Join-Path $tmpDir "out.ll"
        # 局部把 ErrorActionPreference 调成 Continue，避免 vyxc 输出到 stderr 的 warning
        # 触发 PowerShell 的 NativeCommandError，把外部命令直接终止脚本。
        $prevEAP = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        Push-Location $repoRoot
        try {
            $compilerArgs = @($script:IrEmissionArgs) + @("-$Level", "-o", $out) + @($script:IrSourceArgs) + @($VyxRel)
            $combined = & $vyxc @compilerArgs 2>&1
            $rc = $LASTEXITCODE
        } finally {
            Pop-Location
            $ErrorActionPreference = $prevEAP
        }
        if ($rc -ne 0) {
            $err = ($combined | Out-String).Trim()
            throw "vyxc failed ($rc) for $VyxRel @ -$Level :`n$err"
        }
        if (-not (Test-Path $out)) { throw "vyxc produced no IR for $VyxRel @ -$Level" }
        return [System.IO.File]::ReadAllText($out)
    } finally {
        if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
    }
}

# ---------- 模式实现 ----------
function Confirm-Bless-Interactive {
    if ($ConfirmBless) { return $true }
    Write-Host "[bless] 即将覆盖所有 baseline。该操作仅应在评审通过后进行。"
    $resp = Read-Host "[bless] 输入 'BLESS' 确认（其他任意输入取消）"
    return ($resp -ceq "BLESS")
}

function Run-Mode {
    param([string]$Mode)

    if ($Mode -eq "bless" -and -not (Confirm-Bless-Interactive)) {
        Write-Host "[bless] 已取消（未输入 BLESS）。"
        return 2
    }

    foreach ($lvl in $Levels) {
        $dir = Join-Path $snapRoot $lvl
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    }

    $genCount = 0; $skipCount = 0; $matchCount = 0; $mismatchCount = 0; $missingCount = 0; $errCount = 0
    $countWarnCount = 0; $microWarnCount = 0
    $report = New-Object System.Collections.Generic.List[string]
    # md report 行（只在 check 模式输出）
    $mdRows = New-Object System.Collections.Generic.List[string]
    $mdRows.Add("| level | case-id | sha (got/want) | counts | micro |")
    $mdRows.Add("|---|---|---|---|---|")

    $microDir = Join-Path $snapRoot "micropoints"

    foreach ($case in $cases) {
        $rel = [string]$case.SourcePath
        $caseId = if ([string]::IsNullOrWhiteSpace($case.SnapshotId)) {
            Get-CaseId -RelPath $rel
        } else {
            [string]$case.SnapshotId
        }
        foreach ($lvl in $Levels) {
            $dir         = Join-Path $snapRoot $lvl
            $normPath    = Join-Path $dir ($caseId + ".ll.norm")
            $shaPath     = Join-Path $dir ($caseId + ".sha256")
            $countsPath  = Join-Path $dir ($caseId + ".counts.json")
            $dslPath     = Join-Path $microDir ($caseId + ".txt")

            try {
                $raw  = Emit-IR -VyxRel $rel -Level $lvl
                $norm = Normalize-IR -Text $raw
                $sha  = Sha256-OfText -Text $norm
                $counts = CountOpcodes-OfText -Text $norm
            } catch {
                $errCount++
                $report.Add(("ERR    " + $lvl + " " + $rel + " :: " + $_.Exception.Message))
                continue
            }

            $hasBaseline = (Test-Path $normPath) -and (Test-Path $shaPath)

            switch ($Mode) {
                "generate" {
                    if ($hasBaseline) {
                        # 即使 hash baseline 存在，也补齐缺失的 counts.json（D2 增量补丁）
                        if (-not (Test-Path $countsPath)) {
                            [System.IO.File]::WriteAllText($countsPath, (CountsToJson $counts))
                            $genCount++
                            $report.Add(("GEN+   " + $lvl + " " + $caseId + " counts (hash baseline kept)"))
                        } else {
                            $skipCount++
                            $report.Add(("SKIP   " + $lvl + " " + $caseId + " (all baselines exist; use -Mode bless to refresh)"))
                        }
                    } else {
                        [System.IO.File]::WriteAllText($normPath, $norm)
                        [System.IO.File]::WriteAllText($shaPath, $sha)
                        [System.IO.File]::WriteAllText($countsPath, (CountsToJson $counts))
                        $genCount++
                        $report.Add(("GEN    " + $lvl + " " + $caseId + " sha=" + $sha.Substring(0,16)))
                    }
                }
                "bless" {
                    # bless 写覆盖；但如果原 counts.json 已配置 _tol_default/_tol，
                    # 必须复用，避免 codegen 刷基线时把人工调整的阈值清掉。
                    $oldCounts = ReadCountsFromFile -Path $countsPath
                    [System.IO.File]::WriteAllText($normPath, $norm)
                    [System.IO.File]::WriteAllText($shaPath, $sha)
                    [System.IO.File]::WriteAllText($countsPath, (CountsToJson $counts $oldCounts))
                    $genCount++
                    $report.Add(("BLESS  " + $lvl + " " + $caseId + " sha=" + $sha.Substring(0,16)))
                }
                "check" {
                    if (-not $hasBaseline) {
                        $missingCount++
                        $report.Add(("MISS   " + $lvl + " " + $caseId + " (no baseline; run -Mode generate first)"))
                        $mdRows.Add(("| " + $lvl + " | " + $caseId + " | (missing) | - | - |"))
                        continue
                    }
                    $expSha = ([System.IO.File]::ReadAllText($shaPath)).Trim()
                    $shaOk = ($expSha -ceq $sha)
                    if ($shaOk) {
                        $matchCount++
                    } else {
                        $mismatchCount++
                        $report.Add(("WARN   " + $lvl + " " + $caseId + " sha-mismatch  got=" + $sha.Substring(0,16) + " want=" + $expSha.Substring(0,16)))
                    }

                    # count diff
                    $countSummary = "ok"
                    $baseCounts = ReadCountsFromFile -Path $countsPath
                    if ($null -ne $baseCounts) {
                        $diffs = Diff-Counts -Baseline $baseCounts -Current $counts -Tol $CountTolerance
                        if ($diffs.Count -gt 0) {
                            $countWarnCount++
                            $parts = @()
                            foreach ($d in $diffs) {
                                $parts += ($d.op + " " + $d.baseline + "→" + $d.current + " (" + $d.pct + "% > " + $d.tol + "%)")
                            }
                            $countSummary = ("WARN " + ($parts -join "; "))
                            $report.Add(("WARN   " + $lvl + " " + $caseId + " count " + ($parts -join "; ")))
                        }
                    } else {
                        $countSummary = "(no counts baseline)"
                    }

                    # micropoints — D2 仅在 O0 跑（O2 经过优化，断言会因 inlining/DCE 不稳定）
                    $microSummary = "-"
                    if ($lvl -eq "O0" -and (Test-Path $dslPath)) {
                        $microErrs = Run-Micropoints -DslPath $dslPath -Text $norm
                        if ($microErrs.Count -gt 0) {
                            $microWarnCount++
                            $microSummary = ("WARN " + ($microErrs -join "; "))
                            foreach ($e in $microErrs) {
                                $report.Add(("WARN   " + $lvl + " " + $caseId + " micro " + $e))
                            }
                        } else {
                            $microSummary = "ok"
                        }
                    }

                    $shaCell = if ($shaOk) { "ok" } else { ($sha.Substring(0,12) + " / " + $expSha.Substring(0,12)) }
                    $mdRows.Add(("| " + $lvl + " | " + $caseId + " | " + $shaCell + " | " + $countSummary + " | " + $microSummary + " |"))
                }
            }
        }
    }

    # meta.json (generate / bless 模式)
    if ($Mode -eq "generate" -or $Mode -eq "bless") {
        $meta = [ordered]@{
            generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
            vyxc             = $vyxc
            levels           = $Levels
            normalize_rules  = "strip headers (ModuleID/source_filename/target_datalayout/target_triple); replace repo root with <REPO>"
            mode             = $Mode
            case_count       = $cases.Count
        }
        # D6: 派生 industrial_full（manifest 把工业泛型 suite 的全部
        # codegenable 用例都纳入即视为全集）。
        $industrialInManifest = @($cases | Where-Object { $_.SourcePath -match 'cases/conformance/generics/industrial/' }).Count
        $industrialOnDisk     = @(Get-ChildItem -Path (Join-Path $repoRoot "tests\cases\conformance\generics\industrial") -Filter *.vyx).Count
        # 排除 EF 用例（chain_diag_3level / hkt_functor）—— 当前 EF 数=2
        $industrialEFCount    = 2
        $meta['industrial_total_in_repo']        = $industrialOnDisk
        $meta['industrial_ef_excluded']          = $industrialEFCount
        $meta['industrial_in_manifest']          = $industrialInManifest
        $meta['industrial_full']                 = ($industrialInManifest -ge ($industrialOnDisk - $industrialEFCount))
        $meta | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $snapRoot "meta.json") -Encoding utf8
    }

    Write-Host "----"
    foreach ($line in $report) { Write-Host $line }
    Write-Host "----"
    Write-Host ("[ir_diff] mode=" + $Mode + " strict=" + $Strict + " gen=" + $genCount + " skip=" + $skipCount + " match=" + $matchCount + " mismatch=" + $mismatchCount + " missing=" + $missingCount + " err=" + $errCount + " count_warn=" + $countWarnCount + " micro_warn=" + $microWarnCount)

    # check 模式输出 markdown 报告（D2 要求）
    if ($Mode -eq "check") {
        $reportPath = Join-Path $snapRoot "last_report.md"
        $tplPath    = Join-Path $snapRoot "REPORT.tpl.md"
        $body = @()
        $body += "# IR Regression Guard — last_report"
        $body += ""
        $body += "_Generated by `scripts/ir_diff.ps1 -Mode check`_"
        $body += ""
        $body += "- Generated at (UTC): " + ((Get-Date).ToUniversalTime().ToString("o"))
        $body += "- Compiler: ``" + $vyxc + "``"
        $body += "- Cases: " + $cases.Count + " × levels " + ($Levels -join ",") + " = " + ($cases.Count * $Levels.Count)
        $body += "- match=" + $matchCount + " mismatch=" + $mismatchCount + " count_warn=" + $countWarnCount + " micro_warn=" + $microWarnCount + " missing=" + $missingCount + " err=" + $errCount
        if ($Strict) {
            $body += "- Mode: STRICT（D4）。任何 mismatch / count_warn / micro_warn 都会让本脚本 exit=1。"
        } else {
            $body += "- Mode: WARN-only（D2-D3）。任何 mismatch / count_warn / micro_warn 都不会让本脚本 fail。"
        }
        $body += "- Tolerance: instr count ±" + ([Math]::Round($CountTolerance*100, 2)) + "% (CLI fallback; D5+ per-case 真实阈值见 ``counts.json`` 的 ``_tol_default`` / ``_tol`` 字段)"
        $body += ""
        $body += "## Per-case status"
        $body += ""
        $body += $mdRows -join "`n"
        $body += ""
        if ($mismatchCount -gt 0 -or $countWarnCount -gt 0 -or $microWarnCount -gt 0) {
            $body += "## Notes"
            $body += ""
            $body += "若上表出现 WARN，按以下顺序处置："
            $body += ""
            $body += "1. 本地 ``git diff`` 确认 codegen 是否预期变更。"
            $body += "2. 若是预期变更：``scripts/ir_diff.ps1 -Mode bless`` 刷基线（评审 commit 中）。"
            $body += "3. 若非预期：定位最近 codegen 修改并修复，重跑 check 直到全 ok。"
        }
        [System.IO.File]::WriteAllText($reportPath, ($body -join "`n"))
        # 模板（首次或 missing 时写入；不覆盖既有手改内容）
        if (-not (Test-Path $tplPath)) {
            $tpl = @(
                "# IR Regression Guard — Report Template",
                "",
                "<!-- 由 scripts/ir_diff.ps1 -Mode check 在每次跑后写入 last_report.md。",
                "     本模板说明列含义：",
                "     - level     : O0 / O2",
                "     - case-id   : 来自 manifest.txt 的衍生 id",
                "     - sha       : 'ok' 或 'got_prefix / want_prefix'",
                "     - counts    : 'ok' / '(no counts baseline)' / 'WARN <op> <base>→<cur> (<pct>%)'",
                "     - micro     : '-'(无 dsl) / 'ok' / 'WARN <DSL 报错>' -->"
            ) -join "`n"
            [System.IO.File]::WriteAllText($tplPath, $tpl)
        }
    }

    if ($Mode -eq "check") {
        if ($errCount -gt 0)        { return 4 }
        if ($missingCount -gt 0)    { return 3 }
        if ($mismatchCount -gt 0 -or $countWarnCount -gt 0 -or $microWarnCount -gt 0) {
            if ($Strict) { return 1 } else { return 0 }   # warn-only by default (D2-D3 phase)
        }
        return 0
    } else {
        if ($errCount -gt 0) { return 4 } else { return 0 }
    }
}

$rc = Run-Mode -Mode $Mode
exit $rc

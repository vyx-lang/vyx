# run_newmir_focus.ps1 — MIR 路径快速回归信号
#
# 固定跑 43 个 focus 用例(28 个已知失败 + 15 个代表性绿用例,覆盖
# 泛型/反射/drop/trait/字符串/控制流),输出每用例 pass/fail + 失败阶段
# 标签(HIR/MIR/LLVM/runtime-crash/runtime/timeout),并与基线 json 对比
# 报告新增失败/新增通过。避免每个迁移波次都跑 301 全集。
#
# Usage(在仓库根):
#   powershell -File scripts/run_newmir_focus.ps1
#   powershell -File scripts/run_newmir_focus.ps1 -BaselineJson tests\.cache\newmir_focus_baseline.json
#   powershell -File scripts/run_newmir_focus.ps1 -BaselineJson <path> -SaveBaseline   # 强制覆盖保存
#   powershell -File scripts/run_newmir_focus.ps1 -BootstrapCompiler E:\path\to\vyxc.exe
#
# BaselineJson 语义: 文件不存在 => 运行后保存为基线;
#                    文件存在   => 与之对比, 仅 -SaveBaseline 时覆盖更新。
# 退出码: 0 = 无新增失败(允许新增通过); 1 = 出现基线外的新增失败。

param(
    [string]$BootstrapCompiler = "",
    [string[]]$BootstrapCompilerArgs = @(),
    [int]$RunTimeoutSec = 30,
    [int]$MemoryLimitMB = 0,
    [string]$BaselineJson = "",
    [switch]$SaveBaseline,
    [string]$LogPath = "",
    [string]$RuntimeDir = "",
    [string]$RuntimeLib = ""
)

$ErrorActionPreference = "Stop"

$script:FocusIsWindows = $false
if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    $script:FocusIsWindows = [bool]$IsWindows
} else {
    $script:FocusIsWindows = ($env:OS -eq "Windows_NT")
}

# scripts/ -> 仓库根
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$testsRoot = Join-Path $repoRoot "tests"
$cacheRoot = Join-Path $testsRoot ".cache"
if (-not (Test-Path $cacheRoot)) {
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
}

# ---------------------------------------------------------------- 用例清单
# expected: fail = 上轮诊断的 28 个已知失败; pass = 15 个代表性绿用例
$focusCases = @(
    # --- 28 known-failing (诊断分类 1-8) ---
    @{ Rel = 'tests\cases\conformance\adversarial\round1\adv_10_const_inside_pack.vyx';                          Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\adversarial\round1\adv_11_empty_pack_fold.vyx';                            Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\adversarial\round3\a3_01_class_method_generic.vyx';                       Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\adversarial\round3\a3_09_pack_zero_fold_mul.vyx';                         Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\adversarial\round3\a3_10_variadic_class.vyx';                             Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\generics\industrial\full_spec_vec.vyx';                             Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\generics\industrial\hkt_functor.vyx';                               Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\generics\industrial\mono_cycle.vyx';                                Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\generics\industrial\sizeof_dispatch.vyx';                           Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\generics\industrial\trait_object_vec.vyx';                          Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\generics\industrial\turbofish_inference.vyx';                       Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\strict\strict_01_pack_boundary.vyx';                                Expected = 'fail' },
    @{ Rel = 'tests\cases\conformance\strict\strict_06_reflect_where_neg.vyx';                            Expected = 'fail' },
    @{ Rel = 'tests\cases\_clone_semantics.vyx';                                         Expected = 'fail' },
    @{ Rel = 'tests\cases\_history_regression_stress.vyx';                               Expected = 'fail' },
    @{ Rel = 'tests\cases\_p4_early_return_drop.vyx';                                    Expected = 'fail' },
    @{ Rel = 'tests\cases\bootstrap_trust_test.vyx';                                     Expected = 'fail' },
    @{ Rel = 'tests\cases\concepts\ok_struct_where.vyx';                                 Expected = 'fail' },
    @{ Rel = 'tests\cases\lang_validation\test_edge_stress.vyx';                         Expected = 'fail' },
    @{ Rel = 'tests\cases\lang_validation\test_oop_polymorphism.vyx';                    Expected = 'fail' },
    @{ Rel = 'tests\cases\lang_validation\test_ref_fat_value_abi.vyx';                   Expected = 'fail' },
    @{ Rel = 'tests\cases\lang_validation\test_same_short_name_no_imported_drop.vyx';    Expected = 'fail' },
    @{ Rel = 'tests\cases\reflection_bind_smoke.vyx';                                    Expected = 'fail' },
    @{ Rel = 'tests\cases\reflection_full_model.vyx';                                    Expected = 'fail' },
    @{ Rel = 'tests\cases\reflection_property_smoke.vyx';                                Expected = 'fail' },
    @{ Rel = 'tests\cases\reflection_static_manifest.vyx';                               Expected = 'fail' },
    @{ Rel = 'tests\cases\smoke\common\common_result_try.vyx';                                       Expected = 'fail' },
    @{ Rel = 'tests\cases\interpreters\brainfuck_interpreter_io.vyx';              Expected = 'fail' },
    # --- 15 representative green (泛型/反射/drop/trait/字符串/控制流) ---
    @{ Rel = 'tests\cases\conformance\adversarial\round1\adv_01_reflect_in_class_method.vyx';                    Expected = 'pass' },  # 反射+类方法
    @{ Rel = 'tests\cases\conformance\adversarial\round1\adv_02_typeof_pack_element.vyx';                        Expected = 'pass' },  # pack/typeof
    @{ Rel = 'tests\cases\conformance\adversarial\round3\a3_02_option_generic.vyx';                             Expected = 'pass' },  # 泛型 Option
    @{ Rel = 'tests\cases\conformance\generics\industrial\generic_array_init.vyx';                        Expected = 'pass' },  # 泛型数组
    @{ Rel = 'tests\cases\conformance\generics\industrial\lambda_capture_generic.vyx';                    Expected = 'pass' },  # 泛型 lambda 捕获
    @{ Rel = 'tests\cases\conformance\generics\industrial\smart_ptr_clone.vyx';                           Expected = 'pass' },  # 智能指针 clone
    @{ Rel = 'tests\cases\_p4_reassign_drop.vyx';                                        Expected = 'pass' },  # 重赋值 drop
    @{ Rel = 'tests\cases\lang_validation\test_return_owned_field_autodrop.vyx';         Expected = 'pass' },  # 所有权返回 autodrop
    @{ Rel = 'tests\cases\reflection_gettype_smoke.vyx';                                 Expected = 'pass' },  # 反射 gettype
    @{ Rel = 'tests\cases\stress\test_control_flow.vyx';                                 Expected = 'pass' },  # 控制流
    @{ Rel = 'tests\cases\stress\test_oop.vyx';                                          Expected = 'pass' },  # trait/OOP
    @{ Rel = 'tests\cases\stress\test_strings_collections.vyx';                          Expected = 'pass' },  # 字符串/集合
    @{ Rel = 'tests\cases\smoke\common\common_smoke_arith.vyx';                                      Expected = 'pass' },  # 基础算术/控制流
    @{ Rel = 'tests\cases\interpreters\json_parser_io.vyx';                        Expected = 'pass' },  # 字符串解析
    @{ Rel = 'tests\projects\trust_proj\src\main.vyx';                                   Expected = 'pass' }   # 字符串插值
)

# ---------------------------------------------------------------- 日志
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $cacheRoot ("newmir_focus_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".log")
} elseif (-not [System.IO.Path]::IsPathRooted($LogPath)) {
    $LogPath = Join-Path $repoRoot $LogPath
}
$LogPath = [System.IO.Path]::GetFullPath($LogPath)
$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
}
"" | Set-Content -Encoding utf8 -LiteralPath $LogPath
$script:FocusLogPath = $LogPath

function Write-FocusLine {
    param([string]$Message = "")
    Write-Host $Message
    try { [Console]::Out.Flush() } catch {}
    Add-Content -Encoding utf8 -LiteralPath $script:FocusLogPath -Value $Message
}

# ---------------------------------------------------------------- WER 抑制(复用主测试基建)
$processHelper = Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1"
if (Test-Path $processHelper) {
    . $processHelper
}
if (Get-Command Initialize-VyxCrashSuppression -ErrorAction SilentlyContinue) {
    Initialize-VyxCrashSuppression
}

function Quote-FocusArg {
    param([string]$Arg)
    if ($null -eq $Arg -or $Arg.Length -eq 0) { return '""' }
    if ($Arg -notmatch '[\s"]') { return $Arg }
    return '"' + $Arg.Replace('"', '\"') + '"'
}

# 与 tests/run_all_modules.ps1 的 Invoke-RunAllLocalProcess 同约定:
# 优先 Invoke-VyxProcess(SetErrorMode/SEM_NOGPFAULTERRORBOX + 内存 Job),否则本地回退。
function Invoke-FocusProcess {
    param(
        [Parameter(Mandatory=$true)][string]$FilePath,
        [Parameter(Mandatory=$true)][string[]]$ArgumentList,
        [Parameter(Mandatory=$true)][string]$WorkingDirectory,
        [Parameter(Mandatory=$true)][string]$StdoutLog,
        [Parameter(Mandatory=$true)][string]$StderrLog,
        [Parameter(Mandatory=$true)][string]$DialogLog,
        [int]$TimeoutSec = 30,
        [int]$MemoryLimitMB = 0
    )

    if (Get-Command Invoke-VyxProcess -ErrorAction SilentlyContinue) {
        return Invoke-VyxProcess -FilePath $FilePath `
            -ArgumentList $ArgumentList `
            -WorkingDirectory $WorkingDirectory `
            -StdoutLog $StdoutLog `
            -StderrLog $StderrLog `
            -DialogLog $DialogLog `
            -TimeoutSec $TimeoutSec `
            -MemoryLimitMB $MemoryLimitMB
    }

    foreach ($p in @($StdoutLog, $StderrLog, $DialogLog)) {
        if (Test-Path $p) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue }
    }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FilePath
    $psi.Arguments = ($ArgumentList | ForEach-Object { Quote-FocusArg $_ }) -join ' '
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    if (Get-Command Set-VyxProcessCrashSuppressionEnvironment -ErrorAction SilentlyContinue) {
        Set-VyxProcessCrashSuppressionEnvironment -ProcessStartInfo $psi
    } else {
        try { $psi.EnvironmentVariables["SEM_NOGPFAULTERRORBOX"] = "1" } catch {}
        try { $psi.EnvironmentVariables["__COMPAT_LAYER"] = "DisableWerUI" } catch {}
        try { $psi.EnvironmentVariables["VYX_DISABLE_WER_UI"] = "1" } catch {}
    }

    $proc = [System.Diagnostics.Process]::new()
    $proc.StartInfo = $psi
    $timedOut = $false
    try {
        [void]$proc.Start()
        $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
        $stderrTask = $proc.StandardError.ReadToEndAsync()
        $timeoutMs = [Math]::Max(1, $TimeoutSec) * 1000
        if (-not $proc.WaitForExit($timeoutMs)) {
            $timedOut = $true
            try { $proc.Kill() } catch {}
            try { $proc.WaitForExit() } catch {}
        } else {
            try { $proc.WaitForExit() } catch {}
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        Set-Content -Encoding utf8 -LiteralPath $StdoutLog -Value $stdout
        Set-Content -Encoding utf8 -LiteralPath $StderrLog -Value $stderr
        "" | Set-Content -Encoding utf8 -LiteralPath $DialogLog
        $exit = $proc.ExitCode
        if ($timedOut) { $exit = -2147483648 }
        return [pscustomobject]@{
            ExitCode = $exit
            TimedOut = $timedOut
            DialogCaught = $false
            MemoryExceeded = $false
            PeakWorkingSetMB = 0.0
            StdoutLog = $StdoutLog
            StderrLog = $StderrLog
            DialogLog = $DialogLog
        }
    } catch {
        $msg = "failed to launch: " + $FilePath + " " + ($ArgumentList -join " ") + "`n" + $_.Exception.Message
        $msg | Set-Content -Encoding utf8 -LiteralPath $DialogLog
        "" | Set-Content -Encoding utf8 -LiteralPath $StdoutLog
        "" | Set-Content -Encoding utf8 -LiteralPath $StderrLog
        return [pscustomobject]@{
            ExitCode = 1
            TimedOut = $false
            DialogCaught = $true
            MemoryExceeded = $false
            PeakWorkingSetMB = 0.0
            StdoutLog = $StdoutLog
            StderrLog = $StderrLog
            DialogLog = $DialogLog
        }
    } finally {
        try { $proc.Dispose() } catch {}
    }
}

# ---------------------------------------------------------------- 编译器与运行时解析
function Resolve-FocusBootstrapCompiler {
    param([string]$Explicit)
    if ($Explicit -ne "") { return (Resolve-Path $Explicit).Path }
    if ($env:VYX_BOOTSTRAP_VYXC -and (Test-Path $env:VYX_BOOTSTRAP_VYXC)) {
        return (Resolve-Path $env:VYX_BOOTSTRAP_VYXC).Path
    }
    $default = Join-Path $repoRoot "bootstrap_compiler\out\vyxc.exe"
    if (Test-Path $default) { return (Resolve-Path $default).Path }
    return $null
}

function Resolve-FocusRuntimeDir {
    param([string]$Explicit)
    if ($Explicit -ne "") { return (Resolve-Path $Explicit).Path }
    foreach ($d in @(
        (Join-Path $repoRoot "bootstrap_compiler\out"),
        (Join-Path $repoRoot "bootstrap_compiler\out\vyx_rt"),
        (Join-Path $repoRoot "build_yolo_vyxcg\vyx_codegen"),
        (Join-Path $repoRoot "build_yolo_vyxcg\vyx_rt")
    )) {
        if ((Test-Path (Join-Path $d "vyx_runtime.lib")) -or
            (Test-Path (Join-Path $d "libvyx_runtime.a")) -or
            (Test-Path (Join-Path $d "vyx_rt.lib")) -or
            (Test-Path (Join-Path $d "libvyx_rt.dll.a")) -or
            (Test-Path (Join-Path $d "libvyx_rt.so")) -or
            (Test-Path (Join-Path $d "libvyx_rt.dylib"))) {
            return (Resolve-Path $d).Path
        }
    }
    return $null
}

$vyxcBoot = Resolve-FocusBootstrapCompiler $BootstrapCompiler
if (-not $vyxcBoot) {
    Write-Error "Bootstrap vyxc not found. Pass -BootstrapCompiler or set `$env:VYX_BOOTSTRAP_VYXC (default: bootstrap_compiler\out\vyxc.exe)"
}

$resolvedRuntimeDir = Resolve-FocusRuntimeDir $RuntimeDir
$resolvedRuntimeLib = if ($RuntimeLib -ne "") { $RuntimeLib } else { "vyx_runtime" }
$runtimeArgs = @()
if ($resolvedRuntimeDir) {
    if ($script:FocusIsWindows) {
        $env:Path = $resolvedRuntimeDir + ";" + $env:Path
    } else {
        $env:PATH = $resolvedRuntimeDir + ":" + $env:PATH
        $env:LD_LIBRARY_PATH = $resolvedRuntimeDir + ":" + $env:LD_LIBRARY_PATH
    }
    $runtimeArgs = @("-L", $resolvedRuntimeDir, "-l", $resolvedRuntimeLib)
}

# ---------------------------------------------------------------- 失败阶段推断
# 优先级: 最早失败的阶段优先 (HIR > MIR > LLVM > runtime)。
# 注: "llvm-mir-lower: refusing incomplete HIR unit" 由 hir2-verify:error 兜住归 HIR;
#     "llvm-mir-lower: refusing invalid MIR unit" 由 mir2-verify:error 兜住归 MIR。
function Get-FailureStage {
    param(
        [int]$ExitCode,
        [bool]$TimedOut,
        [bool]$DialogCaught,
        [bool]$MemoryExceeded,
        [string]$StdoutText,
        [string]$StderrText
    )

    if ($ExitCode -eq 0) { return "pass" }
    if ($TimedOut) { return "timeout" }
    if ($MemoryExceeded) { return "memory" }

    $all = ($StdoutText + "`n" + $StderrText)

    if ($all -match 'hir2-verify:error' -or $all -match 'refusing incomplete HIR unit') { return "HIR" }
    if ($all -match 'mir2-verify:error' -or $all -match 'refusing invalid MIR unit') { return "MIR" }
    if ($all -match 'llvm-mir-lower:') { return "LLVM" }
    # 其它编译期诊断(sema/parse 等)同样导致 exit 1 且无运行输出
    if ($all -match 'fatal: I\d+' -or $all -match 'error: [A-Z]\d{4}') { return "compile" }

    # 大负数退出码 => Windows NTSTATUS 崩溃(0xC0000005 = -1073741819 等)
    if ($ExitCode -lt -1) { return "runtime-crash" }
    if ($DialogCaught) { return "runtime-crash" }

    # 正常退出但非零 => 程序内断言/语义错误
    return "runtime"
}

# ---------------------------------------------------------------- 跑用例
$focusRunDir = Join-Path $cacheRoot "newmir_focus_runs"
$focusLogDir = Join-Path $cacheRoot "newmir_focus_process_logs"
foreach ($d in @($focusRunDir, $focusLogDir)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
}

function Get-FocusSafeStem {
    param([string]$Rel)
    return ($Rel -replace '[\\/:*?"<>|]', '_')
}

Write-FocusLine ("newmir focus: {0} case(s), compiler={1}" -f $focusCases.Count, $vyxcBoot)
if ($resolvedRuntimeDir) {
    Write-FocusLine ("runtime: " + $resolvedRuntimeDir + " / " + $resolvedRuntimeLib)
} else {
    Write-FocusLine "runtime: 未找到, 命令不会追加 -L/-l"
}
Write-FocusLine ("timeout: {0}s  memory_limit: {1}MB  完整日志: {2}" -f $RunTimeoutSec, $MemoryLimitMB, $LogPath)
Write-FocusLine ""

$results = [ordered]@{}
$idx = 0
Push-Location $repoRoot
try {
    foreach ($case in $focusCases) {
        $idx++
        $rel = $case.Rel
        $full = Join-Path $repoRoot $rel
        $safeStem = Get-FocusSafeStem $rel

        if (-not (Test-Path $full)) {
            Write-FocusLine ("[{0}/{1}] {2}  =>  missing (test file not found)" -f $idx, $focusCases.Count, $rel)
            $results[$rel] = [ordered]@{ status = "fail"; stage = "missing"; exit = -999; expected = $case.Expected }
            continue
        }

        $outExe = Join-Path $focusRunDir ($safeStem + ".exe")
        foreach ($stale in @($outExe, ($outExe + ".tmp.obj"), ($outExe + ".tmp.ll"))) {
            if (Test-Path $stale) { Remove-Item -LiteralPath $stale -Force -ErrorAction SilentlyContinue }
        }
        $stdoutLog = Join-Path $focusLogDir ($safeStem + ".stdout.log")
        $stderrLog = Join-Path $focusLogDir ($safeStem + ".stderr.log")
        $dialogLog = Join-Path $focusLogDir ($safeStem + ".dialog.log")

        $args = @("--src=file", $full, "--run=aot") + $BootstrapCompilerArgs + @("-o", $outExe) + $runtimeArgs
        $run = Invoke-FocusProcess -FilePath $vyxcBoot `
            -ArgumentList $args `
            -WorkingDirectory $repoRoot `
            -StdoutLog $stdoutLog `
            -StderrLog $stderrLog `
            -DialogLog $dialogLog `
            -TimeoutSec $RunTimeoutSec `
            -MemoryLimitMB $MemoryLimitMB

        $stdoutText = ""
        $stderrText = ""
        if (Test-Path $stdoutLog) { $stdoutText = Get-Content -Raw -LiteralPath $stdoutLog }
        if (Test-Path $stderrLog) { $stderrText = Get-Content -Raw -LiteralPath $stderrLog }

        $stage = Get-FailureStage -ExitCode $run.ExitCode `
            -TimedOut ([bool]$run.TimedOut) `
            -DialogCaught ([bool]$run.DialogCaught) `
            -MemoryExceeded ([bool]$run.MemoryExceeded) `
            -StdoutText $stdoutText `
            -StderrText $stderrText
        $status = if ($stage -eq "pass") { "pass" } else { "fail" }

        $results[$rel] = [ordered]@{
            status   = $status
            stage    = $stage
            exit     = $run.ExitCode
            expected = $case.Expected
        }
        Write-FocusLine ("[{0}/{1}] {2}  =>  {3}({4}) exit={5}" -f $idx, $focusCases.Count, $rel, $status, $stage, $run.ExitCode)
    }
} finally {
    Pop-Location
}

# ---------------------------------------------------------------- 基线对比
$baselineResults = $null
$baselineLoaded = $false
if (-not [string]::IsNullOrWhiteSpace($BaselineJson)) {
    if (-not [System.IO.Path]::IsPathRooted($BaselineJson)) {
        $BaselineJson = Join-Path $repoRoot $BaselineJson
    }
    $BaselineJson = [System.IO.Path]::GetFullPath($BaselineJson)
    if ((Test-Path $BaselineJson) -and -not $SaveBaseline) {
        try {
            $baselineDoc = Get-Content -Raw -LiteralPath $BaselineJson | ConvertFrom-Json
            $baselineResults = @{}
            foreach ($prop in $baselineDoc.results.PSObject.Properties) {
                $baselineResults[$prop.Name] = $prop.Value
            }
            $baselineLoaded = $true
        } catch {
            Write-FocusLine ("警告: 基线 json 解析失败, 跳过对比: " + $_.Exception.Message)
        }
    }
}

$newFailures = New-Object System.Collections.Generic.List[string]
$newPasses = New-Object System.Collections.Generic.List[string]
$stageChanges = New-Object System.Collections.Generic.List[string]
if ($baselineLoaded) {
    foreach ($rel in $results.Keys) {
        $cur = $results[$rel]
        if (-not $baselineResults.ContainsKey($rel)) {
            if ($cur.status -eq "fail") { $newFailures.Add($rel) }
            continue
        }
        $base = $baselineResults[$rel]
        if ($base.status -eq "pass" -and $cur.status -eq "fail") {
            $newFailures.Add($rel)
        } elseif ($base.status -eq "fail" -and $cur.status -eq "pass") {
            $newPasses.Add($rel)
        } elseif ($base.status -eq "fail" -and $cur.status -eq "fail" -and $base.stage -ne $cur.stage) {
            $stageChanges.Add(("{0}: {1} -> {2}" -f $rel, $base.stage, $cur.stage))
        }
    }
}

# ---------------------------------------------------------------- 汇总表
$passCount = @($results.Values | Where-Object { $_.status -eq "pass" }).Count
$failCount = @($results.Values | Where-Object { $_.status -eq "fail" }).Count
$driftRows = @($results.GetEnumerator() | Where-Object {
    ($_.Value.expected -eq 'pass' -and $_.Value.status -eq 'fail') -or
    ($_.Value.expected -eq 'fail' -and $_.Value.status -eq 'pass')
})

Write-FocusLine ""
Write-FocusLine "================== newmir focus 汇总 =================="
Write-FocusLine ("{0,-72} {1,-6} {2,-14} {3,6}  {4}" -f "用例", "结果", "阶段", "exit", "vs基线")
foreach ($entry in $results.GetEnumerator()) {
    $rel = $entry.Key
    $r = $entry.Value
    $delta = ""
    if ($baselineLoaded) {
        if ($newFailures.Contains($rel)) { $delta = "NEW-FAIL" }
        elseif ($newPasses.Contains($rel)) { $delta = "NEW-PASS" }
        elseif ($baselineResults.ContainsKey($rel) -and $baselineResults[$rel].stage -ne $r.stage) { $delta = "STAGE-CHG" }
        else { $delta = "same" }
    }
    Write-FocusLine ("{0,-72} {1,-6} {2,-14} {3,6}  {4}" -f $rel, $r.status, $r.stage, $r.exit, $delta)
}
Write-FocusLine "-------------------------------------------------------"
Write-FocusLine ("合计: {0} pass / {1} fail (共 {2})" -f $passCount, $failCount, $results.Count)

$stageGroups = $results.Values | Where-Object { $_.status -eq "fail" } | Group-Object { $_.stage } | Sort-Object Count -Descending
if ($stageGroups) {
    Write-FocusLine ("失败阶段分布: " + (($stageGroups | ForEach-Object { "{0}={1}" -f $_.Name, $_.Count }) -join "  "))
}

if ($driftRows.Count -gt 0) {
    Write-FocusLine ""
    Write-FocusLine "与预期(28失败/15绿)的漂移:"
    foreach ($row in $driftRows) {
        Write-FocusLine ("  {0}: expected={1} actual={2}({3})" -f $row.Key, $row.Value.expected, $row.Value.status, $row.Value.stage)
    }
} else {
    Write-FocusLine "与预期(28失败/15绿)一致, 无漂移。"
}

if ($baselineLoaded) {
    Write-FocusLine ""
    Write-FocusLine ("基线: " + $BaselineJson)
    Write-FocusLine ("新增失败: {0}  新增通过: {1}  阶段变化: {2}" -f $newFailures.Count, $newPasses.Count, $stageChanges.Count)
    foreach ($f in $newFailures) { Write-FocusLine ("  NEW-FAIL: " + $f) }
    foreach ($p in $newPasses) { Write-FocusLine ("  NEW-PASS: " + $p) }
    foreach ($s in $stageChanges) { Write-FocusLine ("  STAGE-CHG: " + $s) }
}

# ---------------------------------------------------------------- 基线保存
if (-not [string]::IsNullOrWhiteSpace($BaselineJson)) {
    $shouldSave = $SaveBaseline -or (-not (Test-Path $BaselineJson))
    if ($shouldSave) {
        $doc = [ordered]@{
            generated = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
            compiler  = $vyxcBoot
            flags     = (@("--run=aot") + $BootstrapCompilerArgs)
            results   = $results
        }
        $baseDir = Split-Path -Parent $BaselineJson
        if ($baseDir -and -not (Test-Path $baseDir)) {
            New-Item -ItemType Directory -Force -Path $baseDir | Out-Null
        }
        $doc | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 -LiteralPath $BaselineJson
        Write-FocusLine ""
        Write-FocusLine ("基线已保存: " + $BaselineJson)
    }
}

Write-FocusLine ("完整日志: " + $LogPath)

if ($baselineLoaded -and $newFailures.Count -gt 0) {
    exit 1
}
exit 0

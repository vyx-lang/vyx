param(
    [string]$HostCompiler = "",
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [string]$RuntimeLib = "vyx_runtime",
    [string]$CasesCsv = "tests\checks\diagnostics\bootstrap_diagnostic_cases.csv",
    [string]$OutDir = "",
    [int]$RunTimeoutSec = 60,
    [int]$FixpointTimeoutSec = 300,
    [int]$StabilityReps = 10,
    [switch]$SelfhostOnly,
    [switch]$SkipStdTree,
    [switch]$SkipFixpoint,
    [switch]$SkipFullSweep,
    [switch]$FullHostSweep,
    [switch]$SkipAsan
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot ("tests\.cache\bootstrap_diagnostics_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
} elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $repoRoot $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$logPath = Join-Path $OutDir "diagnostic.log"
$csvPath = Join-Path $OutDir "diagnostic_results.csv"
$jsonPath = Join-Path $OutDir "diagnostic_summary.json"
"" | Set-Content -Encoding utf8 -LiteralPath $logPath

$processHelper = Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1"
if (Test-Path $processHelper) {
    . $processHelper
}
if (Get-Command Initialize-VyxCrashSuppression -ErrorAction SilentlyContinue) {
    Initialize-VyxCrashSuppression
}

function Write-Log {
    param([string]$Message = "")
    Write-Host $Message
    Add-Content -Encoding utf8 -LiteralPath $script:logPath -Value $Message
}

function Resolve-Tool {
    param([string]$Explicit, [string[]]$Candidates, [string]$Label)
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
        if (Test-Path -LiteralPath $Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
        $cmd = Get-Command $Explicit -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
        throw "$Label not found: $Explicit"
    }
    foreach ($c in $Candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    return ""
}

function Resolve-Runtime {
    param([string]$Explicit)
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
        if (-not (Test-Path -LiteralPath $Explicit)) { throw "RuntimeDir not found: $Explicit" }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    foreach ($d in @(
        (Join-Path $repoRoot "bootstrap_compiler\out"),
        (Join-Path $repoRoot "bootstrap_compiler\out\vyx_rt"),
        (Join-Path $repoRoot "build_yolo_vyxcg\vyx_codegen"),
        (Join-Path $repoRoot "build_yolo_vyxcg_release\vyx_codegen"),
        (Join-Path $repoRoot "build_vyxcg\vyx_codegen"),
        (Join-Path $repoRoot "build\vyx_codegen"),
        (Join-Path $repoRoot "cmake-build-debug\vyx_codegen")
    )) {
        if ((Test-Path (Join-Path $d "vyx_runtime.lib")) -or
            (Test-Path (Join-Path $d "libvyx_runtime.a")) -or
            (Test-Path (Join-Path $d "vyx_rt.lib")) -or
            (Test-Path (Join-Path $d "libvyx_rt.dll.a")) -or
            (Test-Path (Join-Path $d "libvyx_rt.so")) -or
            (Test-Path (Join-Path $d "libvyx_rt.dylib")) -or
            (Test-Path (Join-Path $d "vyx_codegen.lib")) -or
            (Test-Path (Join-Path $d "libvyx_codegen.dll.a")) -or
            (Test-Path (Join-Path $d "libvyx_codegen.so")) -or
            (Test-Path (Join-Path $d "libvyx_codegen.dylib"))) {
            return (Resolve-Path -LiteralPath $d).Path
        }
    }
    return ""
}

function Test-RuntimeLibrary {
    param([string]$Directory, [string]$Name)
    if ([string]::IsNullOrWhiteSpace($Directory) -or [string]::IsNullOrWhiteSpace($Name)) {
        return $false
    }
    foreach ($file in @(
        ($Name + ".lib"),
        ("lib" + $Name + ".a"),
        ("lib" + $Name + ".dll.a"),
        ("lib" + $Name + ".so"),
        ("lib" + $Name + ".dylib")
    )) {
        if (Test-Path -LiteralPath (Join-Path $Directory $file) -PathType Leaf) {
            return $true
        }
    }
    return $false
}

function Safe-Stem {
    param([string]$Text)
    return ($Text -replace '[\\/:*?"<>| ]', '_')
}

function Normalize-Text {
    param([string]$Text)
    if ($null -eq $Text) { return "" }
    $t = (($Text -replace "`r`n", "`n") -replace "`r", "`n").Trim()
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($t -split "`n")) {
        $trimmed = $line.TrimEnd()
        if ($trimmed.StartsWith("[CodeGen]")) { continue }
        if ($trimmed.StartsWith("[S3 solver]")) { continue }
        if ($trimmed.StartsWith("[S4 mono-sched]")) { continue }
        if ($trimmed.StartsWith("[S5 mono-sched]")) { continue }
        if ($trimmed -match "warning: implicit narrowing conversion") { continue }
        if ($trimmed -match "^\s*\|") { continue }
        if ($trimmed -match "^\s*\d+\s*\|") { continue }
        if ($trimmed -match "\^") { continue }
        if ($trimmed -ne "") { [void]$lines.Add($trimmed) }
    }
    return ($lines -join "`n").Trim()
}

function Normalize-ComparableText {
    param([string]$Text)
    $norm = Normalize-Text $Text
    if ([string]::IsNullOrWhiteSpace($norm)) { return "" }
    return (($norm -split "`n") | Sort-Object) -join "`n"
}

function Read-LogText {
    param([string]$PathValue)
    if ([string]::IsNullOrWhiteSpace($PathValue) -or -not (Test-Path -LiteralPath $PathValue)) { return "" }
    return [System.IO.File]::ReadAllText($PathValue)
}

$script:Results = New-Object System.Collections.Generic.List[object]

function Add-Result {
    param(
        [string]$Category,
        [string]$Name,
        [string]$Stage,
        [string]$Status,
        [int]$ExitCode,
        [bool]$TimedOut,
        [bool]$DialogCaught,
        [double]$ElapsedMs,
        [string]$Details,
        [string]$Log
    )
    [void]$script:Results.Add([pscustomobject]@{
        Category = $Category
        Name = $Name
        Stage = $Stage
        Status = $Status
        ExitCode = $ExitCode
        TimedOut = $TimedOut
        DialogCaught = $DialogCaught
        ElapsedMs = [Math]::Round($ElapsedMs, 3)
        Details = $Details
        Log = $Log
    })
    Write-Log ("[{0}] {1} {2}: {3} exit={4} {5}" -f $Category, $Stage, $Name, $Status, $ExitCode, $Details)
}

function Invoke-DiagnosticProcess {
    param(
        [string]$Category,
        [string]$Stage,
        [string]$Name,
        [string]$Exe,
        [string[]]$ArgList,
        [int]$TimeoutSec
    )
    $stem = Safe-Stem ($Category + "_" + $Stage + "_" + $Name)
    $stdout = Join-Path $OutDir ($stem + ".stdout.log")
    $stderr = Join-Path $OutDir ($stem + ".stderr.log")
    $dialog = Join-Path $OutDir ($stem + ".dialog.log")
    $cmdPath = Join-Path $OutDir ($stem + ".command.txt")
    ($Exe + " " + ($ArgList -join " ")) | Set-Content -Encoding utf8 -LiteralPath $cmdPath
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    if (Get-Command Invoke-VyxProcess -ErrorAction SilentlyContinue) {
        $run = Invoke-VyxProcess -FilePath $Exe `
            -ArgumentList $ArgList `
            -WorkingDirectory $repoRoot `
            -StdoutLog $stdout `
            -StderrLog $stderr `
            -DialogLog $dialog `
            -TimeoutSec $TimeoutSec
        $sw.Stop()
        return [pscustomobject]@{
            ExitCode = [int]$run.ExitCode
            TimedOut = [bool]$run.TimedOut
            DialogCaught = [bool]$run.DialogCaught
            ElapsedMs = $sw.Elapsed.TotalMilliseconds
            StdoutLog = $run.StdoutLog
            StderrLog = $run.StderrLog
            DialogLog = $run.DialogLog
            CommandLog = $cmdPath
            Stdout = Read-LogText $run.StdoutLog
            Stderr = Read-LogText $run.StderrLog
        }
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $argParts = @()
    foreach ($arg in $ArgList) {
        $escaped = ($arg -replace '"','\"')
        if ($arg -match '\s') { $argParts += '"' + $escaped + '"' }
        else { $argParts += $escaped }
    }
    $psi.Arguments = ($argParts -join " ")
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $repoRoot
    if (Get-Command Set-VyxProcessCrashSuppressionEnvironment -ErrorAction SilentlyContinue) {
        Set-VyxProcessCrashSuppressionEnvironment -ProcessStartInfo $psi
    } else {
        try { $psi.EnvironmentVariables["SEM_NOGPFAULTERRORBOX"] = "1" } catch {}
        try { $psi.EnvironmentVariables["__COMPAT_LAYER"] = "DisableWerUI" } catch {}
        try { $psi.EnvironmentVariables["VYX_DISABLE_WER_UI"] = "1" } catch {}
    }
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $finished = $proc.WaitForExit([Math]::Max(1, $TimeoutSec) * 1000)
    if (-not $finished) {
        try { $proc.Kill() } catch {}
        try { $proc.WaitForExit() } catch {}
    }
    $outText = $stdoutTask.GetAwaiter().GetResult()
    $errText = $stderrTask.GetAwaiter().GetResult()
    $sw.Stop()
    $outText | Set-Content -Encoding utf8 -LiteralPath $stdout
    $errText | Set-Content -Encoding utf8 -LiteralPath $stderr
    $exitCode = -901
    if ($finished) { $exitCode = $proc.ExitCode }
    return [pscustomobject]@{
        ExitCode = [int]$exitCode
        TimedOut = -not $finished
        DialogCaught = $false
        ElapsedMs = $sw.Elapsed.TotalMilliseconds
        StdoutLog = $stdout
        StderrLog = $stderr
        DialogLog = $dialog
        CommandLog = $cmdPath
        Stdout = $outText
        Stderr = $errText
    }
}

function Runtime-Args {
    if ([string]::IsNullOrWhiteSpace($script:RuntimePath)) { return @() }
    return @("-L", $script:RuntimePath, "-l", $script:RuntimeLibName)
}

function Test-StdTree {
    $packageRoot = Join-Path $repoRoot "bootstrap_compiler\std_packages"
    $seedStd = Join-Path $repoRoot "bootstrap_compiler\std"
    if (-not (Test-Path -LiteralPath $packageRoot -PathType Container) -or
        -not (Test-Path -LiteralPath $seedStd -PathType Container)) {
        Add-Result "std" "std_tree" "canonical_seed" "FAIL" 1 $false $false 0 "canonical package root or seed mirror missing" ""
        return
    }

    $layout = New-Object System.Collections.Generic.List[string]
    $sourceIndex = @{}
    $packageCount = 0
    $canonicalCount = 0
    foreach ($packageDir in @(Get-ChildItem -LiteralPath $packageRoot -Directory | Sort-Object Name)) {
        $manifest = Join-Path $packageDir.FullName "Vyx.toml"
        if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
            [void]$layout.Add("$($packageDir.Name):missing_manifest")
            continue
        }
        $packageCount++
        $raw = Get-Content -Raw -LiteralPath $manifest
        $isSourcePackage = $raw -match '(?m)^\s*type\s*=\s*"source"\s*(?:#.*)?$'
        $declared = New-Object System.Collections.Generic.HashSet[string] ([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($match in [regex]::Matches($raw, '"(src[/\\][^"\r\n]+\.vyx)"')) {
            [void]$declared.Add($match.Groups[1].Value.Replace('/', [System.IO.Path]::DirectorySeparatorChar))
        }
        if ($declared.Count -eq 0) {
            [void]$layout.Add("$($packageDir.Name):no_declared_source")
            continue
        }
        foreach ($relative in @($declared)) {
            $absolute = Join-Path $packageDir.FullName $relative
            if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) {
                [void]$layout.Add("$($packageDir.Name):missing:$relative")
                continue
            }
            $canonicalCount++
            if ($isSourcePackage) {
                $name = [System.IO.Path]::GetFileName($relative)
                if ($sourceIndex.ContainsKey($name)) {
                    [void]$layout.Add("duplicate:$name")
                } else {
                    $sourceIndex[$name] = $absolute
                }
            }
        }
        $srcRoot = Join-Path $packageDir.FullName "src"
        if (-not (Test-Path -LiteralPath $srcRoot -PathType Container)) {
            [void]$layout.Add("$($packageDir.Name):missing_src_dir")
            continue
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $srcRoot -Recurse -File -Filter "*.vyx")) {
            $relative = $file.FullName.Substring($packageDir.FullName.Length + 1)
            if (-not $declared.Contains($relative)) {
                [void]$layout.Add("$($packageDir.Name):undeclared:$relative")
            }
        }
    }

    function Get-StdMirrorText {
        param([string]$Path)
        $rawText = ([System.IO.File]::ReadAllText($Path) -replace "`r`n", "`n") -replace "`r", "`n"
        [string[]]$lines = @($rawText -split "`n" | ForEach-Object { $_ -replace '[ \t]+$', '' })
        $last = $lines.Count - 1
        while ($last -ge 0 -and $lines[$last].Length -eq 0) { $last-- }
        if ($last -lt 0) { return "" }
        return (($lines[0..$last]) -join "`n")
    }

    $missing = New-Object System.Collections.Generic.List[string]
    $diff = New-Object System.Collections.Generic.List[string]
    foreach ($name in @($sourceIndex.Keys | Sort-Object)) {
        $mirror = Join-Path $seedStd $name
        if (-not (Test-Path -LiteralPath $mirror -PathType Leaf)) {
            [void]$missing.Add($name)
        } elseif ((Get-StdMirrorText $sourceIndex[$name]) -cne (Get-StdMirrorText $mirror)) {
            [void]$diff.Add($name)
        }
    }

    $seedOnly = @(
        "cacao.vyx", "curl.vyx", "dll.vyx", "grpc.vyx", "httpclient.vyx",
        "llvm.vyx", "openssl.vyx", "sqlite.vyx", "stb_image.vyx",
        "websocket.vyx", "win32.vyx"
    )
    $unclassified = New-Object System.Collections.Generic.List[string]
    foreach ($file in @(Get-ChildItem -LiteralPath $seedStd -File -Filter "*.vyx")) {
        if (-not $sourceIndex.ContainsKey($file.Name) -and $file.Name -notin $seedOnly) {
            [void]$unclassified.Add($file.Name)
        }
    }

    $status = "PASS"
    $exit = 0
    if ($layout.Count -gt 0 -or $missing.Count -gt 0 -or
        $diff.Count -gt 0 -or $unclassified.Count -gt 0) {
        $status = "FAIL"
        $exit = 1
    }
    $detail = "packages={0} canonical={1} seed_required={2} layout={3} missing={4} diff={5} unclassified={6}" -f `
        $packageCount, $canonicalCount, $sourceIndex.Count, $layout.Count, `
        $missing.Count, $diff.Count, $unclassified.Count
    Add-Result "std" "std_tree" "canonical_seed" $status $exit $false $false 0 $detail ""
}

function Invoke-CliMatrix {
    param([string]$CompilerName, [string]$CompilerPath, [string]$InputFile)
    if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
        Add-Result "cli" $CompilerName "resolve" "SKIP" 0 $false $false 0 "compiler not available" ""
        return
    }
    $rtArgs = Runtime-Args
    $irOut = Join-Path $OutDir ($CompilerName + "_cli_emit.ll")
    $objOut = Join-Path $OutDir ($CompilerName + "_cli_emit.obj")
    $exeOut = Join-Path $OutDir ($CompilerName + "_cli_run.exe")
    $isBootstrap = ($CompilerName -eq "bootstrap")
    $dumpTokensArgs = if ($isBootstrap) { @("--src=file", $InputFile, "--dump-tokens") } else { @("--dump-tokens", $InputFile) }
    $parseArgs = if ($isBootstrap) { @("--src=file", $InputFile, "--stop-after-parse") } else { @("--stop-after-parse", $InputFile) }
    $semaArgs = if ($isBootstrap) { @("--src=file", $InputFile, "--stop-after-sema") } else { @("--stop-after-sema", $InputFile) }
    $emitIrArgs = if ($isBootstrap) { @("--src=file", $InputFile, "--emit=ir", "-o", $irOut) } else { @("--emit-ir", $InputFile, "-o", $irOut) }
    $emitObjArgs = if ($isBootstrap) { @("--src=file", $InputFile, "--emit=obj", "-o", $objOut) } else { @("--emit-obj", $InputFile, "-o", $objOut) }
    $runArgs = if ($isBootstrap) { @("--src=file", $InputFile, "--run=aot", "-o", $exeOut) } else { @("--run", $InputFile, "-o", $exeOut) }
    $cases = @(
        [pscustomobject]@{ Name = "usage_no_args"; ArgList = @(); Expect = 1; NeedOutput = "Usage:" },
        [pscustomobject]@{ Name = "unknown_option"; ArgList = @("--definitely-unknown"); Expect = 1; NeedOutput = "unknown option" },
        [pscustomobject]@{ Name = "dump_tokens"; ArgList = $dumpTokensArgs; Expect = 0; NeedOutput = "" },
        [pscustomobject]@{ Name = "stop_after_parse"; ArgList = $parseArgs; Expect = 0; NeedOutput = "" },
        [pscustomobject]@{ Name = "stop_after_sema"; ArgList = $semaArgs + $rtArgs; Expect = 0; NeedOutput = "" },
        [pscustomobject]@{ Name = "emit_ir"; ArgList = $emitIrArgs + $rtArgs; Expect = 0; NeedOutput = "" },
        [pscustomobject]@{ Name = "emit_obj"; ArgList = $emitObjArgs + $rtArgs; Expect = 0; NeedOutput = "" },
        [pscustomobject]@{ Name = "run"; ArgList = $runArgs + $rtArgs; Expect = 0; NeedOutput = "" }
    )
    if ($isBootstrap) {
        $cases += @(
            [pscustomobject]@{ Name = "removed_use_new_mir_codegen"; ArgList = @("--use-new-mir-codegen"); Expect = 1; NeedOutput = "unknown option" },
            [pscustomobject]@{ Name = "removed_emit_ir"; ArgList = @("--emit-ir"); Expect = 1; NeedOutput = "unknown option" },
            [pscustomobject]@{ Name = "removed_emit_cpp"; ArgList = @("--emit-cpp"); Expect = 1; NeedOutput = "unknown option" },
            [pscustomobject]@{ Name = "removed_src"; ArgList = @("--src"); Expect = 1; NeedOutput = "unknown option" },
            [pscustomobject]@{ Name = "removed_run"; ArgList = @("--run"); Expect = 1; NeedOutput = "unknown option" }
        )
    }
    foreach ($c in $cases) {
        $run = Invoke-DiagnosticProcess "cli" $CompilerName $c.Name $CompilerPath ([string[]]$c.ArgList) $RunTimeoutSec
        $combined = ($run.Stdout + "`n" + $run.Stderr)
        $ok = ($run.ExitCode -eq $c.Expect -and -not $run.TimedOut -and -not $run.DialogCaught)
        if ($ok -and -not [string]::IsNullOrWhiteSpace($c.NeedOutput)) {
            $ok = ($combined.IndexOf($c.NeedOutput, [StringComparison]::OrdinalIgnoreCase) -ge 0)
        }
        $details = "expected_exit=" + $c.Expect
        if (-not [string]::IsNullOrWhiteSpace($c.NeedOutput)) { $details += " contains=" + $c.NeedOutput }
        Add-Result "cli" $c.Name $CompilerName ($(if ($ok) { "PASS" } else { "FAIL" })) $run.ExitCode $run.TimedOut $run.DialogCaught $run.ElapsedMs $details $run.StdoutLog
    }
}

function Invoke-PositiveMatrix {
    param([object[]]$Cases)
    foreach ($case in $Cases) {
        $path = Join-Path $repoRoot $case.Path
        if (-not (Test-Path -LiteralPath $path)) {
            Add-Result "positive" $case.Path "resolve" "FAIL" 1 $false $false 0 "missing test file" ""
            continue
        }
        $rtArgs = Runtime-Args
        $hostRun = $null
        $bootRun = $null
        if (-not [string]::IsNullOrWhiteSpace($script:HostPath)) {
            $hostOut = Join-Path $OutDir ("host_" + (Safe-Stem $case.Path) + ".exe")
            $hostRun = Invoke-DiagnosticProcess "positive" "host" $case.Path $script:HostPath ([string[]](@("--run", $path, "-o", $hostOut) + $rtArgs)) $RunTimeoutSec
            $hostOk = ($hostRun.ExitCode -eq 0 -and -not $hostRun.TimedOut -and -not $hostRun.DialogCaught)
            Add-Result "positive" $case.Path "host" ($(if ($hostOk) { "PASS" } else { "FAIL" })) $hostRun.ExitCode $hostRun.TimedOut $hostRun.DialogCaught $hostRun.ElapsedMs $case.Description $hostRun.StdoutLog
        }
        if (-not [string]::IsNullOrWhiteSpace($script:BootPath)) {
            $bootOut = Join-Path $OutDir ("bootstrap_" + (Safe-Stem $case.Path) + ".exe")
            $bootRun = Invoke-DiagnosticProcess "positive" "bootstrap" $case.Path $script:BootPath ([string[]](@("--src=file", $path, "--run=aot", "-o", $bootOut) + $rtArgs)) $RunTimeoutSec
            $bootOk = ($bootRun.ExitCode -eq 0 -and -not $bootRun.TimedOut -and -not $bootRun.DialogCaught)
            Add-Result "positive" $case.Path "bootstrap" ($(if ($bootOk) { "PASS" } else { "FAIL" })) $bootRun.ExitCode $bootRun.TimedOut $bootRun.DialogCaught $bootRun.ElapsedMs $case.Description $bootRun.StdoutLog
        }
        if ($hostRun -and $bootRun -and $hostRun.ExitCode -eq 0 -and $bootRun.ExitCode -eq 0) {
            $hn = Normalize-ComparableText $hostRun.Stdout
            $bn = Normalize-ComparableText $bootRun.Stdout
            $same = ($hn -eq $bn)
            Add-Result "positive" $case.Path "stdout_diff" ($(if ($same) { "PASS" } else { "FAIL" })) 0 $false $false 0 "normalized host/bootstrap stdout equality" ""
        }
    }
}

function Invoke-NegativeMatrix {
    param([object[]]$Cases)
    foreach ($case in $Cases) {
        $path = Join-Path $repoRoot $case.Path
        if (-not (Test-Path -LiteralPath $path)) {
            Add-Result "negative" $case.Path "resolve" "FAIL" 1 $false $false 0 "missing test file" ""
            continue
        }
        $rtArgs = Runtime-Args
        $hostRun = $null
        $bootRun = $null
        if (-not [string]::IsNullOrWhiteSpace($script:HostPath)) {
            $hostRun = Invoke-DiagnosticProcess "negative" "host" $case.Path $script:HostPath ([string[]](@("--run", $path) + $rtArgs)) $RunTimeoutSec
            $hostOk = ($hostRun.ExitCode -eq 1 -and -not $hostRun.TimedOut -and -not $hostRun.DialogCaught)
            Add-Result "negative" $case.Path "host" ($(if ($hostOk) { "PASS" } else { "FAIL" })) $hostRun.ExitCode $hostRun.TimedOut $hostRun.DialogCaught $hostRun.ElapsedMs $case.Description $hostRun.StderrLog
        }
        if (-not [string]::IsNullOrWhiteSpace($script:BootPath)) {
            $bootRun = Invoke-DiagnosticProcess "negative" "bootstrap" $case.Path $script:BootPath ([string[]](@("--src=file", $path, "--run=aot") + $rtArgs)) $RunTimeoutSec
            $bootOk = ($bootRun.ExitCode -eq 1 -and -not $bootRun.TimedOut -and -not $bootRun.DialogCaught)
            Add-Result "negative" $case.Path "bootstrap" ($(if ($bootOk) { "PASS" } else { "FAIL" })) $bootRun.ExitCode $bootRun.TimedOut $bootRun.DialogCaught $bootRun.ElapsedMs $case.Description $bootRun.StderrLog
        }
    }
}

function Invoke-Stability {
    param([object[]]$Cases)
    if ([string]::IsNullOrWhiteSpace($script:BootPath)) {
        Add-Result "stability" "bootstrap" "resolve" "SKIP" 0 $false $false 0 "bootstrap compiler not available" ""
        return
    }
    foreach ($case in $Cases) {
        $path = Join-Path $repoRoot $case.Path
        if (-not (Test-Path -LiteralPath $path)) {
            Add-Result "stability" $case.Path "resolve" "FAIL" 1 $false $false 0 "missing test file" ""
            continue
        }
        foreach ($mode in @("dump_tokens", "emit_ir")) {
            $bad = 0
            $codes = @{}
            $firstNorm = ""
            $drift = 0
            for ($i = 0; $i -lt $StabilityReps; $i++) {
                $probeArgs = @()
                if ($mode -eq "dump_tokens") {
                    $probeArgs = @("--src=file", $path, "--dump-tokens")
                } else {
                    $outLl = Join-Path $OutDir ("stability_" + (Safe-Stem $case.Path) + "_$i.ll")
                    $probeArgs = @("--src=file", $path, "--emit=ir", "-o", $outLl) + (Runtime-Args)
                }
                $run = Invoke-DiagnosticProcess "stability" $mode ($case.Path + "_$i") $script:BootPath ([string[]]$probeArgs) $RunTimeoutSec
                if (-not $codes.ContainsKey($run.ExitCode)) { $codes[$run.ExitCode] = 0 }
                $codes[$run.ExitCode] = $codes[$run.ExitCode] + 1
                if ($run.ExitCode -ne 0 -or $run.TimedOut -or $run.DialogCaught) { $bad++ }
                $norm = Normalize-ComparableText $run.Stdout
                if ($i -eq 0) { $firstNorm = $norm }
                elseif ($mode -eq "dump_tokens" -and $norm -ne $firstNorm) { $drift++ }
            }
            $parts = @()
            foreach ($k in ($codes.Keys | Sort-Object)) { $parts += ("{0}:{1}" -f $k, $codes[$k]) }
            $ok = ($bad -eq 0 -and $drift -eq 0)
            Add-Result "stability" $case.Path $mode ($(if ($ok) { "PASS" } else { "FAIL" })) $bad $false $false 0 ("reps={0} bad={1} drift={2} codes={3}" -f $StabilityReps, $bad, $drift, ($parts -join ",")) ""
        }
    }
}

function Invoke-Fixpoint {
    if ($SkipFixpoint) {
        Add-Result "fixpoint" "selfhost" "test_selfhost_fixpoint" "SKIP" 0 $false $false 0 "SkipFixpoint set" ""
        return
    }
    # The diagnostics check already owns the diagnostic sweep.  Invoke the
    # maintained strict compiler fixed-point gate directly instead of routing
    # through the old wrapper that duplicated this script's work.
    $scriptPath = Join-Path $repoRoot "bootstrap_compiler\scripts\test_selfhost_fixpoint.ps1"
    if (-not (Test-Path -LiteralPath $scriptPath)) {
        Add-Result "fixpoint" "selfhost" "test_selfhost_fixpoint" "FAIL" 1 $false $false 0 "missing test_selfhost_fixpoint.ps1" ""
        return
    }
    $run = Invoke-DiagnosticProcess "fixpoint" "selfhost" "test_selfhost_fixpoint" "powershell" ([string[]]@("-ExecutionPolicy", "Bypass", "-File", $scriptPath, "-SkipBuild", "-SkipSamples", "-TimeoutSec", $FixpointTimeoutSec.ToString())) ([Math]::Max($FixpointTimeoutSec + 60, 120))
    $ok = ($run.ExitCode -eq 0 -and -not $run.TimedOut -and -not $run.DialogCaught)
    Add-Result "fixpoint" "selfhost" "test_selfhost_fixpoint" ($(if ($ok) { "PASS" } else { "FAIL" })) $run.ExitCode $run.TimedOut $run.DialogCaught $run.ElapsedMs "A->B->C IR fixpoint" $run.StdoutLog
}

function Invoke-FullSweep {
    if ($SkipFullSweep) {
        Add-Result "full_sweep" "bootstrap" "run_all_modules" "SKIP" 0 $false $false 0 "SkipFullSweep set" ""
        return
    }
    if ([string]::IsNullOrWhiteSpace($script:BootPath)) {
        Add-Result "full_sweep" "bootstrap" "run_all_modules" "SKIP" 0 $false $false 0 "bootstrap compiler not available" ""
        return
    }
    $runner = Join-Path $repoRoot "tests\run_all_modules.ps1"
    $bootLog = Join-Path $OutDir "run_all_modules_bootstrap.log"
    $sweepArgs = @("-ExecutionPolicy", "Bypass", "-File", $runner,
                   "-BootstrapOnly",
                   "-BootstrapCompiler", $script:BootPath,
                   "-RuntimeDir", $script:RuntimePath,
                   "-RuntimeLib", $script:RuntimeLibName,
                   "-RunTimeoutSec", $RunTimeoutSec.ToString(),
                   "-LogPath", $bootLog)
    $run = Invoke-DiagnosticProcess "full_sweep" "bootstrap" "run_all_modules" "powershell" ([string[]]$sweepArgs) 3600
    $ok = ($run.ExitCode -eq 0 -and -not $run.TimedOut -and -not $run.DialogCaught)
    Add-Result "full_sweep" "bootstrap" "run_all_modules" ($(if ($ok) { "PASS" } else { "FAIL" })) $run.ExitCode $run.TimedOut $run.DialogCaught $run.ElapsedMs ("log=" + $bootLog) $run.StdoutLog

    if ($FullHostSweep -and -not [string]::IsNullOrWhiteSpace($script:HostPath)) {
        $hostLog = Join-Path $OutDir "run_all_modules_host.log"
        $hostArgs = @("-ExecutionPolicy", "Bypass", "-File", $runner,
                      "-HostOnly",
                      "-HostCompiler", $script:HostPath,
                      "-RuntimeDir", $script:RuntimePath,
                      "-RuntimeLib", $script:RuntimeLibName,
                      "-RunTimeoutSec", $RunTimeoutSec.ToString(),
                      "-LogPath", $hostLog)
        $hostRun = Invoke-DiagnosticProcess "full_sweep" "host" "run_all_modules" "powershell" ([string[]]$hostArgs) 3600
        $hostOk = ($hostRun.ExitCode -eq 0 -and -not $hostRun.TimedOut -and -not $hostRun.DialogCaught)
        Add-Result "full_sweep" "host" "run_all_modules" ($(if ($hostOk) { "PASS" } else { "FAIL" })) $hostRun.ExitCode $hostRun.TimedOut $hostRun.DialogCaught $hostRun.ElapsedMs ("log=" + $hostLog) $hostRun.StdoutLog
    }
}

function Invoke-AsanProbe {
    if ($SkipAsan) {
        Add-Result "asan" "bootstrap" "compile_and_emit" "SKIP" 0 $false $false 0 "SkipAsan set" ""
        return
    }
    $llvmToolCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:LLVM_BIN)) {
        $llvmToolCandidates += (Join-Path $env:LLVM_BIN "clang++.exe")
        $llvmToolCandidates += (Join-Path $env:LLVM_BIN "clang++")
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LLVM_ROOT)) {
        $llvmToolCandidates += (Join-Path $env:LLVM_ROOT "bin\clang++.exe")
        $llvmToolCandidates += (Join-Path $env:LLVM_ROOT "bin\clang++")
    }
    $llvmToolCandidates += "clang++"
    $clang = Resolve-Tool "" $llvmToolCandidates "clang++"
    $fp = Join-Path $repoRoot "bootstrap_compiler\out\selfhost_fixpoint\fixpoint\fp_b.ll"
    if ([string]::IsNullOrWhiteSpace($clang) -or -not (Test-Path -LiteralPath $fp)) {
        Add-Result "asan" "bootstrap" "compile" "SKIP" 0 $false $false 0 "clang++ or fp_b.ll unavailable" ""
        return
    }
    $asanExe = Join-Path $OutDir "boot_b_asan.exe"
    $build = Invoke-DiagnosticProcess "asan" "clang" "boot_b" $clang ([string[]]@("-fsanitize=address", $fp, "-o", $asanExe, "-L", $script:RuntimePath, ("-l" + $script:RuntimeLibName))) 300
    $buildOk = ($build.ExitCode -eq 0)
    Add-Result "asan" "boot_b" "compile" ($(if ($buildOk) { "PASS" } else { "FAIL" })) $build.ExitCode $build.TimedOut $build.DialogCaught $build.ElapsedMs "compile selfhost IR with ASan" $build.StdoutLog
    if (-not $buildOk) { return }
    $oldPath = $env:Path
    $pathPrefix = $script:RuntimePath
    if (-not [string]::IsNullOrWhiteSpace($env:LLVM_ROOT)) {
        $clangRuntimeRoot = Join-Path $env:LLVM_ROOT "lib\clang"
        if (Test-Path -LiteralPath $clangRuntimeRoot) {
            $runtimeDirs = @(Get-ChildItem -LiteralPath $clangRuntimeRoot -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName "lib\windows" } |
                Where-Object { Test-Path -LiteralPath $_ })
            if ($runtimeDirs.Count -gt 0) {
                $pathPrefix = $runtimeDirs[0] + ";" + $pathPrefix
            }
        }
        $llvmBin = Join-Path $env:LLVM_ROOT "bin"
        if (Test-Path -LiteralPath $llvmBin) {
            $pathPrefix = $llvmBin + ";" + $pathPrefix
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LLVM_BIN) -and (Test-Path -LiteralPath $env:LLVM_BIN)) {
        $pathPrefix = $env:LLVM_BIN + ";" + $pathPrefix
    }
    $env:Path = $pathPrefix + ";" + $env:Path
    $oldAsan = $env:ASAN_OPTIONS
    $env:ASAN_OPTIONS = "halt_on_error=1:symbolize=1:strict_string_checks=1"
    try {
        $probe = Join-Path $repoRoot "tests\cases\stress\test_match_pattern.vyx"
        $outLl = Join-Path $OutDir "asan_match_pattern.ll"
        $run = Invoke-DiagnosticProcess "asan" "boot_b" "emit_match_pattern" $asanExe ([string[]]@("--src=file", $probe, "--emit=ir", "-o", $outLl, "-L", $script:RuntimePath, "-l", $script:RuntimeLibName)) 300
        $ok = ($run.ExitCode -eq 0 -and -not $run.TimedOut -and -not $run.DialogCaught)
        Add-Result "asan" "emit_match_pattern" "boot_b" ($(if ($ok) { "PASS" } else { "FAIL" })) $run.ExitCode $run.TimedOut $run.DialogCaught $run.ElapsedMs "ASan selfhost compiler emit probe" $run.StdoutLog
    } finally {
        $env:Path = $oldPath
        $env:ASAN_OPTIONS = $oldAsan
    }
}

$script:HostPath = ""
if (-not $SelfhostOnly) {
    $script:HostPath = Resolve-Tool $HostCompiler @(
        (Join-Path $repoRoot "build_yolo_vyxcg\vyxc.exe"),
        (Join-Path $repoRoot "build_yolo_vyxcg_release\vyxc.exe"),
        (Join-Path $repoRoot "cmake-build-debug\vyxc.exe"),
        (Join-Path $repoRoot "build\vyxc.exe")
    ) "host vyxc"
}

$script:BootPath = Resolve-Tool $BootstrapCompiler @(
    (Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"),
    (Join-Path $repoRoot "bootstrap_compiler\boot_b.exe"),
    (Join-Path $repoRoot "bootstrap_compiler\boot_c.exe"),
    (Join-Path $repoRoot "bootstrap_compiler\boot_a.exe")
) "bootstrap compiler"

$script:RuntimePath = Resolve-Runtime $RuntimeDir
$script:RuntimeLibName = $RuntimeLib
if (-not [string]::IsNullOrWhiteSpace($script:RuntimePath) -and
    -not (Test-RuntimeLibrary $script:RuntimePath $script:RuntimeLibName)) {
    foreach ($candidate in @("vyx_runtime", "vyx_rt", "vyx_codegen")) {
        if (Test-RuntimeLibrary $script:RuntimePath $candidate) {
            $script:RuntimeLibName = $candidate
            break
        }
    }
}
if (-not [string]::IsNullOrWhiteSpace($script:RuntimePath)) {
    $env:Path = $script:RuntimePath + ";" + $env:Path
}

Write-Log "repo: $repoRoot"
Write-Log "out:  $OutDir"
Write-Log "host: $script:HostPath"
Write-Log "boot: $script:BootPath"
Write-Log "rt:   $script:RuntimePath / $script:RuntimeLibName"

if ($SelfhostOnly) {
    Add-Result "resolve" "host" "compiler" "SKIP" 0 $false $false 0 "SelfhostOnly set" ""
} elseif ([string]::IsNullOrWhiteSpace($script:HostPath)) {
    Add-Result "resolve" "host" "compiler" "SKIP" 0 $false $false 0 "host compiler unavailable" ""
}
if ([string]::IsNullOrWhiteSpace($script:BootPath)) {
    Add-Result "resolve" "bootstrap" "compiler" "FAIL" 1 $false $false 0 "bootstrap compiler unavailable" ""
}
if ([string]::IsNullOrWhiteSpace($script:RuntimePath)) {
    Add-Result "resolve" "runtime" $script:RuntimeLibName "FAIL" 1 $false $false 0 "runtime library unavailable" ""
}

if (-not [System.IO.Path]::IsPathRooted($CasesCsv)) {
    $CasesCsv = Join-Path $repoRoot $CasesCsv
}
if (-not (Test-Path -LiteralPath $CasesCsv)) {
    throw "CasesCsv not found: $CasesCsv"
}
$cases = @(Import-Csv -LiteralPath $CasesCsv)
$positive = @($cases | Where-Object { $_.Kind -eq "positive" })
$negative = @($cases | Where-Object { $_.Kind -eq "negative" })
$stability = @($cases | Where-Object { $_.Kind -eq "stability" })
$cliInput = @($cases | Where-Object { $_.Kind -eq "cli" } | Select-Object -First 1)
if ($cliInput.Count -eq 0) { throw "CasesCsv has no cli row." }
$cliPath = Join-Path $repoRoot $cliInput[0].Path

if ($SkipStdTree) {
    Add-Result "std" "std_tree" "sha256" "SKIP" 0 $false $false 0 "SkipStdTree set" ""
} else {
    Test-StdTree
}
if (-not $SelfhostOnly) {
    Invoke-CliMatrix "host" $script:HostPath $cliPath
}
Invoke-CliMatrix "bootstrap" $script:BootPath $cliPath
Invoke-PositiveMatrix $positive
Invoke-NegativeMatrix $negative
Invoke-Stability $stability
Invoke-Fixpoint
Invoke-AsanProbe
Invoke-FullSweep

$script:Results | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $csvPath
$failCount = @($script:Results | Where-Object { $_.Status -eq "FAIL" }).Count
$skipCount = @($script:Results | Where-Object { $_.Status -eq "SKIP" }).Count
$passCount = @($script:Results | Where-Object { $_.Status -eq "PASS" }).Count
$summary = [pscustomobject]@{
    repo = $repoRoot
    out_dir = $OutDir
    host_compiler = $script:HostPath
    bootstrap_compiler = $script:BootPath
    runtime_dir = $script:RuntimePath
    pass = $passCount
    fail = $failCount
    skip = $skipCount
    results_csv = $csvPath
    log = $logPath
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 -LiteralPath $jsonPath
Write-Log ""
Write-Log ("summary: pass={0} fail={1} skip={2}" -f $passCount, $failCount, $skipCount)
Write-Log ("results: " + $csvPath)
Write-Log ("summary_json: " + $jsonPath)

if ($failCount -gt 0) { exit 1 }
exit 0

param(
    [int]$Iterations = 5,
    [int]$Warmups = 1,
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount),
    [string]$HostVyxc = "",
    [string]$BootstrapVyxc = "",
    [string]$Clangxx = "clang++",
    [string]$OptFlag = "-O2",
    [string]$RuntimeDir = "",
    [string]$RuntimeLib = "vyx_runtime",
    [string]$OutDir = "",
    [string[]]$Workloads = @(),
    [switch]$SkipCppLto,
    [switch]$SkipHost,
    [switch]$SkipBootstrap,
    [switch]$SkipCpp26,
    [int]$RuntimeWarmups = 1,
    [int]$RuntimeRuns = 3,
    [int]$RunTimeoutSec = 60,
    [switch]$SkipRuntime,
    [switch]$RequireSemanticMatch
)

$ErrorActionPreference = "Stop"

$benchRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $benchRoot
if ($OutDir.Length -eq 0) { $OutDir = Join-Path $benchRoot "out" }
elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $OutDir))
}
$workRoot = Join-Path $OutDir "work"
$logRoot = Join-Path $OutDir "logs"
$csvPath = Join-Path $OutDir "results.csv"
$jsonlPath = Join-Path $OutDir "results.jsonl"
$runtimeCsvPath = Join-Path $OutDir "runtime_results.csv"
$runtimeJsonlPath = Join-Path $OutDir "runtime_results.jsonl"
$runtimeSummaryPath = Join-Path $OutDir "runtime_summary.csv"
$runtimeComparisonPath = Join-Path $OutDir "runtime_comparison_summary.csv"
$runtimeSemanticPath = Join-Path $OutDir "runtime_semantic_summary.csv"

function Resolve-BenchPath([string]$Path, [string]$Base) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return [System.IO.Path]::GetFullPath((Join-Path $Base $Path))
}

if ($HostVyxc.Length -eq 0) {
    $candidatesHost = @(
        (Join-Path $repoRoot "build_yolo_vyxcg/vyxc.exe"),
        (Join-Path $repoRoot "cmake-build-debug/vyxc.exe"),
        (Join-Path $repoRoot "build/vyxc.exe")
    )
    foreach ($c in $candidatesHost) { if (Test-Path $c) { $HostVyxc = $c; break } }
    if ($HostVyxc.Length -eq 0) { $HostVyxc = $candidatesHost[0] }
}
$HostVyxc = Resolve-BenchPath $HostVyxc $repoRoot

if ($BootstrapVyxc.Length -eq 0) {
    $candidates = @(
        (Join-Path $repoRoot "bootstrap_compiler/out/boot.exe"),
        (Join-Path $repoRoot "bootstrap_compiler/boot.exe"),
        (Join-Path $repoRoot "bootstrap_compiler/boot_a.exe"),
        (Join-Path $repoRoot "bootstrap_compiler/target/boot_d.exe")
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $BootstrapVyxc = $c; break } }
    if ($BootstrapVyxc.Length -eq 0) { $BootstrapVyxc = $candidates[0] }
}
$BootstrapVyxc = Resolve-BenchPath $BootstrapVyxc $repoRoot

if (-not $SkipCpp26) {
    $clangCommand = Get-Command $Clangxx -CommandType Application -ErrorAction Stop
    $Clangxx = $clangCommand.Source
}

function Resolve-VyxRuntimePath() {
    if ($RuntimeDir.Length -gt 0) {
        $resolved = Resolve-BenchPath $RuntimeDir $repoRoot
        if (-not (Test-Path $resolved)) { throw "RuntimeDir not found: $resolved" }
        return [System.IO.Path]::GetFullPath($resolved)
    }

    $runtimeFiles = @(
        ($RuntimeLib + ".lib"),
        ("lib" + $RuntimeLib + ".a"),
        ($RuntimeLib + ".dll"),
        ("lib" + $RuntimeLib + ".so"),
        ("lib" + $RuntimeLib + ".dylib")
    )
    $candidates = @(
        (Join-Path $repoRoot "bootstrap_compiler/out"),
        (Join-Path $repoRoot "bootstrap_compiler"),
        (Join-Path $repoRoot "cmake-build-debug")
    )
    foreach ($candidate in $candidates) {
        if (-not (Test-Path $candidate)) { continue }
        foreach ($runtimeFile in $runtimeFiles) {
            if (Test-Path (Join-Path $candidate $runtimeFile)) {
                return [System.IO.Path]::GetFullPath($candidate)
            }
        }
        if (Test-Path (Join-Path $candidate "vyx_codegen.dll")) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    return ""
}

$VyxRuntimePath = Resolve-VyxRuntimePath

function Write-ProgressLine([string]$Message) {
    Write-Host $Message
    [Console]::Out.Flush()
}

function Reset-Dir([string]$Path) {
    if (Test-Path $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Copy-Workload([string]$Source, [string]$Destination) {
    Reset-Dir $Destination
    Copy-Item -Path (Join-Path $Source "*") -Destination $Destination -Recurse -Force
}

function Touch-Append([string]$Path, [string]$Text) {
    Add-Content -LiteralPath $Path -Value $Text
}

function Quote-Arg([string]$Arg) {
    if ($Arg -notmatch '[\s"]') { return $Arg }
    return '"' + $Arg.Replace('"', '\"') + '"'
}

function Join-PathList([string[]]$Paths) {
    return (($Paths | Where-Object { $_ -ne $null -and $_.Length -gt 0 } | Select-Object -Unique) -join [System.IO.Path]::PathSeparator)
}

function Invoke-Captured([string]$FilePath, [string[]]$ArgumentList, [string]$WorkingDirectory, [string]$LogPath, [int]$TimeoutSec = 0, [string]$ExtraPath = "") {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FilePath
    $psi.Arguments = ($ArgumentList | ForEach-Object { Quote-Arg $_ }) -join ' '
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    if ($ExtraPath.Length -gt 0) {
        $pathKey = "PATH"
        if ($psi.EnvironmentVariables.ContainsKey("Path")) { $pathKey = "Path" }
        $psi.EnvironmentVariables[$pathKey] = $ExtraPath + [System.IO.Path]::PathSeparator + $psi.EnvironmentVariables[$pathKey]
    }
    $proc = [System.Diagnostics.Process]::new()
    $proc.StartInfo = $psi
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$proc.Start()
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $timedOut = $false
    if ($TimeoutSec -gt 0) {
        if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
            $timedOut = $true
            try { $proc.Kill() } catch {}
            $proc.WaitForExit()
        }
    } else {
        $proc.WaitForExit()
    }
    $sw.Stop()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    Set-Content -LiteralPath $LogPath -Value ($stdout + $stderr)
    $exit = $proc.ExitCode
    if ($timedOut) { $exit = -2147483648 }
    return [pscustomobject]@{
        ExitCode = $exit
        ElapsedMs = $sw.Elapsed.TotalMilliseconds
        Stdout = $stdout
        Stderr = $stderr
        TimedOut = $timedOut
    }
}

function Needs-Rebuild([string]$Source, [string]$Object, [string]$Header) {
    if (-not (Test-Path $Object)) { return $true }
    $objTime = (Get-Item -LiteralPath $Object).LastWriteTimeUtc
    if ((Get-Item -LiteralPath $Source).LastWriteTimeUtc -gt $objTime) { return $true }
    if ((Get-Item -LiteralPath $Header).LastWriteTimeUtc -gt $objTime) { return $true }
    return $false
}

function Needs-Rebuild-Cpp([string]$Source, [string]$Object, [string]$Header) {
    if (-not (Test-Path $Object)) { return $true }
    $objTime = (Get-Item -LiteralPath $Object).LastWriteTimeUtc
    if ((Get-Item -LiteralPath $Source).LastWriteTimeUtc -gt $objTime) { return $true }
    if ((Test-Path $Header) -and (Get-Item -LiteralPath $Header).LastWriteTimeUtc -gt $objTime) { return $true }
    return $false
}

function Invoke-Cpp26Build([string]$Workspace, [string]$LogPath) {
    $cache = Join-Path $Workspace ".cache"
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $srcDir = Join-Path $Workspace "src"
    $includeDir = Join-Path $Workspace "include"
    $header = Join-Path $includeDir "work.hpp"
    $exe = Join-Path $Workspace "target/cpp26_bench.exe"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $exe) | Out-Null

    $commands = [System.Collections.Generic.List[string]]::new()
    $objects = [System.Collections.Generic.List[string]]::new()
    $cppOptFlag = "-O2"
    if ($OptFlag.Length -gt 0) { $cppOptFlag = $OptFlag }
    $cppCompileFlags = $cppOptFlag
    $cppLinkFlags = $cppOptFlag
    if (-not $SkipCppLto) {
        $cppCompileFlags += " -flto"
        $cppLinkFlags += " -flto -fuse-ld=lld"
    }
    foreach ($source in @(Get-ChildItem -LiteralPath $srcDir -Filter "*.cpp" | Sort-Object Name)) {
        $obj = Join-Path $cache ($source.BaseName + ".obj")
        [void]$objects.Add($obj)
        $includeArg = ""
        if (Test-Path $includeDir) { $includeArg = " -I `"$includeDir`"" }
        if (Needs-Rebuild-Cpp $source.FullName $obj $header) {
            $commands.Add("& `"$Clangxx`" -std=c++26 $cppCompileFlags$includeArg -c `"$($source.FullName)`" -o `"$obj`"")
        }
    }
    $linkedObjects = ($objects.ToArray() | ForEach-Object { "`"$_`"" }) -join " "
    $commands.Add("& `"$Clangxx`" $cppLinkFlags $linkedObjects -o `"$exe`"")
    $script = "`$ErrorActionPreference = 'Stop'; " +
              (($commands.ToArray()) -join "; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; ") +
              "; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; if (-not (Test-Path -LiteralPath `"$exe`")) { exit 1 }"

    return Invoke-Captured "powershell.exe" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $script) $Workspace $LogPath
}

function Sync-VyxStd([string]$Workspace, [string]$Tool) {
    $stdSource = if ($Tool -eq "bootstrap") {
        Join-Path $repoRoot "bootstrap_compiler/std"
    } else {
        Join-Path $repoRoot "std"
    }
    $stdDest = Join-Path $Workspace "std"
    if ((Test-Path $stdSource) -and -not (Test-Path $stdDest)) {
        Copy-Item -Path $stdSource -Destination $stdDest -Recurse -Force
    }
}

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Write-Utf8Text([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Add-Utf8Line([string]$Path, [string]$Text) {
    $lastError = $null
    for ($attempt = 1; $attempt -le 8; ++$attempt) {
        try {
            [System.IO.File]::AppendAllText($Path, $Text + [Environment]::NewLine, $utf8NoBom)
            return
        } catch [System.IO.IOException] {
            $lastError = $_
            if ($attempt -lt 8) { Start-Sleep -Milliseconds (20 * $attempt) }
        }
    }
    throw $lastError
}

function Invoke-VyxBuild([string]$Compiler, [string]$Workspace, [string]$LogPath, [string]$Tool) {
    Sync-VyxStd $Workspace $Tool
    $args = [System.Collections.Generic.List[string]]::new()
    [void]$args.Add("build")
    if ($OptFlag.Length -gt 0) { [void]$args.Add($OptFlag) }
    [void]$args.Add("-j")
    [void]$args.Add($Jobs.ToString())
    return Invoke-Captured $Compiler $args.ToArray() $Workspace $LogPath
}

function Add-Result($Rows, [string]$Workload, [string]$Tool, [string]$Scenario, [int]$Iteration, [double]$ElapsedMs, [int]$ExitCode, [string]$OutputPath) {
    $row = [pscustomobject]@{
        timestamp = [DateTimeOffset]::Now.ToString("o")
        workload = $Workload
        tool = $Tool
        scenario = $Scenario
        iteration = $Iteration
        jobs = $Jobs
        elapsed_ms = [Math]::Round($ElapsedMs, 3)
        exit_code = $ExitCode
        output_path = $OutputPath
    }
    [void]$Rows.Add($row)
    $csvLine = '"{0}","{1}","{2}","{3}",{4},{5},{6},{7},"{8}"' -f $row.timestamp,$row.workload,$row.tool,$row.scenario,$row.iteration,$row.jobs,$row.elapsed_ms,$row.exit_code,$row.output_path.Replace('"','""')
    Add-Utf8Line $csvPath $csvLine
    Add-Utf8Line $jsonlPath ($row | ConvertTo-Json -Compress)
}

function Normalize-Stdout([string]$Text) {
    return (($Text -replace "`r`n", "`n") -replace "`r", "`n").Trim()
}

function Get-TextSha256([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $hash = $sha.ComputeHash($bytes)
        return (($hash | ForEach-Object { $_.ToString("x2") }) -join "")
    } finally {
        $sha.Dispose()
    }
}

function Add-RuntimeResult($Rows, [string]$Workload, [string]$Tool, [string]$Scenario, [int]$Iteration, [int]$Run, [double]$ElapsedMs, [int]$ExitCode, [bool]$TimedOut, [string]$ExePath, [string]$Stdout, [string]$LogPath) {
    $normalized = Normalize-Stdout $Stdout
    $row = [pscustomobject]@{
        timestamp = [DateTimeOffset]::Now.ToString("o")
        workload = $Workload
        tool = $Tool
        scenario = $Scenario
        iteration = $Iteration
        run = $Run
        jobs = $Jobs
        elapsed_ms = [Math]::Round($ElapsedMs, 3)
        exit_code = $ExitCode
        timed_out = $TimedOut
        output_hash = Get-TextSha256 $normalized
        stdout = $normalized
        exe_path = $ExePath
        log_path = $LogPath
    }
    [void]$Rows.Add($row)
    Add-Utf8Line $runtimeJsonlPath ($row | ConvertTo-Json -Compress)
}

function Invoke-RuntimeBenchmark([string]$Workload, [string]$Tool, [string]$Scenario, [int]$Iteration, [string]$ExePath, [string]$Workspace, [System.Collections.ArrayList]$RuntimeRows) {
    if ($SkipRuntime -or $Iteration -eq 0) { return }
    if (-not (Test-Path $ExePath)) {
        Write-ProgressLine ("        runtime skipped: executable not found {0}" -f $ExePath)
        return
    }
    $exeDir = Split-Path -Parent $ExePath
    $extraPath = $exeDir
    if ($Tool -ne "cpp26" -and $VyxRuntimePath.Length -gt 0) {
        $extraPath = Join-PathList @($exeDir, $VyxRuntimePath)
    }

    for ($run = -$RuntimeWarmups; $run -le $RuntimeRuns; ++$run) {
        if ($run -eq 0) { continue }
        $logRun = if ($run -lt 0) { "warmup$(-$run)" } else { "run$run" }
        $log = Join-Path $logRoot ("runtime-{0}-{1}-{2}-{3}-{4}.log" -f $Workload,$Tool,$Scenario,$Iteration,$logRun)
        $result = Invoke-Captured $ExePath @() $Workspace $log $RunTimeoutSec $extraPath
        Add-RuntimeResult $RuntimeRows $Workload $Tool $Scenario $Iteration $run $result.ElapsedMs $result.ExitCode $result.TimedOut $ExePath $result.Stdout $log
        if ($run -gt 0) {
            Write-ProgressLine ("        runtime {0} {1} {2} iter={3} run={4} exit={5} {6} ms" -f $Workload,$Tool,$Scenario,$Iteration,$run,$result.ExitCode,[Math]::Round($result.ElapsedMs,3))
        }
    }
}

function Resolve-VyxOutput([string]$Workspace) {
    $targetOutput = Join-Path $Workspace "target/vyx_bench.exe"
    $rootOutput = Join-Path $Workspace "vyx_bench.exe"
    $cachePath = Join-Path $Workspace ".cache/.vyx_cache"
    if (Test-Path $cachePath) {
        $linkLine = Get-Content -LiteralPath $cachePath | Where-Object { $_ -like "__link:*" } | Select-Object -Last 1
        if ($linkLine) {
            $parts = $linkLine -split "`t"
            if ($parts.Length -ge 2 -and $parts[1].Length -gt 0) {
                $resolved = Join-Path $Workspace $parts[1]
                if (Test-Path $resolved) {
                    return [System.IO.Path]::GetFullPath($resolved)
                }
            }
        }
    }
    $candidates = @()
    if (Test-Path $targetOutput) { $candidates += (Get-Item -LiteralPath $targetOutput) }
    if (Test-Path $rootOutput) { $candidates += (Get-Item -LiteralPath $rootOutput) }
    if ($candidates.Count -eq 0) { return $rootOutput }
    $newest = $candidates | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    return [System.IO.Path]::GetFullPath($newest.FullName)
}

function Run-VyxScenario($Workload, [string]$Tool, [string]$Compiler, [string]$Scenario, [int]$Iteration, [int]$Step, [int]$Total, [System.Collections.ArrayList]$Rows, [System.Collections.ArrayList]$RuntimeRows) {
    $source = $Workload.VyxSource
    $workspace = Join-Path $workRoot "$($Workload.Name)/$Tool/$Scenario/$Iteration"
    $log = Join-Path $logRoot "$($Workload.Name)-$Tool-$Scenario-$Iteration.log"
    Copy-Workload $source $workspace

    if ($Scenario -eq "warm") {
        [void](Invoke-VyxBuild $Compiler $workspace $log $Tool)
    } elseif ($Scenario -eq "incremental") {
        [void](Invoke-VyxBuild $Compiler $workspace $log $Tool)
        Touch-Append (Join-Path $workspace $Workload.IncrementalVyx) "`nfn bench_touch_marker() -> i64 { return 1; }"
    }

    Write-ProgressLine ("[{0}/{1}] {2} {3} {4}" -f $Step,$Total,$Tool,$Scenario,$Workload.Name)
    $result = Invoke-VyxBuild $Compiler $workspace $log $Tool
    $output = Resolve-VyxOutput $workspace
    Add-Result $Rows $Workload.Name $Tool $Scenario $Iteration $result.ElapsedMs $result.ExitCode $output
    Write-ProgressLine ("    {0} {1} {2} iter={3} exit={4} {5} ms" -f $Workload.Name,$Tool,$Scenario,$Iteration,$result.ExitCode,[Math]::Round($result.ElapsedMs,3))
    if ($result.ExitCode -eq 0) {
        Invoke-RuntimeBenchmark $Workload.Name $Tool $Scenario $Iteration $output $workspace $RuntimeRows
    }
}

function Run-CppScenario($Workload, [string]$Scenario, [int]$Iteration, [int]$Step, [int]$Total, [System.Collections.ArrayList]$Rows, [System.Collections.ArrayList]$RuntimeRows) {
    $source = $Workload.CppSource
    $workspace = Join-Path $workRoot "$($Workload.Name)/cpp26/$Scenario/$Iteration"
    $log = Join-Path $logRoot "$($Workload.Name)-cpp26-$Scenario-$Iteration.log"
    Copy-Workload $source $workspace

    if ($Scenario -eq "warm") {
        [void](Invoke-Cpp26Build $workspace $log)
    } elseif ($Scenario -eq "incremental") {
        [void](Invoke-Cpp26Build $workspace $log)
        Touch-Append (Join-Path $workspace $Workload.IncrementalCpp) "`nstd::int64_t cpp_touch_marker() { return 1; }"
    }

    Write-ProgressLine ("[{0}/{1}] cpp26 {2} {3}" -f $Step,$Total,$Scenario,$Workload.Name)
    $result = Invoke-Cpp26Build $workspace $log
    $output = Join-Path $workspace "target/cpp26_bench.exe"
    Add-Result $Rows $Workload.Name "cpp26" $Scenario $Iteration $result.ElapsedMs $result.ExitCode $output
    Write-ProgressLine ("    {0} cpp26 {1} iter={2} exit={3} {4} ms" -f $Workload.Name,$Scenario,$Iteration,$result.ExitCode,[Math]::Round($result.ElapsedMs,3))
    if ($result.ExitCode -eq 0) {
        Invoke-RuntimeBenchmark $Workload.Name "cpp26" $Scenario $Iteration $output $workspace $RuntimeRows
    }
}

function Get-Median([object[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return 0.0 }
    $mid = [int][Math]::Floor($sorted.Count / 2.0)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$mid] }
    return (([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0)
}

function Write-EmptyCsv([string]$Path, [string]$Header) {
    Write-Utf8Text $Path ($Header + [Environment]::NewLine)
}

function Export-RuntimeTables([System.Collections.ArrayList]$RuntimeRows) {
    if ($RuntimeRows.Count -gt 0) {
        $RuntimeRows | Export-Csv -Path $runtimeCsvPath -NoTypeInformation -Encoding UTF8
    } else {
        Write-EmptyCsv $runtimeCsvPath "timestamp,workload,tool,scenario,iteration,run,jobs,elapsed_ms,exit_code,timed_out,output_hash,stdout,exe_path,log_path"
    }

    $measured = @($RuntimeRows | Where-Object { $_.iteration -gt 0 -and $_.run -gt 0 })
    if ($measured.Count -eq 0) {
        Write-EmptyCsv $runtimeSummaryPath "workload,tool,scenario,runs,avg_ms,median_ms,min_ms,max_ms,exit_ok,timed_out,unique_outputs"
        Write-EmptyCsv $runtimeComparisonPath "workload,scenario,tool,avg_ms,median_ms,cpp26_avg_ms,cpp26_median_ms,ratio_to_cpp26,speedup_vs_cpp26,median_ratio_to_cpp26,median_speedup_vs_cpp26"
        Write-EmptyCsv $runtimeSemanticPath "workload,scenario,tool,runs,exit_ok,stable,matches_cpp26,output_hash,stdout"
        return @()
    }

    $summaryRows = @(
        $measured |
            Group-Object workload, tool, scenario |
            ForEach-Object {
                $vals = @($_.Group | ForEach-Object { [double]$_.elapsed_ms })
                $uniqueOutputs = @($_.Group | Where-Object { $_.exit_code -eq 0 -and -not $_.timed_out } | Select-Object -ExpandProperty output_hash -Unique)
                [pscustomobject]@{
                    workload = $_.Group[0].workload
                    tool = $_.Group[0].tool
                    scenario = $_.Group[0].scenario
                    runs = $_.Count
                    avg_ms = [Math]::Round(($_.Group | Measure-Object elapsed_ms -Average).Average, 3)
                    median_ms = [Math]::Round((Get-Median $vals), 3)
                    min_ms = [Math]::Round(($_.Group | Measure-Object elapsed_ms -Minimum).Minimum, 3)
                    max_ms = [Math]::Round(($_.Group | Measure-Object elapsed_ms -Maximum).Maximum, 3)
                    exit_ok = @($_.Group | Where-Object { $_.exit_code -eq 0 }).Count
                    timed_out = @($_.Group | Where-Object { $_.timed_out }).Count
                    unique_outputs = $uniqueOutputs.Count
                }
            }
    )
    $summaryRows | Export-Csv -Path $runtimeSummaryPath -NoTypeInformation -Encoding UTF8

    $comparisonRows = [System.Collections.ArrayList]::new()
    foreach ($workload in @($summaryRows | Select-Object -ExpandProperty workload -Unique)) {
        foreach ($scenario in @($summaryRows | Where-Object { $_.workload -eq $workload } | Select-Object -ExpandProperty scenario -Unique)) {
            $cpp = @($summaryRows | Where-Object { $_.workload -eq $workload -and $_.scenario -eq $scenario -and $_.tool -eq "cpp26" } | Select-Object -First 1)
            $cppAvg = 0.0
            $cppMedian = 0.0
            if ($cpp.Count -gt 0) {
                $cppAvg = [double]$cpp[0].avg_ms
                $cppMedian = [double]$cpp[0].median_ms
            }
            foreach ($row in @($summaryRows | Where-Object { $_.workload -eq $workload -and $_.scenario -eq $scenario })) {
                $ratio = $null
                $speedup = $null
                $medianRatio = $null
                $medianSpeedup = $null
                if ($cppAvg -gt 0) {
                    $ratio = [Math]::Round(([double]$row.avg_ms / $cppAvg), 4)
                    if ([double]$row.avg_ms -gt 0) { $speedup = [Math]::Round(($cppAvg / [double]$row.avg_ms), 4) }
                }
                if ($cppMedian -gt 0) {
                    $medianRatio = [Math]::Round(([double]$row.median_ms / $cppMedian), 4)
                    if ([double]$row.median_ms -gt 0) { $medianSpeedup = [Math]::Round(($cppMedian / [double]$row.median_ms), 4) }
                }
                [void]$comparisonRows.Add([pscustomobject]@{
                    workload = $workload
                    scenario = $scenario
                    tool = $row.tool
                    avg_ms = $row.avg_ms
                    median_ms = $row.median_ms
                    cpp26_avg_ms = if ($cppAvg -gt 0) { [Math]::Round($cppAvg, 3) } else { $null }
                    cpp26_median_ms = if ($cppMedian -gt 0) { [Math]::Round($cppMedian, 3) } else { $null }
                    ratio_to_cpp26 = $ratio
                    speedup_vs_cpp26 = $speedup
                    median_ratio_to_cpp26 = $medianRatio
                    median_speedup_vs_cpp26 = $medianSpeedup
                })
            }
        }
    }
    $comparisonRows | Export-Csv -Path $runtimeComparisonPath -NoTypeInformation -Encoding UTF8

    $semanticRows = [System.Collections.ArrayList]::new()
    foreach ($workload in @($measured | Select-Object -ExpandProperty workload -Unique)) {
        foreach ($scenario in @($measured | Where-Object { $_.workload -eq $workload } | Select-Object -ExpandProperty scenario -Unique)) {
            $cppRows = @($measured | Where-Object { $_.workload -eq $workload -and $_.scenario -eq $scenario -and $_.tool -eq "cpp26" -and $_.exit_code -eq 0 -and -not $_.timed_out })
            $cppStdout = ""
            if ($cppRows.Count -gt 0) { $cppStdout = $cppRows[0].stdout }
            foreach ($group in @($measured | Where-Object { $_.workload -eq $workload -and $_.scenario -eq $scenario } | Group-Object tool)) {
                $okRows = @($group.Group | Where-Object { $_.exit_code -eq 0 -and -not $_.timed_out })
                $outputs = @($okRows | Select-Object -ExpandProperty stdout -Unique)
                $hashes = @($okRows | Select-Object -ExpandProperty output_hash -Unique)
                $stable = ($outputs.Count -eq 1)
                $matchesCpp = $false
                if ($cppStdout.Length -gt 0 -and $stable) { $matchesCpp = ($outputs[0] -eq $cppStdout) }
                [void]$semanticRows.Add([pscustomobject]@{
                    workload = $workload
                    scenario = $scenario
                    tool = $group.Name
                    runs = $group.Count
                    exit_ok = $okRows.Count
                    stable = $stable
                    matches_cpp26 = $matchesCpp
                    output_hash = if ($hashes.Count -eq 1) { $hashes[0] } elseif ($hashes.Count -gt 1) { "<diverged>" } else { "" }
                    stdout = if ($outputs.Count -eq 1) { $outputs[0] } elseif ($outputs.Count -gt 1) { "<diverged>" } else { "" }
                })
            }
        }
    }
    $semanticRows | Export-Csv -Path $runtimeSemanticPath -NoTypeInformation -Encoding UTF8
    return $summaryRows
}

New-Item -ItemType Directory -Force -Path $OutDir, $workRoot, $logRoot | Out-Null
Write-Utf8Text $csvPath ('timestamp,workload,tool,scenario,iteration,jobs,elapsed_ms,exit_code,output_path' + [Environment]::NewLine)
Write-Utf8Text $jsonlPath ''
Write-Utf8Text $runtimeJsonlPath ''

if (-not $SkipHost -and -not (Test-Path $HostVyxc)) { throw "HostVyxc not found: $HostVyxc" }
if (-not $SkipBootstrap -and -not (Test-Path $BootstrapVyxc)) { throw "BootstrapVyxc not found: $BootstrapVyxc" }

$allWorkloads = @(
    [pscustomobject]@{ Name = "baseline"; VyxSource = Join-Path $benchRoot "workloads/vyx_project"; CppSource = Join-Path $benchRoot "workloads/cpp26_project"; IncrementalVyx = "src/string_work.vyx"; IncrementalCpp = "src/string_work.cpp" },
    [pscustomobject]@{ Name = "core_compute"; VyxSource = Join-Path $benchRoot "workloads/core_compute_vyx_project"; CppSource = Join-Path $benchRoot "workloads/core_compute_cpp26_project"; IncrementalVyx = "src/compute_work.vyx"; IncrementalCpp = "src/compute_work.cpp" },
    [pscustomobject]@{ Name = "strings"; VyxSource = Join-Path $benchRoot "workloads/strings_vyx_project"; CppSource = Join-Path $benchRoot "workloads/strings_cpp26_project"; IncrementalVyx = "src/string_work.vyx"; IncrementalCpp = "src/string_work.cpp" },
    [pscustomobject]@{ Name = "generics"; VyxSource = Join-Path $benchRoot "workloads/generics_vyx_project"; CppSource = Join-Path $benchRoot "workloads/generics_cpp26_project"; IncrementalVyx = "src/generic_work.vyx"; IncrementalCpp = "src/generic_work.cpp" },
    [pscustomobject]@{ Name = "collections"; VyxSource = Join-Path $benchRoot "workloads/collections_vyx_project"; CppSource = Join-Path $benchRoot "workloads/collections_cpp26_project"; IncrementalVyx = "src/collections_work.vyx"; IncrementalCpp = "src/collections_work.cpp" },
    [pscustomobject]@{ Name = "memory_model"; VyxSource = Join-Path $benchRoot "workloads/memory_model_vyx_project"; CppSource = Join-Path $benchRoot "workloads/memory_model_cpp26_project"; IncrementalVyx = "src/main.vyx"; IncrementalCpp = "src/main.cpp" },
    [pscustomobject]@{ Name = "memory_value"; VyxSource = Join-Path $benchRoot "workloads/memory_value_vyx_project"; CppSource = Join-Path $benchRoot "workloads/memory_value_cpp26_project"; IncrementalVyx = "src/main.vyx"; IncrementalCpp = "src/main.cpp" },
    [pscustomobject]@{ Name = "memory_unique"; VyxSource = Join-Path $benchRoot "workloads/memory_unique_vyx_project"; CppSource = Join-Path $benchRoot "workloads/memory_unique_cpp26_project"; IncrementalVyx = "src/main.vyx"; IncrementalCpp = "src/main.cpp" },
    [pscustomobject]@{ Name = "memory_shared"; VyxSource = Join-Path $benchRoot "workloads/memory_shared_vyx_project"; CppSource = Join-Path $benchRoot "workloads/memory_shared_cpp26_project"; IncrementalVyx = "src/main.vyx"; IncrementalCpp = "src/main.cpp" },
    [pscustomobject]@{ Name = "type_abstraction"; VyxSource = Join-Path $benchRoot "workloads/type_abstraction_vyx_project"; CppSource = Join-Path $benchRoot "workloads/type_abstraction_cpp26_project"; IncrementalVyx = "src/main.vyx"; IncrementalCpp = "src/main.cpp" },
    [pscustomobject]@{ Name = "heavy"; VyxSource = Join-Path $benchRoot "workloads/heavy_vyx_project"; CppSource = Join-Path $benchRoot "workloads/heavy_cpp26_project"; IncrementalVyx = "src/vec_heavy.vyx"; IncrementalCpp = "src/heavy_touch.cpp" }
)
if ($Workloads.Count -gt 0) {
    $selectedNames = @($Workloads | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_.Length -gt 0 })
    $allWorkloads = @($allWorkloads | Where-Object { $selectedNames -contains $_.Name.ToLowerInvariant() })
    if ($allWorkloads.Count -ne $selectedNames.Count) { throw "Unknown workload in: $($Workloads -join ', ')" }
}

$tools = @()
if (-not $SkipHost) { $tools += "host" }
if (-not $SkipBootstrap) { $tools += "bootstrap" }
if (-not $SkipCpp26) { $tools += "cpp26" }
$scenarios = @("cold", "warm", "incremental")
$total = ($Warmups + $Iterations) * $allWorkloads.Count * $tools.Count * $scenarios.Count
$step = 0
$rows = [System.Collections.ArrayList]::new()
$runtimeRows = [System.Collections.ArrayList]::new()

Write-ProgressLine "benchmark root: $benchRoot"
Write-ProgressLine "output dir: $OutDir"
if ($OptFlag.Length -gt 0) { Write-ProgressLine "compile opt flag: $OptFlag" }
if (-not $SkipCpp26) {
    $cppLtoMode = if ($SkipCppLto) { "disabled" } else { "enabled (-flto, lld)" }
    Write-ProgressLine "cpp26 whole-program LTO: $cppLtoMode"
}
if (-not $SkipHost) { Write-ProgressLine "host std: $(Join-Path $repoRoot 'std')" }
if (-not $SkipBootstrap) { Write-ProgressLine "bootstrap std: $(Join-Path $repoRoot 'bootstrap_compiler/std')" }
if (-not $SkipRuntime) {
    Write-ProgressLine ("runtime runs: warmups={0} measured={1} timeout={2}s" -f $RuntimeWarmups,$RuntimeRuns,$RunTimeoutSec)
    if ($VyxRuntimePath.Length -gt 0) { Write-ProgressLine "vyx runtime path: $VyxRuntimePath" }
}

for ($i = 1; $i -le $Warmups; ++$i) {
    foreach ($workload in $allWorkloads) {
        foreach ($tool in $tools) {
            foreach ($scenario in $scenarios) {
                $step++
                if ($tool -eq "host") { Run-VyxScenario $workload "host" $HostVyxc $scenario (-$i) $step $total $rows $runtimeRows }
                elseif ($tool -eq "bootstrap") { Run-VyxScenario $workload "bootstrap" $BootstrapVyxc $scenario (-$i) $step $total $rows $runtimeRows }
                else { Run-CppScenario $workload $scenario (-$i) $step $total $rows $runtimeRows }
            }
        }
    }
}

for ($i = 1; $i -le $Iterations; ++$i) {
    foreach ($workload in $allWorkloads) {
        foreach ($tool in $tools) {
            foreach ($scenario in $scenarios) {
                $step++
                if ($tool -eq "host") { Run-VyxScenario $workload "host" $HostVyxc $scenario $i $step $total $rows $runtimeRows }
                elseif ($tool -eq "bootstrap") { Run-VyxScenario $workload "bootstrap" $BootstrapVyxc $scenario $i $step $total $rows $runtimeRows }
                else { Run-CppScenario $workload $scenario $i $step $total $rows $runtimeRows }
            }
        }
    }
}

Write-ProgressLine "summary:"
$rows |
    Where-Object { $_.iteration -gt 0 } |
    Group-Object workload, tool, scenario |
    ForEach-Object {
        $avg = ($_.Group | Measure-Object elapsed_ms -Average).Average
        [pscustomobject]@{ case = $_.Name; avg_ms = [Math]::Round($avg, 3); runs = $_.Count }
    } |
    Format-Table -AutoSize

if (-not $SkipRuntime) {
    $runtimeSummary = Export-RuntimeTables $runtimeRows
    Write-ProgressLine "runtime summary:"
    $runtimeSummary |
        Sort-Object workload, scenario, tool |
        Format-Table -AutoSize
    Write-ProgressLine "wrote $runtimeCsvPath"
    Write-ProgressLine "wrote $runtimeJsonlPath"
    Write-ProgressLine "wrote $runtimeSummaryPath"
    Write-ProgressLine "wrote $runtimeComparisonPath"
    Write-ProgressLine "wrote $runtimeSemanticPath"
    if ($RequireSemanticMatch) {
        $compileFailures = @($rows | Where-Object {
            $_.iteration -gt 0 -and $_.exit_code -ne 0
        })
        if ($compileFailures.Count -gt 0) {
            $labels = ($compileFailures | ForEach-Object {
                "$($_.workload)/$($_.tool)/$($_.scenario)/iter=$($_.iteration)"
            } | Select-Object -Unique) -join ", "
            throw "benchmark compilation failed: $labels"
        }
        $missingRuntime = [System.Collections.ArrayList]::new()
        foreach ($workload in $allWorkloads) {
            foreach ($tool in $tools) {
                foreach ($scenario in $scenarios) {
                    $measuredRows = @($runtimeRows | Where-Object {
                        $_.workload -eq $workload.Name -and $_.tool -eq $tool -and $_.scenario -eq $scenario -and $_.iteration -gt 0 -and $_.run -gt 0
                    })
                    $expectedRows = $Iterations * $RuntimeRuns
                    if ($measuredRows.Count -ne $expectedRows) {
                        [void]$missingRuntime.Add("$($workload.Name)/$tool/$scenario expected=$expectedRows actual=$($measuredRows.Count)")
                    }
                }
            }
        }
        if ($missingRuntime.Count -gt 0) {
            throw "runtime samples missing: $($missingRuntime -join ', ')"
        }
        $semanticFailures = @(Import-Csv -LiteralPath $runtimeSemanticPath | Where-Object {
            $_.exit_ok -ne $_.runs -or $_.stable -ne "True" -or $_.matches_cpp26 -ne "True"
        })
        if ($semanticFailures.Count -gt 0) {
            $labels = ($semanticFailures | ForEach-Object { "$($_.workload)/$($_.tool)/$($_.scenario)" }) -join ", "
            throw "runtime semantic validation failed: $labels"
        }
    }
}

Write-ProgressLine "wrote $csvPath"
Write-ProgressLine "wrote $jsonlPath"

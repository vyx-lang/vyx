# Benchmark runner with condensed console output.
# Default profile is native-release = all-O3 (C++ O3 via CMake, Rust opt-level=3, Vyx -O3).
# Machine-readable output is unchanged: out/results.tsv, out/summary.tsv, out/environment.json.
# Tool chatter is captured to out/build_*.log; pass -StreamToolOutput to see it live.
# Pass -ShowRawRecords to additionally print each raw benchmark record line.
param(
    [UInt64]$Workload = 1000000000,
    [UInt64]$LiveSet = 1000000,
    [UInt64]$StringBytes = 1048576,
    [UInt64]$Seed = 1592594996,
    [ValidateSet("all", "vec", "dict", "string")][string]$Case = "all",
    [ValidateSet("matched-o2", "native-release")][string]$Profile = "native-release",
    [ValidateSet("native", "deterministic-splitmix64")][string]$HashPolicy = "deterministic-splitmix64",
    [int]$Runs = 2,
    [int]$Warmups = 1,
    [UInt64]$ClockRate = 0,
    [UInt64]$OrderSeed = 20260822,
    [switch]$NoReserve,
    [switch]$SkipBuild,
    [switch]$KeepBuild,
    [switch]$StreamToolOutput,
    [switch]$ShowRawRecords
)

$ErrorActionPreference = "Stop"
if ($Runs -lt 1) { throw "Runs must be at least 1." }
if ($Warmups -lt 0) { throw "Warmups cannot be negative." }

$Invariant = [Globalization.CultureInfo]::InvariantCulture
$script:CanInlineUpdate = ($Host.Name -eq "ConsoleHost") -and -not [Console]::IsOutputRedirected
$script:LastInlineLength = 0
$script:RunCounter = 0
$script:TotalRunCount = 0
$scriptStartTime = [DateTime]::UtcNow

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$suite = $PSScriptRoot
$cppDir = Join-Path $suite "cpp"
$cppBuild = Join-Path $cppDir "build"
$rustDir = Join-Path $suite "rust"
$vyxDir = Join-Path $suite "vyx"
$outDir = Join-Path $suite "out"
$boot = Join-Path $root "bootstrap_compiler\out\boot.exe"
$runtimeDir = Join-Path $root "runtime\out"
$vyxExe = Join-Path $outDir "containers_bench_vyx.exe"
$log = Join-Path $outDir "results.tsv"
$summaryLog = Join-Path $outDir "summary.tsv"
$environmentLog = Join-Path $outDir "environment.json"

if ($ClockRate -eq 0) {
    $ClockRate = if ($env:OS -eq "Windows_NT") { [UInt64]1000 } else { [UInt64]1000000 }
}
$reserveValue = if ($NoReserve) { 0 } else { 1 }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Rust profile selection for the all-O3 mode: prefer [profile.native-release]
# (opt-level=3 + thin LTO) when defined; otherwise plain --release, which is
# already opt-level=3 by default.
$cargoTomlPath = Join-Path $rustDir "Cargo.toml"
$useCargoNativeProfile = $false
if ($Profile -eq "native-release") {
    if ((Test-Path -LiteralPath $cargoTomlPath) -and
        (Select-String -LiteralPath $cargoTomlPath -Pattern '^\s*\[profile\.native-release\]' -Quiet)) {
        $useCargoNativeProfile = $true
    }
}

# ---------------------------------------------------------------------------
# Presentation helpers
# ---------------------------------------------------------------------------

function Format-Ns([double]$Ns) {
    if ($Ns -ge 1000000000) { return ($Ns / 1000000000.0).ToString("F3", $Invariant) + "s" }
    if ($Ns -ge 1000000) { return ($Ns / 1000000.0).ToString("F2", $Invariant) + "ms" }
    if ($Ns -ge 1000) { return ($Ns / 1000.0).ToString("F1", $Invariant) + "us" }
    return $Ns.ToString("F0", $Invariant) + "ns"
}

function Format-Bytes([double]$Bytes) {
    if ($Bytes -ge 1073741824) { return ($Bytes / 1073741824.0).ToString("F2", $Invariant) + "GiB" }
    if ($Bytes -ge 1048576) { return ($Bytes / 1048576.0).ToString("F1", $Invariant) + "MiB" }
    if ($Bytes -ge 1024) { return ($Bytes / 1024.0).ToString("F1", $Invariant) + "KiB" }
    return $Bytes.ToString("F0", $Invariant) + "B"
}

function Write-Inline([string]$Text) {
    if (-not $script:CanInlineUpdate) { return }
    $padding = ""
    if ($Text.Length -lt $script:LastInlineLength) {
        $padding = " " * ($script:LastInlineLength - $Text.Length)
    }
    Write-Host ("`r" + $Text + $padding) -NoNewline
    $script:LastInlineLength = $Text.Length
}

function Clear-Inline {
    if (-not $script:CanInlineUpdate) { return }
    if ($script:LastInlineLength -gt 0) {
        Write-Host ("`r" + (" " * $script:LastInlineLength) + "`r") -NoNewline
        $script:LastInlineLength = 0
    }
}

function Write-StepOk([string]$Label, [long]$ElapsedMs, [string]$Note = "") {
    Write-Host ("  [" + $Label.PadRight(12) + "] ok ") -ForegroundColor Green -NoNewline
    Write-Host ("{0,7:F1}s" -f ($ElapsedMs / 1000.0)) -NoNewline
    if ($Note.Length -gt 0) {
        Write-Host ("  " + $Note) -ForegroundColor DarkGray -NoNewline
    }
    Write-Host ""
}

function Write-Table([string[]]$Headers, [object[][]]$Rows, [string[]]$Align = @(), [int]$Highlight = -1) {
    $columnCount = $Headers.Count
    $widths = New-Object "int[]" $columnCount
    for ($i = 0; $i -lt $columnCount; $i++) { $widths[$i] = $Headers[$i].Length }
    for ($r = 0; $r -lt $Rows.Count; $r++) {
        $cellCount = if ($null -eq $Rows[$r]) { 0 } else { $Rows[$r].Count }
        if ($cellCount -ne $columnCount) {
            throw ("Write-Table: row " + $r + " has " + $cellCount +
                " cell(s); expected " + $columnCount + " (caller built rows incorrectly)")
        }
    }
    foreach ($row in $Rows) {
        for ($i = 0; $i -lt $columnCount; $i++) {
            $length = ([string]$row[$i]).Length
            if ($length -gt $widths[$i]) { $widths[$i] = $length }
        }
    }
    $parts = @()
    for ($i = 0; $i -lt $columnCount; $i++) {
        $sign = "-"
        if (($i -lt $Align.Count) -and ($Align[$i] -eq "R")) { $sign = "" }
        $parts += ("{" + $i + "," + $sign + $widths[$i] + "}")
    }
    $format = "  " + ($parts -join "  ")
    Write-Host ($format -f $Headers) -ForegroundColor White
    Write-Host ("  " + (($widths | ForEach-Object { "-" * $_ }) -join "  ")) -ForegroundColor DarkGray
    for ($r = 0; $r -lt $Rows.Count; $r++) {
        $line = $format -f $Rows[$r]
        if ($r -eq $Highlight) { Write-Host $line -ForegroundColor Green }
        else { Write-Host $line }
    }
}

function Write-Banner {
    $cases = if ($Case -eq "all") { "vec,dict,string" } else { $Case }
    $reserveText = if ($NoReserve) { "off" } else { "on" }
    $profileText = if ($Profile -eq "matched-o2") { "matched-o2" } else { "native-release (all-O3)" }
    Write-Host ""
    Write-Host ("  containers benchmark  profile=" + $profileText + "  hash=" + $HashPolicy) -ForegroundColor White
    Write-Host ("  workload=" + $Workload.ToString("N0", $Invariant) +
        "  live_set=" + $LiveSet.ToString("N0", $Invariant) +
        "  string_bytes=" + (Format-Bytes $StringBytes))
    Write-Host ("  seed=" + $Seed + "  reserve=" + $reserveText +
        "  runs=" + $Runs + "  warmups=" + $Warmups + "  clock_rate=" + $ClockRate)
    Write-Host ("  cases=" + $cases + "  order_seed=" + $OrderSeed)
    Write-Host ("  root=" + $root) -ForegroundColor DarkGray
    Write-Host ""
}

# ---------------------------------------------------------------------------
# Process / tooling helpers
# ---------------------------------------------------------------------------

function Resolve-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($null -eq $cmd) { throw ("Required tool not found: " + $name) }
    return $cmd.Source
}

function ConvertTo-ArgumentString([string[]]$Arguments) {
    $builder = New-Object System.Text.StringBuilder
    foreach ($argument in $Arguments) {
        if ($builder.Length -gt 0) { [void]$builder.Append(" ") }
        if ($argument -match "\s") { [void]$builder.Append('"' + $argument + '"') }
        else { [void]$builder.Append($argument) }
    }
    return $builder.ToString()
}

function Get-CommandVersion([string]$file, [string[]]$arguments) {
    try {
        $text = & $file @arguments 2>&1 | Out-String
        return $text.Trim()
    } catch { return "unavailable" }
}

function Count-LinesMatching([string]$Text, [string]$Pattern) {
    return @(($Text -split "\r?\n") | Where-Object { $_ -match $Pattern }).Count
}

function Invoke-Captured([string]$Label, [string]$File, [string[]]$Arguments, [string]$Cwd, [string]$LogPath) {
    $commandLine = $File + " " + (ConvertTo-ArgumentString $Arguments)
    if ($StreamToolOutput) {
        Write-Host ("> " + $commandLine) -ForegroundColor DarkGray
        $swStream = [System.Diagnostics.Stopwatch]::StartNew()
        Push-Location $Cwd
        try { & $File @Arguments } finally { Pop-Location }
        $swStream.Stop()
        if ($LASTEXITCODE -ne 0) { throw ($File + " failed with exit code " + $LASTEXITCODE) }
        return @{ StdOut = ""; StdErr = ""; ElapsedMs = $swStream.ElapsedMilliseconds }
    }
    Set-Content -LiteralPath $LogPath -Encoding utf8 -Value ("# cmd: " + $commandLine)

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $File
    $psi.Arguments = ConvertTo-ArgumentString $Arguments
    $psi.WorkingDirectory = $Cwd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::new()
    $p.StartInfo = $psi
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$p.Start()
    $stdoutTask = $p.StandardOutput.ReadToEndAsync()
    $stderrTask = $p.StandardError.ReadToEndAsync()
    $lastTickMs = -10000L
    while (-not $p.WaitForExit(200)) {
        $elapsedMs = $sw.ElapsedMilliseconds
        if (($elapsedMs - $lastTickMs) -ge 1000) {
            $lastTickMs = $elapsedMs
            Write-Inline ("  [" + $Label + "] running... " +
                ($elapsedMs / 1000.0).ToString("F1", $Invariant) + "s")
        }
    }
    $sw.Stop()
    Clear-Inline
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    if ($stdout.Trim().Length -gt 0) { Add-Content -LiteralPath $LogPath -Encoding utf8 -Value $stdout }
    if ($stderr.Trim().Length -gt 0) {
        Add-Content -LiteralPath $LogPath -Encoding utf8 -Value "# stderr"
        Add-Content -LiteralPath $LogPath -Encoding utf8 -Value $stderr
    }
    if ($p.ExitCode -ne 0) {
        Write-Host ("  [" + $Label + "] FAILED exit=" + $p.ExitCode + "  log: " + $LogPath) -ForegroundColor Red
        $tail = @((($stdout -split "\r?\n") + ($stderr -split "\r?\n")) |
            Where-Object { $_.Trim().Length -gt 0 } | Select-Object -Last 15)
        foreach ($line in $tail) { Write-Host ("    " + $line) -ForegroundColor DarkRed }
        $p.Dispose()
        throw ($File + " failed with exit code " + $p.ExitCode)
    }
    $p.Dispose()
    return @{ StdOut = $stdout; StdErr = $stderr; ElapsedMs = $sw.ElapsedMilliseconds }
}

# ---------------------------------------------------------------------------
# Benchmark protocol (unchanged logic)
# ---------------------------------------------------------------------------

function Convert-BenchmarkRecord([string]$line) {
    $fields = @{}
    foreach ($field in ($line -split "\s+" | Where-Object { $_.Length -gt 0 })) {
        $separator = $field.IndexOf([char]"=")
        if ($separator -lt 1) { throw ("Malformed benchmark field: " + $field) }
        $fields[$field.Substring(0, $separator)] = $field.Substring($separator + 1)
    }
    $required = @("language", "bench", "workload", "live_set", "string_bytes", "seed",
        "reserve", "profile", "api_mode", "hash_policy", "timing_mode", "clock_rate",
        "setup_ticks", "op_ticks", "total_ticks", "setup_ns", "op_ns", "total_ns",
        "op_ops_per_sec", "final_size", "checksum", "peak_rss_bytes")
    foreach ($name in $required) {
        if (-not $fields.ContainsKey($name)) { throw ("Missing benchmark field: " + $name) }
    }
    return $fields
}

function Get-RunOrder([int]$round) {
    $names = @("cpp26", "rust", "vyx")
    $shift = [int](($OrderSeed + [UInt64]$round) % [UInt64]$names.Count)
    if ($shift -gt 0) {
        $names = @($names[$shift..($names.Count - 1)] + $names[0..($shift - 1)])
    }
    if (($round % 2) -eq 0) { [array]::Reverse($names) }
    return $names
}

function Median([double[]]$values) {
    if ($null -eq $values -or $values.Count -eq 0) { return 0.0 }
    $ordered = @($values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) { return [double]$ordered[$middle] }
    return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
}

function Archive-PreviousResults {
    $existing = @($log, $summaryLog, $environmentLog) |
        Where-Object { Test-Path -LiteralPath $_ }
    if ($existing.Count -eq 0) { return }

    $archiveRoot = Join-Path $outDir "archive"
    $stamp = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss-fff")
    $archiveDir = Join-Path $archiveRoot $stamp
    New-Item -ItemType Directory -Force -Path $archiveDir | Out-Null
    foreach ($path in $existing) {
        Move-Item -LiteralPath $path -Destination (Join-Path $archiveDir ([System.IO.Path]::GetFileName($path)))
    }
    Write-Host ("Archived previous results: " + $archiveDir)
}

function Run-Measured([string]$name, [string]$exe, [string]$benchCase,
                       [string]$runKind, [int]$round, [int]$orderIndex) {
    $runArguments = @("--case=$benchCase", "--workload=$Workload", "--live-set=$LiveSet",
        "--string-bytes=$StringBytes", "--seed=$Seed", "--profile=$Profile",
        "--hash-policy=$HashPolicy", "--clock-rate=$ClockRate")
    if ($NoReserve) { $runArguments += "--no-reserve" }
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exe
    $psi.Arguments = ConvertTo-ArgumentString $runArguments
    $psi.WorkingDirectory = $suite
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables["PATH"] = (Join-Path $runtimeDir "") + [System.IO.Path]::PathSeparator + $env:PATH
    $p = [System.Diagnostics.Process]::new()
    $p.StartInfo = $psi
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$p.Start()
    $stdoutTask = $p.StandardOutput.ReadToEndAsync()
    $stderrTask = $p.StandardError.ReadToEndAsync()
    $script:RunCounter++
    [UInt64]$peakWorkingSet = 0
    $lastTickMs = -10000L
    while (-not $p.WaitForExit(100)) {
        try {
            $p.Refresh()
            $current = [UInt64][Math]::Max(0, $p.WorkingSet64)
            if ($current -gt $peakWorkingSet) { $peakWorkingSet = $current }
            $reported = [UInt64][Math]::Max(0, $p.PeakWorkingSet64)
            if ($reported -gt $peakWorkingSet) { $peakWorkingSet = $reported }
        } catch [System.InvalidOperationException] { break } catch [System.ComponentModel.Win32Exception] { break }
        $elapsedMs = $sw.ElapsedMilliseconds
        if (($elapsedMs - $lastTickMs) -ge 500) {
            $lastTickMs = $elapsedMs
            Write-Inline ("  [{0}/{1}] {2}/{3} running {4:F1}s  rss~{5}" -f `
                $script:RunCounter, $script:TotalRunCount, $benchCase, $name,
                ($elapsedMs / 1000.0), (Format-Bytes $peakWorkingSet))
        }
    }
    try {
        $p.Refresh()
        $reported = [UInt64][Math]::Max(0, $p.PeakWorkingSet64)
        if ($reported -gt $peakWorkingSet) { $peakWorkingSet = $reported }
    } catch [System.InvalidOperationException] { } catch [System.ComponentModel.Win32Exception] { }
    $sw.Stop()
    Clear-Inline
    $stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
    $stderr = $stderrTask.GetAwaiter().GetResult().Trim()
    if ($p.ExitCode -ne 0) { throw ($name + " failed (" + $p.ExitCode + "): " + $stderr + " " + $stdout) }
    $lines = @($stdout -split "\r?\n" | Where-Object { $_.Trim().Length -gt 0 })
    if ($lines.Count -ne 1) { throw ($name + "/" + $benchCase + " emitted " + $lines.Count + " records; expected 1") }
    $fields = Convert-BenchmarkRecord $lines[0]
    if ($fields["language"] -ne $name -or $fields["bench"] -ne $benchCase) { throw "Unexpected benchmark identity." }
    if ([UInt64]$fields["workload"] -ne $Workload -or [UInt64]$fields["live_set"] -ne $LiveSet -or
        [UInt64]$fields["string_bytes"] -ne $StringBytes -or [UInt64]$fields["seed"] -ne $Seed) {
        throw ($name + "/" + $benchCase + " did not honor the requested configuration")
    }
    if ([UInt64]$fields["reserve"] -ne $reserveValue -or $fields["profile"] -ne $Profile -or
        $fields["api_mode"] -ne "safe" -or $fields["hash_policy"] -ne $HashPolicy -or
        $fields["timing_mode"] -ne "c_clock_cpu" -or [UInt64]$fields["clock_rate"] -ne $ClockRate) {
        throw ($name + "/" + $benchCase + " violated the benchmark profile/API/timing contract")
    }
    $wallNs = [UInt64][Math]::Max(1.0, [Math]::Round($sw.Elapsed.TotalMilliseconds * 1000000.0))
    $wallMs = $sw.Elapsed.TotalMilliseconds.ToString("F3", [Globalization.CultureInfo]::InvariantCulture)
    $wallRate = ([double]$Workload * 1000000000.0 / [double]$wallNs).ToString("F3", [Globalization.CultureInfo]::InvariantCulture)
    $peakSource = "runner_process"
    if ($peakWorkingSet -eq 0 -and $fields["peak_rss_bytes"] -match "^[0-9]+$") {
        $reportedPeak = [UInt64]$fields["peak_rss_bytes"]
        if ($reportedPeak -gt 0) {
            $peakWorkingSet = $reportedPeak
            $peakSource = "self_report_fallback"
        }
    }
    if ($peakWorkingSet -eq 0) { $peakSource = "unavailable" }
    $row = @($runKind, $round, $orderIndex, $fields["language"], $fields["bench"], $fields["workload"],
        $fields["live_set"], $fields["string_bytes"], $fields["seed"], $fields["reserve"], $fields["profile"],
        $fields["api_mode"], $fields["hash_policy"], $fields["timing_mode"], $fields["clock_rate"],
        $fields["setup_ticks"], $fields["op_ticks"], $fields["total_ticks"], $fields["setup_ns"],
        $fields["op_ns"], $fields["total_ns"], $fields["op_ops_per_sec"], $fields["final_size"],
        $fields["checksum"], $fields["peak_rss_bytes"], $wallNs, $wallMs, $wallRate, $peakWorkingSet, $peakSource) -join [char]9
    Add-Content -LiteralPath $log -Encoding ascii -Value $row

    $tag = "meas"; $rowColor = "White"
    if ($runKind -eq "warmup") { $tag = "warm"; $rowColor = "DarkGray" }
    $rateMillions = [double]$Workload * 1000000000.0 / [double]$wallNs / 1000000.0
    $messageArgs = @(
        $round, $tag, $benchCase, $name,
        (Format-Ns ([double]$fields["op_ns"])),
        (Format-Ns ([double]$wallNs)),
        ($rateMillions.ToString("F1", $Invariant) + "M/s"),
        (Format-Bytes $peakWorkingSet)
    )
    Write-Host ("  r{0,-2} {1,-4} {2,-6} {3,-5} op={4,9} wall={5,9} {6,9} rss={7,8}" -f $messageArgs) -ForegroundColor $rowColor
    if ($ShowRawRecords) { Write-Host ("        " + $lines[0]) -ForegroundColor DarkGray }

    $p.Dispose()
    return [pscustomobject]@{
        Language = $fields["language"]; Bench = $fields["bench"]; FinalSize = $fields["final_size"]; Checksum = $fields["checksum"]
        CpuOpNs = [double]$fields["op_ns"]; WallNs = [double]$wallNs
        PeakRss = [double]$peakWorkingSet; PeakRssSource = $peakSource
    }
}

function Assert-Protocol([object[]]$records, [string]$benchCase, [int]$round) {
    $baseline = $records | Where-Object { $_.Language -eq "cpp26" } | Select-Object -First 1
    foreach ($record in $records) {
        if ($record.Checksum -ne $baseline.Checksum -or $record.FinalSize -ne $baseline.FinalSize) {
            throw ("Protocol mismatch in round " + $round + " for " + $benchCase + ": cpp26=" +
                $baseline.FinalSize + "/" + $baseline.Checksum + ", " + $record.Language + "=" +
                $record.FinalSize + "/" + $record.Checksum)
        }
    }
    Write-Host ("  [ok] {0} r{1}  size={2} checksum={3}" -f `
        $benchCase, $round, $baseline.FinalSize, $baseline.Checksum) -ForegroundColor Green
}

function Write-CaseTables([object[]]$Records, [string[]]$Cases) {
    foreach ($benchCase in $Cases) {
        $caseItems = @($Records | Where-Object { $_.Bench -eq $benchCase })
        if ($caseItems.Count -eq 0) { continue }
        Write-Host ""
        Write-Host ("== " + $benchCase + " " + ("=" * [Math]::Max(8, 62 - $benchCase.Length))) -ForegroundColor Cyan

        $stats = @()
        foreach ($lang in @("cpp26", "rust", "vyx")) {
            $items = @($caseItems | Where-Object { $_.Language -eq $lang })
            if ($items.Count -eq 0) { continue }
            $wall = [double[]]@($items | ForEach-Object { $_.WallNs })
            $cpuOp = [double[]]@($items | ForEach-Object { $_.CpuOpNs })
            $rssValues = [double[]]@(@($items | Where-Object { $_.PeakRssSource -ne "unavailable" }) |
                ForEach-Object { $_.PeakRss })
            $stats += [pscustomobject]@{
                Language = $lang
                Runs = $items.Count
                WallMed = (Median $wall)
                WallMin = ($wall | Measure-Object -Minimum).Minimum
                WallMax = ($wall | Measure-Object -Maximum).Maximum
                CpuMed = (Median $cpuOp)
                RssMed = $(if ($rssValues.Count -gt 0) { Median $rssValues } else { 0.0 })
                RssKnown = ($rssValues.Count -gt 0)
            }
        }
        if ($stats.Count -eq 0) { continue }
        $ordered = @($stats | Sort-Object WallMed)
        $base = @($ordered | Where-Object { $_.Language -eq "cpp26" }) | Select-Object -First 1
        if ($null -eq $base) { $base = $ordered[0] }

        # List[object[]] makes "one row = one element" explicit; immune to the += flattening trap.
        $rowList = New-Object 'System.Collections.Generic.List[object[]]'
        foreach ($s in $ordered) {
            $mops = [double]$Workload * 1000.0 / $s.WallMed
            $ratio = $base.WallMed / $s.WallMed
            $rowList.Add([object[]]@(
                $s.Language,
                ([string]$s.Runs),
                (Format-Ns $s.WallMed),
                (Format-Ns $s.WallMin),
                (Format-Ns $s.WallMax),
                ($mops.ToString("F1", $Invariant)),
                ($ratio.ToString("F2", $Invariant) + "x"),
                $(if ($s.RssKnown) { Format-Bytes $s.RssMed } else { "na" }),
                (Format-Ns $s.CpuMed)
            ))
        }
        Write-Table -Headers @("lang", "runs", "wall_med", "wall_min", "wall_max", "Mop/s", "vs_base", "rss_med", "cpu_op*") `
            -Rows $rowList.ToArray() -Align @("L", "R", "R", "R", "R", "R", "R", "R", "R") -Highlight 0
        $best = $ordered[0]
        $bestArgs = @(
            $best.Language,
            ([double]$Workload * 1000.0 / $best.WallMed).ToString("F1", $Invariant),
            (($base.WallMed / $best.WallMed).ToString("F2", $Invariant) + "x"),
            $base.Language
        )
        Write-Host ("  fastest: {0}  {1} Mop/s  ({2} vs {3}, wall median)" -f $bestArgs) -ForegroundColor Green
    }
    Write-Host ""
    Write-Host "  * cpu_op = diagnostic C clock() CPU time; ranking uses runner wall median." -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

Write-Banner

if (-not $SkipBuild) {
    $cmake = Resolve-Tool "cmake"
    $cargo = Resolve-Tool "cargo"
    if (-not (Test-Path -LiteralPath $boot)) { throw ("Missing current boot compiler: " + $boot) }
    $runtimeLib = if ($env:OS -eq "Windows_NT") {
        Join-Path $runtimeDir "vyx_runtime.lib"
    } else {
        Join-Path $runtimeDir "libvyx_runtime.a"
    }
    if (-not (Test-Path -LiteralPath $runtimeLib)) {
        throw ("Missing canonical application runtime: " + $runtimeLib)
    }
    if ((Test-Path -LiteralPath $cppBuild) -and -not $KeepBuild) {
        Remove-Item -LiteralPath $cppBuild -Recurse -Force
        Write-Host "  [prep] cleared previous cpp/build" -ForegroundColor DarkGray
    }
    New-Item -ItemType Directory -Force -Path $cppBuild | Out-Null

    $configLog = Join-Path $outDir "build_cmake_config.log"
    $configResult = Invoke-Captured "cmake-config" $cmake @("-S", $cppDir, "-B", $cppBuild,
        "-DCMAKE_BUILD_TYPE=Release", ("-DVYX_BENCH_PROFILE=" + $Profile)) $suite $configLog
    $configWarnCount = Count-LinesMatching ($configResult.StdOut + "`n" + $configResult.StdErr) "warning"
    $configNote = if ($configWarnCount -gt 0) { "$configWarnCount warning(s), see log" } else { "" }
    Write-StepOk "cmake-config" $configResult.ElapsedMs $configNote

    $buildLog = Join-Path $outDir "build_cmake_build.log"
    $buildResult = Invoke-Captured "cmake-build" $cmake @("--build", $cppBuild, "--config", "Release", "--parallel") $suite $buildLog
    $buildWarnCount = Count-LinesMatching ($buildResult.StdOut + "`n" + $buildResult.StdErr) "warning"
    $buildNote = if ($buildWarnCount -gt 0) { "$buildWarnCount warning(s), see log" } else { "" }
    Write-StepOk "cmake-build" $buildResult.ElapsedMs $buildNote

    $cargoArgs = @("build", "--manifest-path", (Join-Path $rustDir "Cargo.toml"))
    if ($useCargoNativeProfile) { $cargoArgs += @("--profile", "native-release") }
    else { $cargoArgs += "--release" }
    if (($Profile -eq "native-release") -and -not $useCargoNativeProfile) {
        Write-Host "  [cargo] Cargo.toml has no [profile.native-release]; using release (opt-level=3, no thin-LTO)" -ForegroundColor Yellow
    }
    $cargoLog = Join-Path $outDir "build_cargo.log"
    $cargoResult = Invoke-Captured "cargo" $cargo $cargoArgs $suite $cargoLog
    $cargoNote = ""
    if (-not $StreamToolOutput -and -not (($cargoResult.StdOut + $cargoResult.StdErr) -match "Compiling")) {
        $cargoNote = "up-to-date"
    }
    Write-StepOk "cargo" $cargoResult.ElapsedMs $cargoNote

    $moduleSources = @(
        "std_packages/core/src/option.vyx",
        "std_packages/core/src/result.vyx",
        "std_packages/core/src/default.vyx",
        "std_packages/core/src/clone.vyx",
        "std_packages/core/src/string.vyx",
        "std_packages/core/src/hash.vyx",
        "std_packages/core/src/ops.vyx",
        "std_packages/core/src/num.vyx",
        "std_packages/collections/src/collections.vyx",
        "std_packages/collections/src/iter.vyx",
        "std_packages/collections/src/vec.vyx",
        "std_packages/collections/src/dict.vyx"
    ) | ForEach-Object { Join-Path $root (Join-Path "bootstrap_compiler" $_) }
    $vyxOpt = if ($Profile -eq "native-release") { "-O3" } else { "-O2" }
    $vyxArgs = @("--src=file", (Join-Path $vyxDir "main.vyx"), "--emit=exe",
        $vyxOpt, "-o", $vyxExe, "-L", $runtimeDir, "-l", "vyx_runtime")
    foreach ($source in $moduleSources) { $vyxArgs += @("--module-source", $source) }
    # This is an application build. The compiler backend is intentionally not linked.
    $vyxLog = Join-Path $outDir "build_vyx.log"
    $bootResult = Invoke-Captured "vyx-boot" $boot $vyxArgs $root $vyxLog
    $bootWarnCount = Count-LinesMatching ($bootResult.StdOut + "`n" + $bootResult.StdErr) "warning"
    $bootNote = if ($bootWarnCount -gt 0) { "$bootWarnCount warning(s), see log" } else { "" }
    Write-StepOk "vyx-boot" $bootResult.ElapsedMs $bootNote
}
else {
    Write-Host "  [build] skipped (-SkipBuild)" -ForegroundColor Yellow
}

$cppExe = Join-Path $cppBuild "containers_bench.exe"
if (-not (Test-Path -LiteralPath $cppExe)) { $cppExe = Join-Path $cppBuild "Release\containers_bench.exe" }
$rustExe = if ($useCargoNativeProfile) {
    Join-Path $rustDir "target\native-release\containers_bench.exe"
} else {
    Join-Path $rustDir "target\release\containers_bench.exe"
}
foreach ($requiredExe in @($cppExe, $rustExe, $vyxExe)) {
    if (-not (Test-Path -LiteralPath $requiredExe)) { throw ("Missing benchmark executable: " + $requiredExe) }
}

$rawHeader = @("run_kind", "round", "order_index", "language", "bench", "workload",
    "live_set", "string_bytes", "seed", "reserve", "profile", "api_mode", "hash_policy",
    "timing_mode", "clock_rate", "setup_ticks", "op_ticks", "total_ticks", "setup_ns",
    "op_ns", "total_ns", "op_ops_per_sec", "final_size", "checksum", "peak_rss_bytes",
    "wall_ns", "wall_ms", "wall_ops_per_sec", "peak_rss_process_bytes", "peak_rss_source") -join [char]9
Archive-PreviousResults
Set-Content -LiteralPath $log -Encoding ascii -Value $rawHeader

$executables = @{
    cpp26 = $cppExe
    rust = $rustExe
    vyx = $vyxExe
}
$selectedCases = if ($Case -eq "all") { @("vec", "dict", "string") } else { @($Case) }
$measured = @()
$totalRounds = $Warmups + $Runs
$script:TotalRunCount = $totalRounds * $selectedCases.Count * $executables.Count

Write-Host ""
Write-Host ("running " + $script:TotalRunCount + " benchmark processes: " +
    $totalRounds + " rounds x " + $selectedCases.Count + " case(s) x " +
    $executables.Count + " languages") -ForegroundColor DarkGray

for ($round = 1; $round -le $totalRounds; $round++) {
    $runKind = if ($round -le $Warmups) { "warmup" } else { "measure" }
    $order = Get-RunOrder $round
    foreach ($benchCase in $selectedCases) {
        $caseRecords = @()
        for ($index = 0; $index -lt $order.Count; $index++) {
            $name = $order[$index]
            $record = Run-Measured $name $executables[$name] $benchCase $runKind $round ($index + 1)
            $caseRecords += $record
            if ($runKind -eq "measure") { $measured += $record }
        }
        Assert-Protocol $caseRecords $benchCase $round
    }
}

Write-CaseTables $measured $selectedCases

$summaryHeader = @("language", "bench", "profile", "hash_policy", "runs", "primary_timing",
    "median_wall_ns", "min_wall_ns", "max_wall_ns", "median_wall_ops_per_sec",
    "median_peak_rss_process_bytes", "peak_rss_samples", "diagnostic_timing",
    "median_cpu_op_ns", "min_cpu_op_ns", "max_cpu_op_ns", "median_cpu_op_ops_per_sec",
    "final_size", "checksum") -join [char]9
Set-Content -LiteralPath $summaryLog -Encoding ascii -Value $summaryHeader
foreach ($group in ($measured | Group-Object { $_.Language + "|" + $_.Bench })) {
    $items = @($group.Group)
    $cpuOp = @($items | ForEach-Object { $_.CpuOpNs })
    $wall = @($items | ForEach-Object { $_.WallNs })
    $rss = @($items | Where-Object { $_.PeakRssSource -ne "unavailable" } | ForEach-Object { $_.PeakRss })
    $medianWall = Median $wall
    $wallRate = ([double]$Workload * 1000000000.0 / $medianWall).ToString("F3", [Globalization.CultureInfo]::InvariantCulture)
    $medianCpuOp = Median $cpuOp
    $cpuRate = if ($medianCpuOp -gt 0) { ([double]$Workload * 1000000000.0 / $medianCpuOp).ToString("F3", [Globalization.CultureInfo]::InvariantCulture) } else { "na" }
    $medianRss = if ($rss.Count -gt 0) { ([UInt64][Math]::Round((Median $rss))).ToString() } else { "na" }
    $first = $items[0]
    $row = @($first.Language, $first.Bench, $Profile, $HashPolicy, $items.Count, "runner_process_wall",
        ([UInt64][Math]::Round($medianWall)), ([UInt64][Math]::Round(($wall | Measure-Object -Minimum).Minimum)),
        ([UInt64][Math]::Round(($wall | Measure-Object -Maximum).Maximum)), $wallRate,
        $medianRss, $rss.Count, "c_clock_cpu_diagnostic",
        ([UInt64][Math]::Round($medianCpuOp)), ([UInt64][Math]::Round(($cpuOp | Measure-Object -Minimum).Minimum)),
        ([UInt64][Math]::Round(($cpuOp | Measure-Object -Maximum).Maximum)), $cpuRate,
        $first.FinalSize, $first.Checksum) -join [char]9
    Add-Content -LiteralPath $summaryLog -Encoding ascii -Value $row
}

$bootHash = if (Test-Path -LiteralPath $boot) { (Get-FileHash -Algorithm SHA256 -LiteralPath $boot).Hash } else { "missing" }
$runtimeCandidates = @((Join-Path $runtimeDir "vyx_runtime.lib"), (Join-Path $runtimeDir "libvyx_runtime.a"))
$runtimePath = $runtimeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$runtimeHash = if ($runtimePath) { (Get-FileHash -Algorithm SHA256 -LiteralPath $runtimePath).Hash } else { "missing" }
$cpu = $env:PROCESSOR_IDENTIFIER
try { $cpu = (Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name) -join "; " } catch { }
$rustFlagsText = if ($useCargoNativeProfile) { "Rust:opt-level=3,lto=thin" } else { "Rust:opt-level=3,release-profile" }
$environment = [ordered]@{
    generated_utc = [DateTime]::UtcNow.ToString("o")
    os = [Environment]::OSVersion.VersionString
    cpu = $cpu
    profile = $Profile
    compiler_flags = if ($Profile -eq "matched-o2") { "C++:/O2-or-O2;$rustFlagsText".Replace($rustFlagsText, "Rust:opt-level=2,lto=false") + ";Vyx:-O2" } else { "C++:/O2-or-O3;$rustFlagsText;Vyx:-O3" }
    cargo_profile = if ($useCargoNativeProfile) { "native-release" } else { "release" }
    workload = $Workload; live_set = $LiveSet; string_bytes = $StringBytes; seed = $Seed
    reserve = $reserveValue; runs = $Runs; warmups = $Warmups; clock_rate = $ClockRate
    api_mode = "safe"; hash_policy = $HashPolicy
    primary_timing = "runner process wall time (process start through exit; includes setup)"
    diagnostic_timing = "C clock CPU time; retained for diagnostics, not ranking"
    dict_hash_note = if ($HashPolicy -eq "deterministic-splitmix64") { "All languages use the Vyx hash_i64 transform; table and bucket policies remain native." } else { "Each language uses its standard native integer hasher; do not mix with deterministic results." }
    allocator = "C++/Rust system allocator; Vyx canonical runtime allocator"
    peak_rss_metric = "runner process peak; self-report fallback; unavailable is not zero"
    result_archive_policy = "Previous top-level TSV/JSON files are moved to out/archive/<UTC timestamp> before a new run."
    cmake = Get-CommandVersion (Resolve-Tool "cmake") @("--version")
    rustc = Get-CommandVersion (Resolve-Tool "rustc") @("--version")
    cargo = Get-CommandVersion (Resolve-Tool "cargo") @("--version")
    git_commit = Get-CommandVersion (Resolve-Tool "git") @("rev-parse", "HEAD")
    boot = $boot; boot_sha256 = $bootHash
    runtime = $runtimePath; runtime_sha256 = $runtimeHash
    compiler_backend_linked = $false
}
$environment | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $environmentLog -Encoding utf8

$totalSeconds = ([DateTime]::UtcNow - $scriptStartTime).TotalSeconds
Write-Host ""
Write-Host ("completed in " + $totalSeconds.ToString("F1", $Invariant) + "s") -ForegroundColor Green
Write-Host ("  raw tsv  : " + $log)
Write-Host ("  summary  : " + $summaryLog)
Write-Host ("  environ  : " + $environmentLog)
Write-Host ("  build log: " + (Join-Path $outDir "build_*.log"))

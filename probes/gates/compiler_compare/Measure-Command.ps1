param(
    [Parameter(Mandatory)][string]$Executable,
    [AllowEmptyCollection()][string[]]$ArgumentList = @(),
    [Parameter(Mandatory)][string]$WorkingDirectory,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Label = '',
    [ValidateRange(50, 10000)][int]$SampleIntervalMs = 100
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or $PSVersionTable.PSVersion.Major -lt 7) {
    throw 'This observer requires Windows and PowerShell 7.'
}
if (Test-Path -LiteralPath $Executable -PathType Leaf) {
    $executablePath = (Resolve-Path -LiteralPath $Executable).Path
} else {
    $executablePath = (Get-Command $Executable -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
}
$workingPath = (Resolve-Path -LiteralPath $WorkingDirectory).Path
if (-not (Test-Path -LiteralPath $workingPath -PathType Container)) { throw 'WorkingDirectory must be a directory.' }
$outputPath = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $outputPath) { throw "Refusing to overwrite measurement output: $outputPath" }
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

if (-not ('CompilerCommandObservation' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class CompilerCommandObservation {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct PROCESSENTRY32 {
        public uint dwSize, cntUsage, th32ProcessID;
        public UIntPtr th32DefaultHeapID;
        public uint th32ModuleID, cntThreads, th32ParentProcessID;
        public int pcPriClassBase;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct MEMORY_COUNTERS {
        public uint cb, PageFaultCount;
        public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage,
            QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage,
            PagefileUsage, PeakPagefileUsage, PrivateUsage;
    }
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool Process32FirstW(IntPtr snapshot, ref PROCESSENTRY32 entry);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool Process32NextW(IntPtr snapshot, ref PROCESSENTRY32 entry);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder path, ref uint size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetProcessTimes(IntPtr process, out long created, out long exited, out long kernel, out long user);
    [DllImport("psapi.dll", SetLastError = true)] static extern bool GetProcessMemoryInfo(IntPtr process, ref MEMORY_COUNTERS counters, uint size);

    public sealed class Sample {
        public int Id, ParentId;
        public string Name, Path;
        public long StartTicks, RssBytes, PrivateBytes, PeakRssBytes, PeakPrivateBytes;
        public double CpuMilliseconds;
        public bool MemoryAvailable;
    }
    static Sample ReadHandle(IntPtr handle, int id, int parentId, string name) {
        long created, exited, kernel, user;
        if (!GetProcessTimes(handle, out created, out exited, out kernel, out user))
            throw new System.ComponentModel.Win32Exception();
        var counters = new MEMORY_COUNTERS { cb = (uint)Marshal.SizeOf(typeof(MEMORY_COUNTERS)) };
        bool memoryAvailable = GetProcessMemoryInfo(handle, ref counters, counters.cb);
        var buffer = new StringBuilder(32768);
        uint size = (uint)buffer.Capacity;
        string path = QueryFullProcessImageNameW(handle, 0, buffer, ref size) ? buffer.ToString() : "";
        return new Sample {
            Id = id, ParentId = parentId, Name = name, Path = path,
            StartTicks = DateTime.FromFileTimeUtc(created).Ticks,
            CpuMilliseconds = (kernel + user) / 10000.0,
            MemoryAvailable = memoryAvailable,
            RssBytes = (long)counters.WorkingSetSize.ToUInt64(),
            PrivateBytes = (long)counters.PrivateUsage.ToUInt64(),
            PeakRssBytes = (long)counters.PeakWorkingSetSize.ToUInt64(),
            PeakPrivateBytes = (long)counters.PeakPagefileUsage.ToUInt64()
        };
    }
    public static Sample ReadRoot(Process root) {
        // Keep the original process handle alive across exit: never reopen by PID.
        return ReadHandle(root.Handle, root.Id, -1, System.IO.Path.GetFileNameWithoutExtension(root.StartInfo.FileName));
    }
    public static double RootWallSeconds(Process root) {
        long created, exited, kernel, user;
        if (!GetProcessTimes(root.Handle, out created, out exited, out kernel, out user))
            throw new System.ComponentModel.Win32Exception();
        return (exited - created) / 10000000.0;
    }
    public static Sample[] ReadTree(int root, long rootStartTicks, Dictionary<int, long> known) {
        var entries = new Dictionary<int, PROCESSENTRY32>();
        IntPtr snapshot = CreateToolhelp32Snapshot(2, 0);
        if (snapshot == new IntPtr(-1)) throw new System.ComponentModel.Win32Exception();
        try {
            var entry = new PROCESSENTRY32 { dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32)) };
            if (Process32FirstW(snapshot, ref entry)) do {
                entries[(int)entry.th32ProcessID] = entry;
            } while (Process32NextW(snapshot, ref entry));
        } finally { CloseHandle(snapshot); }
        var samples = new Dictionary<int, Sample>();
        // Only open root/known descendants or children of a validated member.
        // Reading the snapshot does not collect unrelated paths or command lines.
        var attempted = new HashSet<int>();
        bool changed;
        do {
            changed = false;
            foreach (var pair in entries) {
                int id = pair.Key;
                int parentId = (int)pair.Value.th32ParentProcessID;
                if (attempted.Contains(id)) continue;
                bool candidate = id == root || known.ContainsKey(id) || samples.ContainsKey(parentId);
                if (!candidate) continue;
                attempted.Add(id);
                try {
                    using (Process process = Process.GetProcessById(id)) {
                        var observation = ReadHandle(process.Handle, id, parentId,
                            System.IO.Path.GetFileNameWithoutExtension(pair.Value.szExeFile));
                        bool sameRoot = id == root && observation.StartTicks == rootStartTicks;
                        bool retained = known.TryGetValue(id, out long prior) && prior == observation.StartTicks;
                        bool descendant = samples.TryGetValue(parentId, out Sample parent)
                            && observation.StartTicks >= parent.StartTicks;
                        if (!sameRoot && !retained && !descendant) continue;
                        // In particular, a reused root PID is not automatically a member.
                        samples.Add(id, observation);
                        known[id] = observation.StartTicks;
                        changed = true;
                    }
                } catch (InvalidOperationException) { }
                  catch (ArgumentException) { }
                  catch (System.ComponentModel.Win32Exception) { }
            }
        } while (changed);
        var result = new Sample[samples.Count];
        samples.Values.CopyTo(result, 0);
        return result;
    }
}
'@
}

$machine = Get-CimInstance Win32_ComputerSystem
$cpu = @(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
$metadata = [ordered]@{
    schema_version = 1
    started_utc = [DateTime]::UtcNow.ToString('o')
    executable = $executablePath
    executable_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
    argv = @($ArgumentList)
    working_directory = $workingPath
    label = $Label
    sample_interval_ms = $SampleIntervalMs
    os = [Environment]::OSVersion.VersionString
    powershell_version = $PSVersionTable.PSVersion.ToString()
    logical_processors = $machine.NumberOfLogicalProcessors
    physical_memory_bytes = $machine.TotalPhysicalMemory
    cpu = $cpu
    limitations = @(
        'Only descendants observed during the root command lifetime are measured; short-lived children and unobserved tails can be missed.',
        'CPU is the sum of latest observed cumulative CPU times keyed by PID and creation time, plus root final CPU: a lower bound.',
        'Tree RSS counts shared pages once per process; it is not unique physical memory. Tree peaks are simultaneous sampled sums.',
        'Parent peaks are Windows lifetime counters, queried using the retained original process handle, including a final query after exit.',
        'Peak private uses PeakPagefileUsage (private commit), not peak physical residency. Missing memory queries are counted.',
        'Compiler/tool categories are process-name classifications, not proof that a process is busy doing compilation.',
        'Sampling adds overhead reported as sample_work_ms. No environment variables or unrelated process command lines are collected.',
        'Only the explicitly supplied argv is recorded; the caller must not include credentials in arguments.',
        'No source changes, cache cleaning, build setup, or executable version invocation is performed.'
    )
}
$metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputPath 'metadata.json') -Encoding utf8NoBOM

function Get-ObservedCategory([string]$Name) {
    if ($Name -match '^(boot(?:[_-].*)?|vyxc(?:[_-].*)?)$') { return 'vyx_compiler' }
    if ($Name -in @('clang', 'clang++', 'clang-cl', 'cl', 'cc1', 'cc1plus', 'gcc', 'g++', 'llc', 'rustc')) { return 'native_compiler' }
    if ($Name -in @('lld', 'lld-link', 'ld', 'link', 'llvm-ar', 'llvm-lib', 'ar', 'lib')) { return 'linker_archiver' }
    if ($Name -in @('cmake', 'ninja', 'ninja-build', 'msbuild', 'make')) { return 'build_driver' }
    return 'other'
}

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $executablePath
$startInfo.WorkingDirectory = $workingPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in $ArgumentList) { $startInfo.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo
$stdoutFile = [IO.FileStream]::new((Join-Path $outputPath 'stdout.log'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$stderrFile = [IO.FileStream]::new((Join-Path $outputPath 'stderr.log'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$csv = [IO.StreamWriter]::new((Join-Path $outputPath 'samples.csv'), $false, [Text.UTF8Encoding]::new($false))
$csv.AutoFlush = $true
$csv.WriteLine('elapsed_ms,tree_processes,active_children,vyx_compiler_children,native_compiler_children,linker_archiver_children,build_driver_children,other_children,parent_rss_bytes,parent_private_bytes,tree_rss_bytes,tree_private_bytes,observed_cpu_ms,memory_queries_failed,sample_work_ms')
$known = [Collections.Generic.Dictionary[int, long]]::new()
$latest = @{}
$parentPeakRss = 0L; $parentPeakPrivate = 0L
$treePeakRss = 0L; $treePeakPrivate = 0L
$peaks = @{ active = 0; vyx_compiler = 0; native_compiler = 0; linker_archiver = 0; build_driver = 0; other = 0 }
$sampleCount = 0; $sampleCostMs = 0.0; $memoryQueryFailures = 0; $cpuMs = 0.0
$clock = [Diagnostics.Stopwatch]::StartNew()
$exitCode = $null
Write-Host "Measuring $executablePath [$Label]. Logs: $outputPath"
try {
    if (-not $process.Start()) { throw 'Measured command did not start.' }
    # Force a retained handle before the target can exit/reuse its PID.
    $null = $process.Handle
    $initialRoot = [CompilerCommandObservation]::ReadRoot($process)
    $rootStartTicks = $initialRoot.StartTicks
    $rootKey = "$($process.Id):$rootStartTicks"
    $latest[$rootKey] = $initialRoot
    $known[$process.Id] = $rootStartTicks
    $parentPeakRss = $initialRoot.PeakRssBytes
    $parentPeakPrivate = $initialRoot.PeakPrivateBytes
    $stdoutTask = $process.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
    $stderrTask = $process.StandardError.BaseStream.CopyToAsync($stderrFile)
    do {
        $sampleClock = [Diagnostics.Stopwatch]::StartNew()
        $observations = [CompilerCommandObservation]::ReadTree($process.Id, $rootStartTicks, $known)
        $rss = 0L; $private = 0L; $parentRss = 0L; $parentPrivate = 0L; $queryFailures = 0
        $counts = @{ active = 0; vyx_compiler = 0; native_compiler = 0; linker_archiver = 0; build_driver = 0; other = 0 }
        foreach ($observation in $observations) {
            $key = "$($observation.Id):$($observation.StartTicks)"
            if ($latest.ContainsKey($key)) {
                if (-not $observation.Path) { $observation.Path = $latest[$key].Path }
                $observation.PeakRssBytes = [Math]::Max($observation.PeakRssBytes, $latest[$key].PeakRssBytes)
                $observation.PeakPrivateBytes = [Math]::Max($observation.PeakPrivateBytes, $latest[$key].PeakPrivateBytes)
            }
            $latest[$key] = $observation
            $rss += $observation.RssBytes
            $private += $observation.PrivateBytes
            if (-not $observation.MemoryAvailable) { $queryFailures++ }
            if ($key -eq $rootKey) {
                $parentRss = $observation.RssBytes
                $parentPrivate = $observation.PrivateBytes
                $parentPeakRss = [Math]::Max($parentPeakRss, $observation.PeakRssBytes)
                $parentPeakPrivate = [Math]::Max($parentPeakPrivate, $observation.PeakPrivateBytes)
            } else {
                $counts.active++
                $category = Get-ObservedCategory $observation.Name
                $counts[$category]++
            }
        }
        $cpuMs = 0.0
        foreach ($observation in $latest.Values) { $cpuMs += $observation.CpuMilliseconds }
        $treePeakRss = [Math]::Max($treePeakRss, $rss)
        $treePeakPrivate = [Math]::Max($treePeakPrivate, $private)
        foreach ($category in @($counts.Keys)) { $peaks[$category] = [Math]::Max($peaks[$category], $counts[$category]) }
        $workMs = $sampleClock.Elapsed.TotalMilliseconds
        $sampleCostMs += $workMs
        $sampleCount++
        $memoryQueryFailures += $queryFailures
        $row = @($clock.ElapsedMilliseconds, $observations.Count, $counts.active, $counts.vyx_compiler, $counts.native_compiler, $counts.linker_archiver, $counts.build_driver, $counts.other, $parentRss, $parentPrivate, $rss, $private, $cpuMs, $queryFailures, $workMs)
        $csv.WriteLine(($row | ForEach-Object { [Convert]::ToString($_, [Globalization.CultureInfo]::InvariantCulture) }) -join ',')
        if ($process.HasExited) { break }
        $remainingMs = [Math]::Max(1, $SampleIntervalMs - [int]$sampleClock.ElapsedMilliseconds)
        $null = $process.WaitForExit($remainingMs)
    } while ($true)
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $finalRoot = [CompilerCommandObservation]::ReadRoot($process)
    if ($latest.ContainsKey($rootKey)) {
        $finalRoot.ParentId = $latest[$rootKey].ParentId
        $finalRoot.Name = $latest[$rootKey].Name
        if (-not $finalRoot.Path) { $finalRoot.Path = $latest[$rootKey].Path }
        $finalRoot.PeakRssBytes = [Math]::Max($finalRoot.PeakRssBytes, $latest[$rootKey].PeakRssBytes)
        $finalRoot.PeakPrivateBytes = [Math]::Max($finalRoot.PeakPrivateBytes, $latest[$rootKey].PeakPrivateBytes)
    }
    $latest[$rootKey] = $finalRoot
    $parentPeakRss = [Math]::Max($parentPeakRss, $finalRoot.PeakRssBytes)
    $parentPeakPrivate = [Math]::Max($parentPeakPrivate, $finalRoot.PeakPrivateBytes)
    $cpuMs = 0.0
    foreach ($observation in $latest.Values) { $cpuMs += $observation.CpuMilliseconds }
    $null = $stdoutTask.GetAwaiter().GetResult()
    $null = $stderrTask.GetAwaiter().GetResult()
    $clock.Stop()
    $wall = [CompilerCommandObservation]::RootWallSeconds($process)
    $summary = [ordered]@{
        exit_code = $exitCode
        wall_seconds = $wall
        harness_elapsed_seconds = $clock.Elapsed.TotalSeconds
        parent_pid = $process.Id
        parent_start_ticks = $rootStartTicks
        parent_peak_rss_bytes = $parentPeakRss
        parent_peak_private_bytes = $parentPeakPrivate
        parent_final_memory_query_succeeded = $finalRoot.MemoryAvailable
        parent_final_cpu_seconds = $finalRoot.CpuMilliseconds / 1000.0
        tree_sampled_peak_rss_bytes = $treePeakRss
        tree_sampled_peak_private_bytes = $treePeakPrivate
        peak_active_children = $peaks.active
        peak_active_vyx_compiler_children = $peaks.vyx_compiler
        peak_active_native_compiler_children = $peaks.native_compiler
        peak_active_linker_archiver_children = $peaks.linker_archiver
        peak_active_build_driver_children = $peaks.build_driver
        peak_active_other_children = $peaks.other
        observed_process_count = $latest.Count
        observed_cpu_seconds = $cpuMs / 1000.0
        observed_cpu_over_wall = $(if ($wall -gt 0) { $cpuMs / 1000.0 / $wall } else { 0 })
        sample_count = $sampleCount
        sampled_memory_query_failures = $memoryQueryFailures
        sample_work_ms = $sampleCostMs
        average_sample_work_ms = $sampleCostMs / [Math]::Max(1, $sampleCount)
    }
    [ordered]@{ metadata = $metadata; summary = $summary } | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $outputPath 'result.json') -Encoding utf8NoBOM
    $latest.Values | Sort-Object StartTicks, Id | Select-Object Id, ParentId, Name, Path, StartTicks,
        @{ Name = 'Category'; Expression = { Get-ObservedCategory $_.Name } },
        CpuMilliseconds, RssBytes, PrivateBytes, PeakRssBytes, PeakPrivateBytes, MemoryAvailable |
        Export-Csv -LiteralPath (Join-Path $outputPath 'processes.csv') -NoTypeInformation -Encoding utf8NoBOM
    Write-Host ("Exit={0}; wall={1:N3}s; tree peak RSS={2:N1}MiB; observed CPU={3:N3}s; peak children={4}" -f $exitCode, $wall, ($treePeakRss / 1MB), ($cpuMs / 1000), $peaks.active)
} finally {
    $csv.Dispose()
    $stdoutFile.Dispose()
    $stderrFile.Dispose()
    $process.Dispose()
}
if ($null -ne $exitCode) { exit $exitCode }

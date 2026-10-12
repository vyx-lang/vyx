param(
    [Parameter(Mandatory)][string]$Compiler,
    [Parameter(Mandatory)][string]$ProjectDir,
    [string]$Target = '',
    [ValidateRange(1, 1024)][int]$Jobs = 20,
    [string]$OutputDir = (Join-Path $PSScriptRoot ('.runs/measure-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))),
    [string[]]$BuildArgs = @(),
    [string[]]$CompilerArgs = @(),
    [string]$CacheLabel = 'preserved; caller managed',
    [ValidateRange(50, 10000)][int]$SampleIntervalMs = 100
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'This harness currently requires Windows and PowerShell 7.' }
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$projectPath = (Resolve-Path -LiteralPath $ProjectDir).Path
$outputPath = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $outputPath) { throw "Refusing to overwrite measurement output: $outputPath" }
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

if (-not ('VyxBuildObservation' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

public static class VyxBuildObservation {
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
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel, out long user);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr process, uint milliseconds);
    [DllImport("psapi.dll", SetLastError = true)] static extern bool GetProcessMemoryInfo(IntPtr process, ref MEMORY_COUNTERS counters, uint size);
    public sealed class Sample {
        public int Id, ParentId;
        public string Name;
        public long StartTicks, RssBytes, PrivateBytes, PeakRssBytes, PeakPrivateBytes;
        public double CpuMilliseconds;
    }
    public static IntPtr OpenWaitHandle(int id) {
        IntPtr handle = OpenProcess(0x101000, false, id);
        if (handle == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        return handle;
    }
    public static bool HasTerminated(IntPtr handle) {
        uint status = WaitForSingleObject(handle, 0);
        if (status == 0) return true;
        if (status == 258) return false;
        throw new System.ComponentModel.Win32Exception();
    }
    public static void CloseWaitHandle(IntPtr handle) { if (handle != IntPtr.Zero) CloseHandle(handle); }
    static Sample ReadProcess(int id, int parent, string name) {
        // Process.GetProcessById/Handle treats a published exit status as
        // completion. Windows can still be running DLL detach with a live
        // address space and an unsignaled process handle at that point.
        IntPtr handle = OpenProcess(0x1000, false, id);
        if (handle == IntPtr.Zero) return null;
        try {
            long creation, exit, kernel, user;
            var counters = new MEMORY_COUNTERS { cb = (uint)Marshal.SizeOf(typeof(MEMORY_COUNTERS)) };
            if (!GetProcessTimes(handle, out creation, out exit, out kernel, out user)
                || !GetProcessMemoryInfo(handle, ref counters, counters.cb)) return null;
            return new Sample {
                Id = id, ParentId = parent, Name = System.IO.Path.GetFileNameWithoutExtension(name),
                StartTicks = DateTime.FromFileTimeUtc(creation).Ticks,
                CpuMilliseconds = (kernel + user) / 10000.0,
                RssBytes = (long)counters.WorkingSetSize.ToUInt64(),
                PrivateBytes = (long)counters.PrivateUsage.ToUInt64(),
                PeakRssBytes = (long)counters.PeakWorkingSetSize.ToUInt64(),
                PeakPrivateBytes = (long)counters.PeakPagefileUsage.ToUInt64()
            };
        } finally { CloseHandle(handle); }
    }
    public static Sample[] ReadTree(int root, Dictionary<int, long> known) {
        var parents = new Dictionary<int, int>();
        var names = new Dictionary<int, string>();
        IntPtr snapshot = CreateToolhelp32Snapshot(2, 0);
        if (snapshot == new IntPtr(-1)) throw new System.ComponentModel.Win32Exception();
        try {
            var entry = new PROCESSENTRY32 { dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32)) };
            if (Process32FirstW(snapshot, ref entry)) do {
                parents[(int)entry.th32ProcessID] = (int)entry.th32ParentProcessID;
                names[(int)entry.th32ProcessID] = entry.szExeFile;
            } while (Process32NextW(snapshot, ref entry));
        } finally { CloseHandle(snapshot); }
        var samples = new Dictionary<int, Sample>();
        var members = new Dictionary<int, Sample>();
        if (parents.ContainsKey(root)) {
            var rootSample = ReadProcess(root, parents[root], names[root]);
            if (rootSample != null) { samples[root] = rootSample; members[root] = rootSample; }
        }
        long rootStart;
        if (members.ContainsKey(root)) rootStart = members[root].StartTicks;
        else if (!known.TryGetValue(root, out rootStart)) return new Sample[0];
        // A Windows PID can be reused after a child exits. Only retain an
        // orphaned descendant when its process creation time still matches.
        foreach (var pair in known) {
            if (!parents.ContainsKey(pair.Key)) continue;
            var prior = ReadProcess(pair.Key, parents[pair.Key], names[pair.Key]);
            if (prior != null && prior.StartTicks == pair.Value && prior.StartTicks >= rootStart) {
                samples[pair.Key] = prior;
                members[pair.Key] = prior;
            }
        }
        bool changed;
        do {
            changed = false;
            foreach (var pair in parents) {
                if (members.ContainsKey(pair.Key) || !members.ContainsKey(pair.Value)) continue;
                Sample child;
                if (!samples.TryGetValue(pair.Key, out child)) {
                    child = ReadProcess(pair.Key, pair.Value, names[pair.Key]);
                    samples[pair.Key] = child;
                }
                // The snapshot stores numeric parent PIDs, not parent
                // identities. A surviving process can retain a parent PID
                // that Windows has since reused for a new compiler. It
                // cannot be this parent's child if it predates the parent.
                if (child != null && child.StartTicks >= members[pair.Value].StartTicks) {
                    members[pair.Key] = child;
                    changed = true;
                }
            }
        } while (changed);
        var result = new List<Sample>();
        foreach (int id in members.Keys) {
            if (!parents.ContainsKey(id)) continue;
            var sample = members[id];
            if (sample == null) continue;
            known[id] = sample.StartTicks;
            result.Add(sample);
        }
        return result.ToArray();
    }
}
'@
}

$argv = [Collections.Generic.List[string]]::new()
if ($CompilerArgs.Count -gt 0) {
    foreach ($argument in $CompilerArgs) { $argv.Add($argument) }
} else {
    $argv.Add('build')
    if ($Target) { $argv.Add('--target'); $argv.Add($Target) }
    $argv.Add("-j$Jobs")
    foreach ($argument in $BuildArgs) { $argv.Add($argument) }
}

$machine = Get-CimInstance Win32_ComputerSystem
$cpu = @(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
$commit = (& git -C $projectPath rev-parse HEAD 2>$null | Out-String).Trim()
$compilerVersion = (& $compilerPath --version 2>&1 | Out-String).Trim()
$compilerDirectory = Split-Path -Parent $compilerPath
$nativeArtifacts = @()
foreach ($entry in @(
    @{ Name = 'vyx_compiler_backend.dll'; Role = 'compiler_backend_dll' },
    @{ Name = 'vyx_runtime.lib'; Role = 'runtime_link_library' },
    @{ Name = 'tbb12.dll'; Role = 'tbb_runtime_dll' }
)) {
    $artifactPath = Join-Path $compilerDirectory $entry.Name
    if (Test-Path -LiteralPath $artifactPath -PathType Leaf) {
        $artifactFile = Get-Item -LiteralPath $artifactPath
        $nativeArtifacts += [ordered]@{
            role = $entry.Role
            path = $artifactFile.FullName
            bytes = $artifactFile.Length
            modified_utc = $artifactFile.LastWriteTimeUtc.ToString('o')
            sha256 = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash
        }
    }
}

# Limit provenance to compiler inputs. Do not inspect unrelated project state,
# editor/session settings, environment files, or repository-root dirty paths.
$sourcePaths = @(
    'bootstrap_compiler/src', 'bootstrap_compiler/std',
    'bootstrap_compiler/Vyx.toml', 'bootstrap_compiler/CMakeLists.txt',
    'vyx_codegen/src', 'vyx_codegen/include', 'vyx_codegen/CMakeLists.txt',
    'tools/dci'
)
$sourceRepo = (& git -C $compilerDirectory rev-parse --show-toplevel 2>$null | Out-String).Trim()
$sourceIdentity = $null
if ($LASTEXITCODE -eq 0 -and $sourceRepo) {
    $sourceCommit = (& git -C $sourceRepo rev-parse HEAD 2>$null | Out-String).Trim()
    $statusText = (& git -C $sourceRepo -c core.quotepath=false status --porcelain=v1 -z --untracked-files=all -- @sourcePaths 2>$null | Out-String -NoNewline)
    $dirty = @()
    $statusEntries = $statusText.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)
    for ($index = 0; $index -lt $statusEntries.Length; $index++) {
        $statusEntry = $statusEntries[$index]
        if ($statusEntry.Length -lt 4) { continue }
        $status = $statusEntry.Substring(0, 2)
        $relativePath = $statusEntry.Substring(3)
        $row = [ordered]@{ status = $status; path = $relativePath }
        if ($status.Contains('R') -or $status.Contains('C')) {
            $index++
            if ($index -lt $statusEntries.Length) { $row.previous_path = $statusEntries[$index] }
        }
        $dirtyPath = Join-Path $sourceRepo $relativePath
        if (Test-Path -LiteralPath $dirtyPath -PathType Leaf) {
            $row.sha256 = (Get-FileHash -LiteralPath $dirtyPath -Algorithm SHA256).Hash
        }
        $dirty += $row
    }
    $diffFile = Join-Path $outputPath 'compiler-source.diff'
    $gitInfo = [Diagnostics.ProcessStartInfo]::new()
    $gitInfo.FileName = (Get-Command git -ErrorAction Stop).Source
    $gitInfo.UseShellExecute = $false
    $gitInfo.CreateNoWindow = $true
    $gitInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $gitInfo.RedirectStandardOutput = $true
    $gitInfo.RedirectStandardError = $true
    foreach ($argument in @('-C', $sourceRepo, 'diff', '--binary', '--no-ext-diff', '--no-textconv', 'HEAD', '--') + $sourcePaths) {
        $gitInfo.ArgumentList.Add($argument)
    }
    $gitProcess = [Diagnostics.Process]::Start($gitInfo)
    $diffStream = [IO.File]::Create($diffFile)
    try {
        $diffTask = $gitProcess.StandardOutput.BaseStream.CopyToAsync($diffStream)
        $gitErrorTask = $gitProcess.StandardError.ReadToEndAsync()
        $gitProcess.WaitForExit()
        $null = $diffTask.GetAwaiter().GetResult()
        $gitError = $gitErrorTask.GetAwaiter().GetResult()
        if ($gitProcess.ExitCode -ne 0) { throw "Cannot capture compiler source diff: $gitError" }
    } finally {
        $diffStream.Dispose()
        $gitProcess.Dispose()
    }
    $sourceIdentity = [ordered]@{
        repository = $sourceRepo
        commit = $sourceCommit
        included_paths = $sourcePaths
        dirty_paths = $dirty
        diff_file = 'compiler-source.diff'
        diff_sha256 = (Get-FileHash -LiteralPath $diffFile -Algorithm SHA256).Hash
        diff_stat = (& git -C $sourceRepo diff --stat --no-ext-diff --no-textconv HEAD -- @sourcePaths 2>$null | Out-String).Trim()
        scope_note = 'Tracked staged/unstaged changes are compared with HEAD. Untracked compiler files are listed with content hashes.'
    }
}
$llvmRoot = $env:LLVM_ROOT
$clangVersion = $null
if ($llvmRoot -and (Test-Path -LiteralPath (Join-Path $llvmRoot 'bin/clang.exe'))) {
    $clangVersion = (& (Join-Path $llvmRoot 'bin/clang.exe') --version 2>&1 | Out-String).Trim()
}
$metadata = [ordered]@{
    schema_version = 2
    started_utc = [DateTime]::UtcNow.ToString('o')
    compiler = $compilerPath
    compiler_sha256 = (Get-FileHash -LiteralPath $compilerPath -Algorithm SHA256).Hash
    compiler_version = $compilerVersion
    compiler_directory_artifacts = $nativeArtifacts
    compiler_source_identity = $sourceIdentity
    project = $projectPath
    commit = $commit
    argv = @($argv)
    jobs = $Jobs
    target = $Target
    cache_label = $CacheLabel
    cache_policy = 'No cleaning or invalidation. The caller prepares cold/warm/no-op state.'
    sample_interval_ms = $SampleIntervalMs
    os = [Environment]::OSVersion.VersionString
    logical_processors = $machine.NumberOfLogicalProcessors
    physical_memory_bytes = $machine.TotalPhysicalMemory
    cpu = $cpu
    llvm_root = $llvmRoot
    clang_version = $clangVersion
    compiler_environment = [ordered]@{
        VYX_CODEGEN_UNITS = $env:VYX_CODEGEN_UNITS
        VYX_CGU_EMIT_THREADS = $env:VYX_CGU_EMIT_THREADS
        VYX_CGU_THREADS = $env:VYX_CGU_THREADS
        VYX_BUILD_MEMORY_RESERVE_MB = $env:VYX_BUILD_MEMORY_RESERVE_MB
        VYX_BUILD_MEMORY_SLOTS = $env:VYX_BUILD_MEMORY_SLOTS
        VYX_BOOTSTRAP_PROFILE = $env:VYX_BOOTSTRAP_PROFILE
        VYX_MIR_HEAP_WALK = $env:VYX_MIR_HEAP_WALK
        VYX_MIR_TIMING = $env:VYX_MIR_TIMING
        VYX_LLVM_TIMING = $env:VYX_LLVM_TIMING
    }
    limitations = @(
        'Tree metrics cover descendants observed during the compiler process lifetime. Very short children can be missed.',
        'CPU is the sum of the latest observed cumulative CPU times, keyed by PID and start time. It undercounts unobserved process tails.',
        'Tree RSS sums process working sets, including shared pages more than once. It is not unique physical memory.',
        'Parent peak RSS/private use Windows process lifetime peaks; tree peaks are simultaneous sampled sums.',
        'Private peak uses Windows PeakPagefileUsage (process private commit peak), not peak physical residency.',
        'Toolhelp snapshots plus process queries add sampling overhead; sample_work_ms reports observation cost.',
        'No unrelated process arguments, command lines, or environment variables are collected.'
        'Native artifact hashes describe files beside the compiler at measurement start; runtime_link_library is an on-disk static link input, not proof of the compiler embedded runtime provenance.',
        'The compiler source diff describes the current checkout. Binary hashes identify the actual tested compiler/backend; source changes may not have been rebuilt yet.'
    )
}
$metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputPath 'metadata.json') -Encoding utf8NoBOM
Write-Host "Cache mode: $CacheLabel. Existing project caches are preserved; cold state must be prepared by the caller."
Write-Host "Measuring $compilerPath ($($argv[0])). Logs: $outputPath"

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $compilerPath
$startInfo.WorkingDirectory = $projectPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in $argv) { $startInfo.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo
$stdoutFile = [IO.FileStream]::new((Join-Path $outputPath 'stdout.log'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$stderrFile = [IO.FileStream]::new((Join-Path $outputPath 'stderr.log'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$csv = [IO.StreamWriter]::new((Join-Path $outputPath 'samples.csv'), $false, [Text.UTF8Encoding]::new($false))
$csv.AutoFlush = $true
$csv.WriteLine('elapsed_ms,tree_processes,vyx_compiler_children,native_tool_children,parent_rss_bytes,parent_private_bytes,tree_rss_bytes,tree_private_bytes,observed_cpu_ms,sample_work_ms')
$known = [Collections.Generic.Dictionary[int, long]]::new()
$latest = @{}
$parentPeakRss = 0L
$parentPeakPrivate = 0L
$treePeakRss = 0L
$treePeakPrivate = 0L
$peakCompilerChildren = 0
$sampleCount = 0
$sampleCostMs = 0.0
$compilerName = [IO.Path]::GetFileNameWithoutExtension($compilerPath)
$clock = [Diagnostics.Stopwatch]::StartNew()
$exitCode = $null
$observationHandle = [IntPtr]::Zero
try {
    if (-not $process.Start()) { throw 'Compiler process did not start.' }
    $observationHandle = [VyxBuildObservation]::OpenWaitHandle($process.Id)
    $stdoutTask = $process.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
    $stderrTask = $process.StandardError.BaseStream.CopyToAsync($stderrFile)
    do {
        $sampleClock = [Diagnostics.Stopwatch]::StartNew()
        $observations = [VyxBuildObservation]::ReadTree($process.Id, $known)
        $rss = 0L; $private = 0L; $parentRss = 0L; $parentPrivate = 0L
        $compilerChildren = 0; $nativeChildren = 0
        foreach ($observation in $observations) {
            $key = "$($observation.Id):$($observation.StartTicks)"
            $latest[$key] = $observation
            $rss += $observation.RssBytes
            $private += $observation.PrivateBytes
            if ($observation.Id -eq $process.Id) {
                $parentRss = $observation.RssBytes
                $parentPrivate = $observation.PrivateBytes
                $parentPeakRss = [Math]::Max($parentPeakRss, $observation.PeakRssBytes)
                $parentPeakPrivate = [Math]::Max($parentPeakPrivate, $observation.PeakPrivateBytes)
            } elseif ($observation.Name -eq $compilerName) {
                $compilerChildren++
            } elseif ($observation.Name -in @('clang', 'clang++', 'clang-cl', 'llc', 'lld', 'lld-link', 'link', 'cl')) {
                $nativeChildren++
            }
        }
        $cpuMs = 0.0
        foreach ($observation in $latest.Values) { $cpuMs += $observation.CpuMilliseconds }
        $treePeakRss = [Math]::Max($treePeakRss, $rss)
        $treePeakPrivate = [Math]::Max($treePeakPrivate, $private)
        $peakCompilerChildren = [Math]::Max($peakCompilerChildren, $compilerChildren)
        $workMs = $sampleClock.Elapsed.TotalMilliseconds
        $sampleCostMs += $workMs
        $sampleCount++
        $row = @($clock.ElapsedMilliseconds, $observations.Count, $compilerChildren, $nativeChildren, $parentRss, $parentPrivate, $rss, $private, $cpuMs, $workMs)
        $csv.WriteLine(($row | ForEach-Object { [Convert]::ToString($_, [Globalization.CultureInfo]::InvariantCulture) }) -join ',')
        if ([VyxBuildObservation]::HasTerminated($observationHandle)) { break }
        Start-Sleep -Milliseconds $SampleIntervalMs
    } while ($true)
    $observedExitSeconds = $clock.Elapsed.TotalSeconds
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $null = $stdoutTask.GetAwaiter().GetResult()
    $null = $stderrTask.GetAwaiter().GetResult()
    $clock.Stop()
    $statusWall = ($process.ExitTime.ToUniversalTime() - $process.StartTime.ToUniversalTime()).TotalSeconds
    $wall = $observedExitSeconds
    $summary = [ordered]@{
        exit_code = $exitCode
        wall_seconds = $wall
        os_exit_status_seconds = $statusWall
        observed_exit_seconds = $observedExitSeconds
        drain_seconds = $clock.Elapsed.TotalSeconds - $observedExitSeconds
        harness_elapsed_seconds = $clock.Elapsed.TotalSeconds
        parent_peak_rss_bytes = $parentPeakRss
        parent_peak_private_bytes = $parentPeakPrivate
        tree_sampled_peak_rss_bytes = $treePeakRss
        tree_sampled_peak_private_bytes = $treePeakPrivate
        peak_active_vyx_compiler_children = $peakCompilerChildren
        observed_process_count = $latest.Count
        observed_cpu_seconds = $cpuMs / 1000.0
        observed_cpu_over_wall = $(if ($wall -gt 0) { $cpuMs / 1000.0 / $wall } else { 0 })
        sample_count = $sampleCount
        sample_work_ms = $sampleCostMs
        average_sample_work_ms = $sampleCostMs / [Math]::Max(1, $sampleCount)
    }
    [ordered]@{ metadata = $metadata; summary = $summary } | ConvertTo-Json -Depth 7 |
        Set-Content -LiteralPath (Join-Path $outputPath 'result.json') -Encoding utf8NoBOM
    $latest.Values | Sort-Object StartTicks, Id | Select-Object Id, ParentId, Name, StartTicks, CpuMilliseconds, PeakRssBytes, PeakPrivateBytes |
        Export-Csv -LiteralPath (Join-Path $outputPath 'processes.csv') -NoTypeInformation -Encoding utf8NoBOM
    Write-Host ("Exit={0}; wall={1:N3}s; tree peak RSS={2:N1}MiB; observed CPU={3:N3}s; peak Vyx children={4}" -f $exitCode, $wall, ($treePeakRss / 1MB), ($cpuMs / 1000), $peakCompilerChildren)
} finally {
    [VyxBuildObservation]::CloseWaitHandle($observationHandle)
    $csv.Dispose()
    $stdoutFile.Dispose()
    $stderrFile.Dispose()
    $process.Dispose()
}
if ($null -ne $exitCode) { exit $exitCode }

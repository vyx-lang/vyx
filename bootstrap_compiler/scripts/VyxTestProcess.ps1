$ErrorActionPreference = "Stop"

$script:VyxTestProcessIsWindows = $false
if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    $script:VyxTestProcessIsWindows = [bool]$IsWindows
} else {
    $script:VyxTestProcessIsWindows = ($env:OS -eq "Windows_NT")
}

if (-not $script:VyxTestProcessIsWindows) {
    function Invoke-VyxProcess {
        param(
            [Parameter(Mandatory=$true)][string]$FilePath,
            [string[]]$ArgumentList = @(),
            [string]$WorkingDirectory = "",
            [string]$StdoutLog = "",
            [string]$StderrLog = "",
            [string]$DialogLog = "",
            [int]$TimeoutSec = 30,
            [int]$MemoryLimitMB = 0
        )

        if ([string]::IsNullOrWhiteSpace($StdoutLog)) { $StdoutLog = [System.IO.Path]::GetTempFileName() }
        if ([string]::IsNullOrWhiteSpace($StderrLog)) { $StderrLog = [System.IO.Path]::GetTempFileName() }
        if ([string]::IsNullOrWhiteSpace($DialogLog)) { $DialogLog = [System.IO.Path]::ChangeExtension($StdoutLog, ".dialog.log") }
        foreach ($p in @($StdoutLog, $StderrLog, $DialogLog)) {
            $dir = Split-Path -Parent $p
            if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
            if (Test-Path $p) { [System.IO.File]::Delete($p) }
        }

        if ($FilePath -in @("powershell", "powershell.exe")) {
            $shell = Get-Command $FilePath -ErrorAction SilentlyContinue
            if ($null -eq $shell) {
                $FilePath = (Get-Process -Id $PID).Path
            }
        }

        $stdoutTask = $null
        $stderrTask = $null
        try {
            $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = $FilePath
            if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
                $startInfo.WorkingDirectory = $WorkingDirectory
            }
            $startInfo.UseShellExecute = $false
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            foreach ($argument in $ArgumentList) {
                [void]$startInfo.ArgumentList.Add([string]$argument)
            }
            $proc = [System.Diagnostics.Process]::new()
            $proc.StartInfo = $startInfo
            if (-not $proc.Start()) {
                throw "process start returned false"
            }
            $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
            $stderrTask = $proc.StandardError.ReadToEndAsync()
        } catch {
            "failed to launch: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
            return [pscustomobject]@{
                ExitCode = -900
                TimedOut = $false
                DialogCaught = $false
                MemoryExceeded = $false
                PeakWorkingSetMB = 0.0
                JobLimited = $false
                StdoutLog = $StdoutLog
                StderrLog = $StderrLog
                DialogLog = $DialogLog
            }
        }

        $timedOut = $false
        if ($TimeoutSec -gt 0) {
            $exited = $proc.WaitForExit($TimeoutSec * 1000)
            if (-not $exited) {
                $timedOut = $true
                "timeout after $TimeoutSec sec: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
                # Kill the complete compiler tree. A bootstrap build launches
                # child compiler and linker processes which must not survive a
                # timed-out gate and keep mutating the retained stage.
                try {
                    $proc.Kill($true)
                } catch {
                    try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {}
                }
            }
        } else {
            $proc.WaitForExit()
        }

        try { $proc.WaitForExit(2000) | Out-Null } catch {}
        try {
            $stdoutText = if ($null -ne $stdoutTask) { $stdoutTask.GetAwaiter().GetResult() } else { "" }
            [System.IO.File]::WriteAllText($StdoutLog, $stdoutText)
        } catch {
            if (-not (Test-Path $StdoutLog)) { "" | Set-Content -Encoding utf8 $StdoutLog }
        }
        try {
            $stderrText = if ($null -ne $stderrTask) { $stderrTask.GetAwaiter().GetResult() } else { "" }
            [System.IO.File]::WriteAllText($StderrLog, $stderrText)
        } catch {
            if (-not (Test-Path $StderrLog)) { "" | Set-Content -Encoding utf8 $StderrLog }
        }
        $exit = -902
        if ($timedOut) {
            $exit = -901
        } else {
            try { $exit = $proc.ExitCode } catch { $exit = -903 }
        }

        if (-not (Test-Path $StdoutLog)) { "" | Set-Content -Encoding utf8 $StdoutLog }
        if (-not (Test-Path $StderrLog)) { "" | Set-Content -Encoding utf8 $StderrLog }
        if (-not (Test-Path $DialogLog)) { "" | Set-Content -Encoding utf8 $DialogLog }

        return [pscustomobject]@{
            ExitCode = $exit
            TimedOut = $timedOut
            DialogCaught = $false
            MemoryExceeded = $false
            PeakWorkingSetMB = 0.0
            JobLimited = $false
            StdoutLog = $StdoutLog
            StderrLog = $StderrLog
            DialogLog = $DialogLog
        }
    }
    return
}

function Initialize-VyxCrashSuppression {
    $env:SEM_NOGPFAULTERRORBOX = "1"
    $env:__COMPAT_LAYER = "DisableWerUI"
if (-not ("Win32.Sem" -as [type])) {
        try {
            Add-Type -Namespace Win32 -Name Sem -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetThreadErrorMode(uint mode, out uint oldMode);
[DllImport("wer.dll")] public static extern int WerSetFlags(uint flags);
[DllImport("wer.dll", CharSet=CharSet.Unicode)] public static extern int WerAddExcludedApplication(string exeName, bool allUsers);
[DllImport("wer.dll", CharSet=CharSet.Unicode)] public static extern int WerRemoveExcludedApplication(string exeName, bool allUsers);
'@ -ErrorAction SilentlyContinue
        } catch {}
    }
    try { [Win32.Sem]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x8000) | Out-Null } catch {}
    try {
        $oldMode = 0
        [Win32.Sem]::SetThreadErrorMode(0x0001 -bor 0x0002 -bor 0x8000, [ref]$oldMode) | Out-Null
    } catch {}
    try { [Win32.Sem]::WerSetFlags(0x20) | Out-Null } catch {}
    try {
        $werKey = "HKCU:\Software\Microsoft\Windows\Windows Error Reporting"
        if (-not (Test-Path $werKey)) {
            New-Item -Path $werKey -Force | Out-Null
        }
        New-ItemProperty -Path $werKey -Name "DontShowUI" -PropertyType DWord -Value 1 -Force | Out-Null
        New-ItemProperty -Path $werKey -Name "ForceQueue" -PropertyType DWord -Value 1 -Force | Out-Null
    } catch {}

    if (-not ("VyxWin32.WindowProbe" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace VyxWin32 {
    public static class WindowProbe {
        public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc cb, IntPtr extra);
        [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc cb, IntPtr extra);
        [DllImport("user32.dll")] private static extern int GetWindowTextLength(IntPtr hWnd);
        [DllImport("user32.dll")] private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);
        [DllImport("user32.dll")] private static extern int GetClassName(IntPtr hWnd, StringBuilder text, int maxCount);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
        [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hWnd);
        [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", SetLastError=true)] private static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);

        private const uint WM_CLOSE = 0x0010;
        private const uint WM_COMMAND = 0x0111;
        private const uint BM_CLICK = 0x00F5;
        private const int IDOK = 1;
        private const int IDCANCEL = 2;
        private const uint SMTO_ABORTIFHUNG = 0x0002;

        private static void SendBounded(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam) {
            IntPtr result;
            SendMessageTimeout(hWnd, msg, wParam, lParam, SMTO_ABORTIFHUNG, 250, out result);
        }

        public static IntPtr[] TopLevelWindows() {
            var wins = new List<IntPtr>();
            EnumWindows((h, extra) => {
                if (IsWindowVisible(h)) {
                    wins.Add(h);
                }
                return true;
            }, IntPtr.Zero);
            return wins.ToArray();
        }

        public static IntPtr[] TopLevelWindowsForPid(int processId) {
            var wins = new List<IntPtr>();
            EnumWindows((h, extra) => {
                uint pid;
                GetWindowThreadProcessId(h, out pid);
                if (pid == (uint)processId && IsWindowVisible(h)) {
                    wins.Add(h);
                }
                return true;
            }, IntPtr.Zero);
            return wins.ToArray();
        }

        public static int ProcessIdForWindow(IntPtr hWnd) {
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            return (int)pid;
        }

        public static string WindowText(IntPtr hWnd) {
            int len = GetWindowTextLength(hWnd);
            var sb = new StringBuilder(Math.Max(len + 1, 256));
            GetWindowText(hWnd, sb, sb.Capacity);
            return sb.ToString();
        }

        public static string ClassName(IntPtr hWnd) {
            var sb = new StringBuilder(256);
            GetClassName(hWnd, sb, sb.Capacity);
            return sb.ToString();
        }

        public static string TreeText(IntPtr hWnd) {
            var lines = new List<string>();
            string title = WindowText(hWnd);
            string cls = ClassName(hWnd);
            if (!String.IsNullOrEmpty(title) || !String.IsNullOrEmpty(cls)) {
                lines.Add("window class=" + cls + " text=" + title);
            }
            EnumChildWindows(hWnd, (child, extra) => {
                string ct = WindowText(child);
                string cc = ClassName(child);
                if (!String.IsNullOrEmpty(ct) || !String.IsNullOrEmpty(cc)) {
                    lines.Add("child class=" + cc + " text=" + ct);
                }
                return true;
            }, IntPtr.Zero);
            return String.Join(Environment.NewLine, lines.ToArray());
        }

        private static bool IsCrashCloseButton(string text) {
            if (String.IsNullOrWhiteSpace(text)) {
                return false;
            }
            string t = text.Trim().Replace("&", "").ToLowerInvariant();
            return t == "ok" ||
                t == "close" ||
                t == "close program" ||
                t == "close the program" ||
                t == "abort" ||
                t == "terminate" ||
                t == "确定" ||
                t == "关闭" ||
                t == "关闭程序" ||
                t == "中止" ||
                t == "终止";
        }

        public static void CloseWindow(IntPtr hWnd) {
            EnumChildWindows(hWnd, (child, extra) => {
                string cls = ClassName(child);
                string text = WindowText(child);
                if (String.Equals(cls, "Button", StringComparison.OrdinalIgnoreCase) && IsCrashCloseButton(text)) {
                    SendBounded(child, BM_CLICK, IntPtr.Zero, IntPtr.Zero);
                }
                return true;
            }, IntPtr.Zero);
            SendBounded(hWnd, WM_COMMAND, new IntPtr(IDOK), IntPtr.Zero);
            SendBounded(hWnd, WM_COMMAND, new IntPtr(IDCANCEL), IntPtr.Zero);
            SendBounded(hWnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
            PostMessage(hWnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }
    }
}
"@
    }
}

function Set-VyxProcessCrashSuppressionEnvironment {
    param([System.Diagnostics.ProcessStartInfo]$ProcessStartInfo)
    if ($null -eq $ProcessStartInfo) { return }
    try { $ProcessStartInfo.EnvironmentVariables["SEM_NOGPFAULTERRORBOX"] = "1" } catch {}
    try { $ProcessStartInfo.EnvironmentVariables["__COMPAT_LAYER"] = "DisableWerUI" } catch {}
    try { $ProcessStartInfo.EnvironmentVariables["VYX_DISABLE_WER_UI"] = "1" } catch {}
}

function Initialize-VyxMemoryJobSupport {
    if ("VyxWin32.JobLimit" -as [type]) { return }
    try {
        Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace VyxWin32 {
    public static class JobLimit {
        private const int JobObjectExtendedLimitInformation = 9;
        private const uint JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100;
        private const uint JOB_OBJECT_LIMIT_JOB_MEMORY = 0x00000200;
        private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct IO_COUNTERS {
            public ulong ReadOperationCount;
            public ulong WriteOperationCount;
            public ulong OtherOperationCount;
            public ulong ReadTransferCount;
            public ulong WriteTransferCount;
            public ulong OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
            public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
            public IO_COUNTERS IoInfo;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string lpName);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(IntPtr hJob, int infoClass, IntPtr lpInfo, uint cbInfo);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool QueryInformationJobObject(IntPtr hJob, int infoClass, IntPtr lpInfo, uint cbInfo, IntPtr lpReturnLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr hObject);

        public static IntPtr CreateLimitedJob(long memoryLimitBytes) {
            if (memoryLimitBytes <= 0) {
                return IntPtr.Zero;
            }
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            if (job == IntPtr.Zero) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateJobObject failed");
            }

            var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            info.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                JOB_OBJECT_LIMIT_JOB_MEMORY |
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            UIntPtr limit = new UIntPtr((ulong)memoryLimitBytes);
            info.ProcessMemoryLimit = limit;
            info.JobMemoryLimit = limit;

            int size = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try {
                Marshal.StructureToPtr(info, buffer, false);
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, buffer, (uint)size)) {
                    int err = Marshal.GetLastWin32Error();
                    CloseHandle(job);
                    throw new Win32Exception(err, "SetInformationJobObject failed");
                }
            } finally {
                Marshal.FreeHGlobal(buffer);
            }
            return job;
        }

        public static void Assign(IntPtr job, IntPtr processHandle) {
            if (job == IntPtr.Zero || processHandle == IntPtr.Zero) {
                throw new ArgumentException("invalid job or process handle");
            }
            if (!AssignProcessToJobObject(job, processHandle)) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "AssignProcessToJobObject failed");
            }
        }

        public static ulong PeakJobMemory(IntPtr job) {
            if (job == IntPtr.Zero) {
                return 0UL;
            }
            int size = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try {
                if (!QueryInformationJobObject(job, JobObjectExtendedLimitInformation, buffer, (uint)size, IntPtr.Zero)) {
                    return 0UL;
                }
                var info = (JOBOBJECT_EXTENDED_LIMIT_INFORMATION)Marshal.PtrToStructure(buffer, typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
                return info.PeakJobMemoryUsed.ToUInt64();
            } finally {
                Marshal.FreeHGlobal(buffer);
            }
        }

        public static void Close(IntPtr job) {
            if (job != IntPtr.Zero) {
                CloseHandle(job);
            }
        }
    }
}
"@ -ErrorAction Stop
    } catch {}
}

function Initialize-VyxSuspendedProcessSupport {
    if ("VyxWin32.SuspendedProcessLauncher" -as [type]) { return $true }
    try {
        Add-Type -TypeDefinition @"
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace VyxWin32 {
    public static class SuspendedProcessLauncher {
        private const uint CREATE_SUSPENDED = 0x00000004;
        private const uint CREATE_NO_WINDOW = 0x08000000;
        private const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
        private const uint STARTF_USESHOWWINDOW = 0x00000001;
        private const ushort SW_HIDE = 0;
        private const uint RESUME_FAILED = 0xffffffff;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct STARTUPINFO {
            public uint cb;
            public string lpReserved;
            public string lpDesktop;
            public string lpTitle;
            public uint dwX;
            public uint dwY;
            public uint dwXSize;
            public uint dwYSize;
            public uint dwXCountChars;
            public uint dwYCountChars;
            public uint dwFillAttribute;
            public uint dwFlags;
            public ushort wShowWindow;
            public ushort cbReserved2;
            public IntPtr lpReserved2;
            public IntPtr hStdInput;
            public IntPtr hStdOutput;
            public IntPtr hStdError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PROCESS_INFORMATION {
            public IntPtr hProcess;
            public IntPtr hThread;
            public uint dwProcessId;
            public uint dwThreadId;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateProcessW(
            string lpApplicationName,
            StringBuilder lpCommandLine,
            IntPtr lpProcessAttributes,
            IntPtr lpThreadAttributes,
            bool bInheritHandles,
            uint dwCreationFlags,
            IntPtr lpEnvironment,
            string lpCurrentDirectory,
            ref STARTUPINFO lpStartupInfo,
            out PROCESS_INFORMATION lpProcessInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint ResumeThread(IntPtr hThread);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateProcess(IntPtr hProcess, uint uExitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr hObject);

        private static string QuoteApplicationName(string value) {
            if (String.IsNullOrWhiteSpace(value)) {
                throw new ArgumentException("process executable is empty", "startInfo");
            }
            return "\"" + value.Replace("\"", "\\\"") + "\"";
        }

        private static IntPtr BuildEnvironmentBlock(ProcessStartInfo startInfo) {
            var entries = new List<string>();
            foreach (DictionaryEntry entry in startInfo.EnvironmentVariables) {
                string key = entry.Key as string;
                string value = entry.Value as string;
                if (String.IsNullOrEmpty(key) || value == null || key.IndexOf('\0') >= 0 || value.IndexOf('\0') >= 0) {
                    continue;
                }
                entries.Add(key + "=" + value);
            }
            entries.Sort(StringComparer.OrdinalIgnoreCase);
            string block = String.Join("\0", entries.ToArray()) + "\0\0";
            return Marshal.StringToHGlobalUni(block);
        }

        public static Process Start(ProcessStartInfo startInfo, IntPtr job) {
            if (startInfo == null) {
                throw new ArgumentNullException("startInfo");
            }
            if (job == IntPtr.Zero) {
                throw new ArgumentException("memory job handle is invalid", "job");
            }
            if (startInfo.UseShellExecute) {
                throw new ArgumentException("suspended launch requires UseShellExecute=false", "startInfo");
            }

            string commandLine = QuoteApplicationName(startInfo.FileName);
            if (!String.IsNullOrWhiteSpace(startInfo.Arguments)) {
                commandLine += " " + startInfo.Arguments;
            }
            var mutableCommandLine = new StringBuilder(commandLine);
            IntPtr environment = BuildEnvironmentBlock(startInfo);
            var startup = new STARTUPINFO();
            startup.cb = (uint)Marshal.SizeOf(typeof(STARTUPINFO));
            if (startInfo.CreateNoWindow) {
                startup.dwFlags |= STARTF_USESHOWWINDOW;
                startup.wShowWindow = SW_HIDE;
            }
            var processInfo = new PROCESS_INFORMATION();
            Process process = null;
            bool created = false;
            bool resumed = false;
            try {
                uint flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
                if (startInfo.CreateNoWindow) {
                    flags |= CREATE_NO_WINDOW;
                }
                string workingDirectory = String.IsNullOrWhiteSpace(startInfo.WorkingDirectory)
                    ? null
                    : startInfo.WorkingDirectory;
                if (!CreateProcessW(
                        startInfo.FileName,
                        mutableCommandLine,
                        IntPtr.Zero,
                        IntPtr.Zero,
                        false,
                        flags,
                        environment,
                        workingDirectory,
                        ref startup,
                        out processInfo)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessW(CREATE_SUSPENDED) failed");
                }
                created = true;

                if (!AssignProcessToJobObject(job, processInfo.hProcess)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "AssignProcessToJobObject before resume failed");
                }

                process = Process.GetProcessById((int)processInfo.dwProcessId);
                IntPtr cachedProcessHandle = process.Handle;
                if (ResumeThread(processInfo.hThread) == RESUME_FAILED) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "ResumeThread failed");
                }
                resumed = true;
                return process;
            } finally {
                if (created && !resumed) {
                    TerminateProcess(processInfo.hProcess, 0xffffffff);
                    if (process != null) {
                        process.Dispose();
                    }
                }
                if (processInfo.hThread != IntPtr.Zero) {
                    CloseHandle(processInfo.hThread);
                }
                if (processInfo.hProcess != IntPtr.Zero) {
                    CloseHandle(processInfo.hProcess);
                }
                if (environment != IntPtr.Zero) {
                    Marshal.FreeHGlobal(environment);
                }
            }
        }
    }
}
"@ -ErrorAction Stop | Out-Null
    } catch {
        return $false
    }
    return ($null -ne ("VyxWin32.SuspendedProcessLauncher" -as [type]))
}

function Start-VyxProcessSuspendedInMemoryJob {
    param(
        [Parameter(Mandatory=$true)][System.Diagnostics.ProcessStartInfo]$ProcessStartInfo,
        [Parameter(Mandatory=$true)][IntPtr]$JobHandle
    )
    if (-not (Test-VyxMemoryJobHandle -JobHandle $JobHandle)) {
        throw "memory job creation failed"
    }
    if (-not (Initialize-VyxSuspendedProcessSupport)) {
        throw "suspended process launcher initialization failed"
    }
    return [VyxWin32.SuspendedProcessLauncher]::Start($ProcessStartInfo, $JobHandle)
}

function Test-VyxMemoryJobHandle {
    param([IntPtr]$JobHandle)
    return ($null -ne $JobHandle -and $JobHandle.ToInt64() -ne 0)
}

function New-VyxMemoryJob {
    param([int]$MemoryLimitMB)
    if ($MemoryLimitMB -le 0) { return [IntPtr]::Zero }
    Initialize-VyxMemoryJobSupport
    try {
        $bytes = [int64]$MemoryLimitMB * 1048576
        return [VyxWin32.JobLimit]::CreateLimitedJob($bytes)
    } catch {
        return [IntPtr]::Zero
    }
}

function Add-VyxProcessToMemoryJob {
    param(
        [IntPtr]$JobHandle,
        [System.Diagnostics.Process]$Process
    )
    if (-not (Test-VyxMemoryJobHandle -JobHandle $JobHandle)) { return $false }
    if ($null -eq $Process) { return $false }
    try {
        [VyxWin32.JobLimit]::Assign($JobHandle, $Process.Handle)
        return $true
    } catch {
        return $false
    }
}

function Get-VyxMemoryJobPeakMB {
    param([IntPtr]$JobHandle)
    if (-not (Test-VyxMemoryJobHandle -JobHandle $JobHandle)) { return 0.0 }
    try {
        $bytes = [VyxWin32.JobLimit]::PeakJobMemory($JobHandle)
        return [Math]::Round(([double]$bytes / 1048576.0), 1)
    } catch {
        return 0.0
    }
}

function Test-VyxMemoryJobLimit {
    param(
        [IntPtr]$JobHandle,
        [int]$MemoryLimitMB,
        [ref]$PeakWorkingSetMB
    )
    if ($MemoryLimitMB -le 0) { return $false }
    $mb = Get-VyxMemoryJobPeakMB -JobHandle $JobHandle
    if ($mb -gt [double]$PeakWorkingSetMB.Value) {
        $PeakWorkingSetMB.Value = $mb
    }
    if ($mb -le 0.0) { return $false }
    return ($mb -ge ([double]$MemoryLimitMB * 0.98))
}

function Close-VyxMemoryJob {
    param([IntPtr]$JobHandle)
    if (-not (Test-VyxMemoryJobHandle -JobHandle $JobHandle)) { return }
    try { [VyxWin32.JobLimit]::Close($JobHandle) } catch {}
}

function Get-VyxCrashSuppressionExeNames {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList = @()
    )
    $names = @{}
    foreach ($candidate in @($FilePath) + $ArgumentList) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $text = $candidate.Trim('"')
        if (-not $text.EndsWith(".exe", [StringComparison]::OrdinalIgnoreCase)) { continue }
        try {
            $leaf = [System.IO.Path]::GetFileName($text)
            if (-not [string]::IsNullOrWhiteSpace($leaf)) {
                $names[$leaf.ToLowerInvariant()] = $leaf
            }
        } catch {}
    }
    return @($names.Values)
}

function Add-VyxCrashReporterExclusions {
    param([string[]]$Names)
    Initialize-VyxCrashSuppression
    $added = New-Object System.Collections.Generic.List[string]
    foreach ($name in $Names) {
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        try {
            $hr = [Win32.Sem]::WerAddExcludedApplication($name, $false)
            if ($hr -eq 0) {
                $added.Add($name)
            }
        } catch {}
    }
    return @($added.ToArray())
}

function Remove-VyxCrashReporterExclusions {
    param([string[]]$Names)
    if ($null -eq $Names) { return }
    foreach ($name in $Names) {
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        try { [Win32.Sem]::WerRemoveExcludedApplication($name, $false) | Out-Null } catch {}
    }
}

function Get-VyxCrashDialogPattern {
    return "Microsoft Visual C\+\+ Runtime Library|Runtime Error|Assertion failed|Debug Assertion Failed|JIT must be enabled|JIT debugging|Press Retry|Abort|Retry|Ignore|Both operands to ICmp instruction|has stopped working|stopped working|Close the program|Close program|unhandled exception|Unhandled exception|Access violation|0xC0000005|0xc0000005|The instruction at|memory could not be|Windows Error Reporting|Windows Error|Windows Problem Reporting|Application Error|System Error|Fatal error|LLVM ERROR|Unable to start correctly|The application was unable to start correctly|Click OK to close|Problem signature|Unknown software exception|Exception Processing Message|Bad Image|Entry Point Not Found|程序异常|异常代码|故障模块|应用程序错误|系统错误|已停止工作|未处理的异常|访问冲突|内存不能|无法正常启动|应用程序无法正常启动|请单击|确定|关闭程序|问题签名|未知的软件异常|异常处理消息|映像损坏|找不到入口点|终止|重试|忽略"
}

function Get-VyxCrashReporterProcessNames {
    return @("WerFault", "WerFaultSecure", "vsjitdebugger", "DW20", "dwwin")
}

function Test-VyxCrashReporterProcessName {
    param([string]$Name)
    if ([string]::IsNullOrWhiteSpace($Name)) { return $false }
    foreach ($candidate in Get-VyxCrashReporterProcessNames) {
        if ([string]::Equals($Name, $candidate, [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Test-VyxTextMentionsTargetProcess {
    param(
        [string]$Text,
        [string[]]$TargetProcessNames = @()
    )
    if ([string]::IsNullOrWhiteSpace($Text)) { return $false }
    foreach ($name in $TargetProcessNames) {
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        if ($Text.IndexOf($name, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }
    return $false
}

function Get-VyxProcessExecutablePath {
    param([int]$ProcessId)
    try {
        $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
        if ($null -ne $proc -and -not [string]::IsNullOrWhiteSpace($proc.ExecutablePath)) {
            return $proc.ExecutablePath
        }
    } catch {}
    try {
        $p = Get-Process -Id $ProcessId -ErrorAction Stop
        if ($null -ne $p.MainModule -and -not [string]::IsNullOrWhiteSpace($p.MainModule.FileName)) {
            return $p.MainModule.FileName
        }
    } catch {}
    return ""
}

function Get-VyxProcessInfo {
    param([int]$ProcessId)
    try {
        return Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
    } catch {
        return $null
    }
}

function Test-VyxCrashReporterTargetsRoot {
    param(
        [int]$ReporterProcessId,
        [int]$RootPid,
        [hashtable]$RootIdSet,
        [string[]]$TargetProcessNames = @(),
        [string]$WorkingDirectory = ""
    )
    $reporter = Get-VyxProcessInfo -ProcessId $ReporterProcessId
    if ($null -eq $reporter) { return $false }

    $parentKey = [string]$reporter.ParentProcessId
    if ($null -ne $RootIdSet -and $RootIdSet.ContainsKey($parentKey)) { return $true }

    $commandLine = [string]$reporter.CommandLine
    if ([string]::IsNullOrWhiteSpace($commandLine)) { return $false }
    $targetPid = 0
    $pidMatch = [regex]::Match($commandLine, '(?i)(?:^|\s)[-/](?:p|pid)(?:\s+|=)(\d+)')
    if ($pidMatch.Success) {
        [void][int]::TryParse($pidMatch.Groups[1].Value, [ref]$targetPid)
    }
    if ($targetPid -le 0) { return $false }
    if ($targetPid -eq $RootPid) { return $true }
    if ($null -ne $RootIdSet -and $RootIdSet.ContainsKey([string]$targetPid)) { return $true }

    $target = Get-VyxProcessInfo -ProcessId $targetPid
    if ($null -eq $target) { return $false }
    $targetName = [System.IO.Path]::GetFileNameWithoutExtension([string]$target.Name)
    foreach ($name in $TargetProcessNames) {
        if ([string]::Equals($targetName, $name, [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return Test-VyxPathUnderRoot -Path ([string]$target.ExecutablePath) -Root $WorkingDirectory
}

function Test-VyxPathUnderRoot {
    param(
        [string]$Path,
        [string]$Root
    )
    if ([string]::IsNullOrWhiteSpace($Path) -or [string]::IsNullOrWhiteSpace($Root)) { return $false }
    try {
        $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
        $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
        if ([string]::IsNullOrWhiteSpace($fullPath) -or [string]::IsNullOrWhiteSpace($fullRoot)) { return $false }
        $prefix = $fullRoot + [System.IO.Path]::DirectorySeparatorChar
        return $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or [string]::Equals($fullPath, $fullRoot, [StringComparison]::OrdinalIgnoreCase)
    } catch {
        return $false
    }
}

function Test-VyxProcessUnderWorkingDirectory {
    param(
        [int]$ProcessId,
        [string]$WorkingDirectory
    )
    if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) { return $false }
    $exePath = Get-VyxProcessExecutablePath -ProcessId $ProcessId
    return Test-VyxPathUnderRoot -Path $exePath -Root $WorkingDirectory
}

function Get-VyxProcessIdSetByName {
    param([string[]]$Names)
    $set = @{}
    foreach ($name in $Names) {
        try {
            foreach ($proc in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
                $set[[string]$proc.Id] = $true
            }
        } catch {}
    }
    return $set
}

function Get-VyxProcessIdSet {
    param([string[]]$Names)
    $set = @{}
    foreach ($name in $Names) {
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        try {
            foreach ($proc in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
                $set[[string]$proc.Id] = $true
            }
        } catch {}
    }
    return $set
}

function Get-VyxAllProcessIdSet {
    $set = @{}
    try {
        foreach ($proc in @(Get-Process -ErrorAction SilentlyContinue)) {
            $set[[string]$proc.Id] = $true
        }
    } catch {}
    return $set
}

function Get-VyxProcessBaseName {
    param([string]$FilePath)
    if ([string]::IsNullOrWhiteSpace($FilePath)) { return "" }
    try { return [System.IO.Path]::GetFileNameWithoutExtension($FilePath) } catch { return "" }
}

function Get-VyxTargetProcessNames {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList = @()
    )
    $names = @{}
    $base = Get-VyxProcessBaseName -FilePath $FilePath
    if (-not [string]::IsNullOrWhiteSpace($base)) {
        $names[$base.ToLowerInvariant()] = $base
    }
    foreach ($candidate in $ArgumentList) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $text = $candidate.Trim('"')
        if (-not $text.EndsWith(".exe", [StringComparison]::OrdinalIgnoreCase)) { continue }
        try {
            $leaf = [System.IO.Path]::GetFileNameWithoutExtension($text)
            if (-not [string]::IsNullOrWhiteSpace($leaf)) {
                $names[$leaf.ToLowerInvariant()] = $leaf
            }
        } catch {}
    }
    return @($names.Values)
}

function Get-VyxVisibleCrashWindowHandleSet {
    Initialize-VyxCrashSuppression
    $handles = @{}
    $crashPattern = Get-VyxCrashDialogPattern
    foreach ($hwnd in [VyxWin32.WindowProbe]::TopLevelWindows()) {
        $key = $hwnd.ToInt64().ToString()
        $title = [VyxWin32.WindowProbe]::WindowText($hwnd)
        $tree = [VyxWin32.WindowProbe]::TreeText($hwnd)
        $joined = ($title + "`n" + $tree)
        if ($joined -match $crashPattern) {
            $handles[$key] = $true
        }
    }
    return $handles
}

function Get-VyxProcessTreeIds {
    param([int]$RootPid)
    $ids = @($RootPid)
    $queue = New-Object System.Collections.Queue
    $queue.Enqueue($RootPid)
    while ($queue.Count -gt 0) {
        $procId = [int]$queue.Dequeue()
        try {
            $children = Get-CimInstance Win32_Process -Filter "ParentProcessId=$procId" -ErrorAction SilentlyContinue
            foreach ($child in $children) {
                $cid = [int]$child.ProcessId
                if ($ids -notcontains $cid) {
                    $ids += $cid
                    $queue.Enqueue($cid)
                }
            }
        } catch {}
    }
    return $ids
}

function Stop-VyxProcessTree {
    param([int]$RootPid)
    # taskkill /F /T is far cheaper than Get-CimInstance + Stop-Process —
    # the former is a single Win32 API call, while CIM round-trips through
    # WMI (~1s per call). Fall back to the CIM walk only if taskkill is
    # unavailable.
    try {
        & taskkill.exe /PID $RootPid /T /F 2>$null | Out-Null
        return
    } catch {}
    $ids = @(Get-VyxProcessTreeIds -RootPid $RootPid)
    [array]::Reverse($ids)
    foreach ($procId in $ids) {
        try { Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue } catch {}
    }
}

function Get-VyxProcessTreeWorkingSetBytes {
    param([int]$RootPid)
    $total = [int64]0
    $ids = @(Get-VyxProcessTreeIds -RootPid $RootPid)
    foreach ($procId in $ids) {
        $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
        if ($null -ne $proc) {
            $total += [int64]$proc.WorkingSet64
        }
    }
    return $total
}

function Test-VyxProcessTreeMemoryLimit {
    param(
        [int]$RootPid,
        [int]$MemoryLimitMB,
        [ref]$PeakWorkingSetMB
    )
    if ($MemoryLimitMB -le 0) { return $false }
    $bytes = Get-VyxProcessTreeWorkingSetBytes -RootPid $RootPid
    $mb = [Math]::Round(([double]$bytes / 1048576.0), 1)
    if ($mb -gt [double]$PeakWorkingSetMB.Value) {
        $PeakWorkingSetMB.Value = $mb
    }
    return ($mb -ge $MemoryLimitMB)
}

function Get-VyxCrashDialogs {
    param(
        [int]$RootPid,
        [hashtable]$IgnoredHandles = $null,
        [hashtable]$IgnoredReporterPids = $null,
        [hashtable]$IgnoredTargetPids = $null,
        [hashtable]$IgnoredExistingPids = $null,
        [string[]]$TargetProcessNames = @(),
        [string]$WorkingDirectory = "",
        [switch]$AllowGlobal
    )
    Initialize-VyxCrashSuppression
    $hits = @()
    $targetNameSet = @{}
    foreach ($name in $TargetProcessNames) {
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            $targetNameSet[$name.ToLowerInvariant()] = $true
        }
    }
    $rootIds = @(Get-VyxProcessTreeIds -RootPid $RootPid)
    $rootIdSet = @{}
    foreach ($id in $rootIds) { $rootIdSet[[string]$id] = $true }
    $seenWindows = @{}
    $crashPattern = Get-VyxCrashDialogPattern

    foreach ($name in Get-VyxCrashReporterProcessNames) {
        try {
            foreach ($proc in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
                $pidKey = [string]$proc.Id
                if ($IgnoredReporterPids -ne $null -and $IgnoredReporterPids.ContainsKey($pidKey)) { continue }
                if (-not (Test-VyxCrashReporterTargetsRoot `
                    -ReporterProcessId $proc.Id `
                    -RootPid $RootPid `
                    -RootIdSet $rootIdSet `
                    -TargetProcessNames $TargetProcessNames `
                    -WorkingDirectory $WorkingDirectory)) { continue }
                $hits += [pscustomobject]@{
                    ProcessId = [int]$proc.Id
                    ProcessName = $proc.ProcessName
                    Handle = [IntPtr]::Zero
                    Title = "crash reporter process"
                    Text = ("new crash reporter process detected: pid={0} process={1}" -f $proc.Id, $proc.ProcessName)
                }
            }
        } catch {}
    }

    foreach ($hwnd in [VyxWin32.WindowProbe]::TopLevelWindows()) {
        $key = $hwnd.ToInt64().ToString()
        if ($seenWindows.ContainsKey($key)) { continue }
        $seenWindows[$key] = $true
        if ($IgnoredHandles -ne $null -and $IgnoredHandles.ContainsKey($key)) { continue }

        $procId = [VyxWin32.WindowProbe]::ProcessIdForWindow($hwnd)
        if ($IgnoredTargetPids -ne $null -and $IgnoredTargetPids.ContainsKey([string]$procId)) { continue }
        $title = [VyxWin32.WindowProbe]::WindowText($hwnd)
        $className = [VyxWin32.WindowProbe]::ClassName($hwnd)
        $tree = [VyxWin32.WindowProbe]::TreeText($hwnd)
        $joined = ($title + "`n" + $tree)
        $processName = ""
        try { $processName = (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { $processName = "" }
        $isWerSidecar = Test-VyxCrashReporterProcessName -Name $processName
        $isCorrelatedWerSidecar = $isWerSidecar -and (Test-VyxCrashReporterTargetsRoot `
            -ReporterProcessId $procId `
            -RootPid $RootPid `
            -RootIdSet $rootIdSet `
            -TargetProcessNames $TargetProcessNames `
            -WorkingDirectory $WorkingDirectory)
        $isTargetDialog = ($className -eq "#32770" -and $targetNameSet.ContainsKey($processName.ToLowerInvariant()))
        $isNewGlobalDialog = $false
        if ($AllowGlobal -and $className -eq "#32770") {
            $isNewGlobalDialog = ($IgnoredExistingPids -eq $null -or -not $IgnoredExistingPids.ContainsKey([string]$procId))
        }
        $mentionsTarget = Test-VyxTextMentionsTargetProcess -Text $joined -TargetProcessNames $TargetProcessNames
        $isWorkspaceDialog = $isNewGlobalDialog -and (Test-VyxProcessUnderWorkingDirectory -ProcessId $procId -WorkingDirectory $WorkingDirectory)
        $looksLikeCrashDialog = $isTargetDialog -or ($AllowGlobal -and $isNewGlobalDialog `
            -and ($mentionsTarget -or $isWorkspaceDialog -or $isCorrelatedWerSidecar))
        if (-not $looksLikeCrashDialog) { continue }
        $hits += [pscustomobject]@{
            ProcessId = $procId
            ProcessName = $processName
            Handle = $hwnd
            Title = $title
            Text = $tree
        }
    }
    if ($hits.Count -gt 0) { return $hits }

    $candidateWindows = @()
    foreach ($procId in $rootIds) {
        foreach ($hwnd in [VyxWin32.WindowProbe]::TopLevelWindowsForPid($procId)) {
            $candidateWindows += $hwnd
        }
    }
    foreach ($hwnd in [VyxWin32.WindowProbe]::TopLevelWindows()) {
        $candidateWindows += $hwnd
    }

    foreach ($hwnd in $candidateWindows) {
        $key = $hwnd.ToInt64().ToString()
        if ($seenWindows.ContainsKey($key)) { continue }
        $seenWindows[$key] = $true
        if ($IgnoredHandles -ne $null -and $IgnoredHandles.ContainsKey($key)) { continue }

        $procId = [VyxWin32.WindowProbe]::ProcessIdForWindow($hwnd)
        $title = [VyxWin32.WindowProbe]::WindowText($hwnd)
        $className = [VyxWin32.WindowProbe]::ClassName($hwnd)
        $tree = [VyxWin32.WindowProbe]::TreeText($hwnd)
        $joined = ($title + "`n" + $tree)
        $processName = ""
        try { $processName = (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { $processName = "" }
        if ($IgnoredTargetPids -ne $null -and $IgnoredTargetPids.ContainsKey([string]$procId)) { continue }
        $belongsToRoot = $rootIdSet.ContainsKey([string]$procId)
        $isWerSidecar = Test-VyxCrashReporterProcessName -Name $processName
        $isCorrelatedWerSidecar = $isWerSidecar -and (Test-VyxCrashReporterTargetsRoot `
            -ReporterProcessId $procId `
            -RootPid $RootPid `
            -RootIdSet $rootIdSet `
            -TargetProcessNames $TargetProcessNames `
            -WorkingDirectory $WorkingDirectory)
        $isTargetDialog = ($className -eq "#32770" -and $targetNameSet.ContainsKey($processName.ToLowerInvariant()))
        $isNewGlobalDialog = $false
        if ($AllowGlobal -and $className -eq "#32770") {
            $isNewGlobalDialog = ($IgnoredExistingPids -eq $null -or -not $IgnoredExistingPids.ContainsKey([string]$procId))
        }
        $mentionsTarget = Test-VyxTextMentionsTargetProcess -Text $joined -TargetProcessNames $TargetProcessNames
        $isWorkspaceDialog = $isNewGlobalDialog -and (Test-VyxProcessUnderWorkingDirectory -ProcessId $procId -WorkingDirectory $WorkingDirectory)
        $looksLikeCrashDialog = ($belongsToRoot -and (($joined -match $crashPattern) -or $className -eq "#32770")) `
            -or $isTargetDialog `
            -or ($AllowGlobal -and $isNewGlobalDialog `
                -and ($mentionsTarget -or $isWorkspaceDialog -or $isCorrelatedWerSidecar))
        if (-not $looksLikeCrashDialog) { continue }
        if ($belongsToRoot -or $isCorrelatedWerSidecar -or $isTargetDialog `
            -or ($AllowGlobal -and ($mentionsTarget -or $isWorkspaceDialog))) {
            $hits += [pscustomobject]@{
                ProcessId = $procId
                ProcessName = $processName
                Handle = $hwnd
                Title = $title
                Text = $tree
            }
        }
    }
    return $hits
}

function Save-VyxCrashDialogReport {
    param(
        [string]$DialogLog,
        [string]$Header,
        [object[]]$Dialogs,
        [switch]$KillProcesses
    )
    $lines = @()
    $lines += $Header
    foreach ($d in $Dialogs) {
        $lines += ("pid={0} process={1} title={2}" -f $d.ProcessId, $d.ProcessName, $d.Title)
        $lines += $d.Text
        try {
            if ($d.Handle -ne [IntPtr]::Zero) {
                [VyxWin32.WindowProbe]::CloseWindow($d.Handle)
            }
        } catch {}
        if (Test-VyxCrashReporterProcessName -Name $d.ProcessName) {
            try { Stop-Process -Id $d.ProcessId -Force -ErrorAction SilentlyContinue } catch {}
        }
        if ($KillProcesses -and $d.ProcessId -gt 0) {
            try { Stop-Process -Id $d.ProcessId -Force -ErrorAction SilentlyContinue } catch {}
        }
    }
    $lines | Set-Content -Encoding utf8 $DialogLog
}

function Test-VyxCrashLikeExitCode {
    param([int]$ExitCode)
    if ($ExitCode -lt 0) { return $true }
    return $false
}

function Wait-VyxCrashDialogsAfterProcessExit {
    param(
        [int]$RootPid,
        [hashtable]$IgnoredHandles = $null,
        [hashtable]$IgnoredReporterPids = $null,
        [hashtable]$IgnoredTargetPids = $null,
        [hashtable]$IgnoredExistingPids = $null,
        [string[]]$TargetProcessNames = @(),
        [string]$WorkingDirectory = "",
        [int]$ExitCode = 0,
        [string]$DialogLog,
        [string]$Header,
        [switch]$KillProcesses
    )
    if (-not (Test-VyxCrashLikeExitCode -ExitCode $ExitCode)) { return $false }
    $seconds = 8
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $dialogs = @(Get-VyxCrashDialogs -RootPid $RootPid -IgnoredHandles $IgnoredHandles -IgnoredReporterPids $IgnoredReporterPids -IgnoredTargetPids $IgnoredTargetPids -IgnoredExistingPids $IgnoredExistingPids -TargetProcessNames $TargetProcessNames -WorkingDirectory $WorkingDirectory -AllowGlobal)
        if ($dialogs.Count -gt 0) {
            Save-VyxCrashDialogReport -DialogLog $DialogLog -Header $Header -Dialogs $dialogs -KillProcesses:$KillProcesses
            return $true
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Quote-VyxCmdArgument {
    param([string]$Arg)
    if ($null -eq $Arg -or $Arg.Length -eq 0) { return '""' }
    if ($Arg -notmatch '[\s"&|<>^()]') { return $Arg }
    return '"' + $Arg.Replace('"', '\"') + '"'
}

function Close-VyxExistingCrashDialogs {
    param([string]$DialogLog = "")
    Initialize-VyxCrashSuppression
    $crashPattern = Get-VyxCrashDialogPattern
    $hits = @()

    foreach ($name in Get-VyxCrashReporterProcessNames) {
        try {
            foreach ($proc in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
                $hits += [pscustomobject]@{
                    ProcessId = [int]$proc.Id
                    ProcessName = $proc.ProcessName
                    Handle = [IntPtr]::Zero
                    Title = "crash reporter process"
                    Text = ("existing crash reporter process detected: pid={0} process={1}" -f $proc.Id, $proc.ProcessName)
                }
            }
        } catch {}
    }

    foreach ($hwnd in [VyxWin32.WindowProbe]::TopLevelWindows()) {
        $title = [VyxWin32.WindowProbe]::WindowText($hwnd)
        $tree = [VyxWin32.WindowProbe]::TreeText($hwnd)
        $joined = ($title + "`n" + $tree)
        if ($joined -notmatch $crashPattern) { continue }
        $procId = [VyxWin32.WindowProbe]::ProcessIdForWindow($hwnd)
        $processName = ""
        try { $processName = (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { $processName = "" }
        $hits += [pscustomobject]@{
            ProcessId = $procId
            ProcessName = $processName
            Handle = $hwnd
            Title = $title
            Text = $tree
        }
    }

    if ($hits.Count -eq 0) { return 0 }

    if (-not [string]::IsNullOrWhiteSpace($DialogLog)) {
        Save-VyxCrashDialogReport -DialogLog $DialogLog -Header "existing crash dialogs closed before launch" -Dialogs $hits
    } else {
        foreach ($d in $hits) {
            try {
                if ($d.Handle -ne [IntPtr]::Zero) {
                    [VyxWin32.WindowProbe]::CloseWindow($d.Handle)
                }
            } catch {}
            if (Test-VyxCrashReporterProcessName -Name $d.ProcessName) {
                try { Stop-Process -Id $d.ProcessId -Force -ErrorAction SilentlyContinue } catch {}
            }
        }
    }
    return $hits.Count
}

function Invoke-VyxProcess {
    param(
        [Parameter(Mandatory=$true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = "",
        [string]$StdoutLog = "",
        [string]$StderrLog = "",
        [string]$DialogLog = "",
        [string]$StdinText = "",
        [int]$TimeoutSec = 30,
        [int]$MemoryLimitMB = 0
    )

    Initialize-VyxCrashSuppression
    $targetProcessNames = @(Get-VyxTargetProcessNames -FilePath $FilePath -ArgumentList $ArgumentList)
    $crashReporterExclusions = Add-VyxCrashReporterExclusions -Names (Get-VyxCrashSuppressionExeNames -FilePath $FilePath -ArgumentList $ArgumentList)

    if ([string]::IsNullOrWhiteSpace($StdoutLog)) { $StdoutLog = [System.IO.Path]::GetTempFileName() }
    if ([string]::IsNullOrWhiteSpace($StderrLog)) { $StderrLog = [System.IO.Path]::GetTempFileName() }
    if ([string]::IsNullOrWhiteSpace($DialogLog)) { $DialogLog = [System.IO.Path]::ChangeExtension($StdoutLog, ".dialog.log") }
    foreach ($p in @($StdoutLog, $StderrLog, $DialogLog)) {
        $dir = Split-Path -Parent $p
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
        if (Test-Path $p) { [System.IO.File]::Delete($p) }
    }
    [void](Close-VyxExistingCrashDialogs -DialogLog $DialogLog)
    $ignoredCrashDialogs = @{}
    $ignoredReporterPids = Get-VyxProcessIdSet -Names (Get-VyxCrashReporterProcessNames)
    $ignoredTargetPids = Get-VyxProcessIdSet -Names $targetProcessNames
    $ignoredExistingPids = Get-VyxAllProcessIdSet

    # Direct ProcessStartInfo path with file-based redirection via cmd.
    # We can't safely use Process.StandardOutput/Error in a long-running
    # PS loop — both ObjectEvent and ReadToEndAsync end up deadlocking the
    # runspace when the child writes a few hundred lines of stderr fast
    # (e.g. host vyxc CodeGen phase logs). Letting cmd redirect to disk
    # sidesteps the OS pipes entirely.
    $hasStdinText = -not [string]::IsNullOrEmpty($StdinText)
    $isWindowsProcessHost = ([System.Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
    $stdinRedirectPath = ""
    if ($isWindowsProcessHost -and $hasStdinText) {
        $stdinRedirectPath = [System.IO.Path]::GetTempFileName()
        [System.IO.File]::WriteAllText(
            $stdinRedirectPath,
            $StdinText,
            [System.Text.UTF8Encoding]::new($false))
        # Windows stdin uses the same suspended cmd launch as build commands,
        # so the complete process tree enters the memory Job before resume.
        $hasStdinText = $false
    }
    if ($hasStdinText -or -not $isWindowsProcessHost) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $FilePath
        if ($ArgumentList -and $ArgumentList.Count -gt 0) {
            $argParts = @()
            foreach ($arg in $ArgumentList) {
                $escaped = ($arg -replace '"','\"')
                if ($arg -match '\s') { $argParts += '"' + $escaped + '"' }
                else { $argParts += $escaped }
            }
            $psi.Arguments = ($argParts -join ' ')
        }
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            $psi.WorkingDirectory = $WorkingDirectory
        }
        Set-VyxProcessCrashSuppressionEnvironment -ProcessStartInfo $psi

        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        $proc.EnableRaisingEvents = $false
        $jobHandle = New-VyxMemoryJob -MemoryLimitMB $MemoryLimitMB
        $jobLimited = $false
        try {
            [void]$proc.Start()
            $jobLimited = Add-VyxProcessToMemoryJob -JobHandle $jobHandle -Process $proc
            if (-not $jobLimited) {
                Close-VyxMemoryJob -JobHandle $jobHandle
                $jobHandle = [IntPtr]::Zero
            }
        } catch {
            "failed to launch: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
            Close-VyxMemoryJob -JobHandle $jobHandle
            Remove-VyxCrashReporterExclusions -Names $crashReporterExclusions
            return [pscustomobject]@{ ExitCode = -900; TimedOut = $false; DialogCaught = $false; MemoryExceeded = $false; PeakWorkingSetMB = 0.0; StdoutLog = $StdoutLog; StderrLog = $StderrLog; DialogLog = $DialogLog }
        }

        $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
        $stderrTask = $proc.StandardError.ReadToEndAsync()
        try {
            $proc.StandardInput.Write($StdinText)
            $proc.StandardInput.Close()
        } catch {
            try { $proc.StandardInput.Close() } catch {}
        }

        $dialogCaught = $false
        $timedOut = $false
        $memoryExceeded = $false
        $peakWorkingSetMB = 0.0
        if ($TimeoutSec -gt 0) {
            $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
            while (-not $proc.HasExited) {
                $remainingMs = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
                if ($remainingMs -le 1) {
                    $timedOut = $true
                    break
                }
                $waitMs = [Math]::Min(150, $remainingMs)
                if ($proc.WaitForExit($waitMs)) { break }
                if (Test-VyxProcessTreeMemoryLimit -RootPid $proc.Id -MemoryLimitMB $MemoryLimitMB -PeakWorkingSetMB ([ref]$peakWorkingSetMB)) {
                    $memoryExceeded = $true
                    "memory limit exceeded: peak ${peakWorkingSetMB}MB limit ${MemoryLimitMB}MB: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
                    Stop-VyxProcessTree -RootPid $proc.Id
                    break
                }
                $dialogs = @(Get-VyxCrashDialogs -RootPid $proc.Id -IgnoredHandles $ignoredCrashDialogs -IgnoredReporterPids $ignoredReporterPids -IgnoredTargetPids $ignoredTargetPids -IgnoredExistingPids $ignoredExistingPids -TargetProcessNames $targetProcessNames -WorkingDirectory $WorkingDirectory -AllowGlobal)
                if ($dialogs.Count -gt 0) {
                    $dialogCaught = $true
                    Save-VyxCrashDialogReport -DialogLog $DialogLog -Header "crash dialog captured: $FilePath $($ArgumentList -join ' ')" -Dialogs $dialogs -KillProcesses
                    try { Stop-VyxProcessTree -RootPid $proc.Id } catch {}
                    break
                }
            }
            if (-not $memoryExceeded -and (Test-VyxMemoryJobLimit -JobHandle $jobHandle -MemoryLimitMB $MemoryLimitMB -PeakWorkingSetMB ([ref]$peakWorkingSetMB))) {
                $memoryExceeded = $true
                "memory limit exceeded: peak ${peakWorkingSetMB}MB limit ${MemoryLimitMB}MB: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
            }
            if ($timedOut) {
                "timeout after $TimeoutSec sec: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
                try { Stop-VyxProcessTree -RootPid $proc.Id } catch {}
            }
        } else {
            $proc.WaitForExit()
        }

        $preExit = 0
        try { $preExit = $proc.ExitCode } catch {}
        if (-not $dialogCaught -and -not $timedOut) {
            $dialogCaught = Wait-VyxCrashDialogsAfterProcessExit -RootPid $proc.Id -IgnoredHandles $ignoredCrashDialogs -IgnoredReporterPids $ignoredReporterPids -IgnoredTargetPids $ignoredTargetPids -IgnoredExistingPids $ignoredExistingPids -TargetProcessNames $targetProcessNames -WorkingDirectory $WorkingDirectory -ExitCode $preExit -DialogLog $DialogLog -Header "crash dialog captured after process exit: $FilePath $($ArgumentList -join ' ')" -KillProcesses
            if ($dialogCaught) {
                try { Stop-VyxProcessTree -RootPid $proc.Id } catch {}
            }
        }

        try { $proc.WaitForExit(2000) | Out-Null } catch {}
        $stdout = ""
        $stderr = ""
        try { $stdout = $stdoutTask.GetAwaiter().GetResult() } catch {}
        try { $stderr = $stderrTask.GetAwaiter().GetResult() } catch {}
        Set-Content -Encoding utf8 -LiteralPath $StdoutLog -Value $stdout
        Set-Content -Encoding utf8 -LiteralPath $StderrLog -Value $stderr
        if (-not $memoryExceeded -and (Test-VyxMemoryJobLimit -JobHandle $jobHandle -MemoryLimitMB $MemoryLimitMB -PeakWorkingSetMB ([ref]$peakWorkingSetMB))) {
            $memoryExceeded = $true
            "memory limit exceeded: peak ${peakWorkingSetMB}MB limit ${MemoryLimitMB}MB: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
        }

        $exit = -902
        if ($timedOut) {
            $exit = -901
        } elseif ($memoryExceeded) {
            $exit = -905
        } elseif ($dialogCaught) {
            $exit = -904
        } else {
            try { $exit = $proc.ExitCode } catch { $exit = -903 }
            if (-not (Test-Path $DialogLog)) { "" | Set-Content -Encoding utf8 $DialogLog }
        }
        Close-VyxMemoryJob -JobHandle $jobHandle
        try { $proc.Dispose() } catch {}
        Remove-VyxCrashReporterExclusions -Names $crashReporterExclusions

        return [pscustomobject]@{
            ExitCode = $exit
            TimedOut = $timedOut
            DialogCaught = $dialogCaught
            MemoryExceeded = $memoryExceeded
            PeakWorkingSetMB = $peakWorkingSetMB
            JobLimited = $jobLimited
            StdoutLog = $StdoutLog
            StderrLog = $StderrLog
            DialogLog = $DialogLog
        }
    }

    $tmpStdout = [System.IO.Path]::GetTempFileName()
    $tmpStderr = [System.IO.Path]::GetTempFileName()

    $launchFilePath = $FilePath
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory) -and -not [System.IO.Path]::IsPathRooted($FilePath)) {
        $candidate = Join-Path $WorkingDirectory $FilePath
        if (Test-Path -LiteralPath $candidate) {
            try { $launchFilePath = (Resolve-Path -LiteralPath $candidate).Path } catch { $launchFilePath = $candidate }
        }
    } elseif (Test-Path -LiteralPath $FilePath) {
        try { $launchFilePath = (Resolve-Path -LiteralPath $FilePath).Path } catch { $launchFilePath = $FilePath }
    }

    $cmdParts = New-Object System.Collections.Generic.List[string]
    $cmdParts.Add((Quote-VyxCmdArgument $launchFilePath))
    foreach ($arg in $ArgumentList) {
        $cmdParts.Add((Quote-VyxCmdArgument $arg))
    }
    $stdinRedirect = ""
    if (-not [string]::IsNullOrWhiteSpace($stdinRedirectPath)) {
        $stdinRedirect = " 0< " + (Quote-VyxCmdArgument $stdinRedirectPath)
    }
    $cmdLine = ($cmdParts.ToArray() -join " ") + $stdinRedirect + " 1> " + (Quote-VyxCmdArgument $tmpStdout) + " 2> " + (Quote-VyxCmdArgument $tmpStderr)
    $cmdExe = $env:ComSpec
    if ([string]::IsNullOrWhiteSpace($cmdExe)) {
        $cmdExe = "cmd.exe"
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $cmdExe
    $psi.Arguments = '/D /C ' + $cmdLine
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $psi.WorkingDirectory = $WorkingDirectory
    }
    Set-VyxProcessCrashSuppressionEnvironment -ProcessStartInfo $psi

    $proc = $null
    $jobHandle = New-VyxMemoryJob -MemoryLimitMB $MemoryLimitMB
    $jobLimited = $false
    try {
        if ($MemoryLimitMB -gt 0) {
            $proc = Start-VyxProcessSuspendedInMemoryJob -ProcessStartInfo $psi -JobHandle $jobHandle
            $jobLimited = $true
        } else {
            $proc = New-Object System.Diagnostics.Process
            $proc.StartInfo = $psi
            $proc.EnableRaisingEvents = $false
            [void]$proc.Start()
        }
    } catch {
        "failed to launch: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
        foreach ($tmpPath in @($tmpStdout, $tmpStderr)) {
            if (Test-Path $tmpPath) { [System.IO.File]::Delete($tmpPath) }
        }
        if (-not [string]::IsNullOrWhiteSpace($stdinRedirectPath)) {
            if (Test-Path $stdinRedirectPath) { [System.IO.File]::Delete($stdinRedirectPath) }
        }
        Close-VyxMemoryJob -JobHandle $jobHandle
        if ($null -ne $proc) {
            try { $proc.Dispose() } catch {}
        }
        Remove-VyxCrashReporterExclusions -Names $crashReporterExclusions
        return [pscustomobject]@{ ExitCode = -900; TimedOut = $false; DialogCaught = $false; MemoryExceeded = $false; PeakWorkingSetMB = 0.0; JobLimited = $false; StdoutLog = $StdoutLog; StderrLog = $StderrLog; DialogLog = $DialogLog }
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $dialogCaught = $false
    $timedOut = $false
    $memoryExceeded = $false
    $peakWorkingSetMB = 0.0
    # Wait for the target process and poll for child assertion/runtime dialogs.
    if ($TimeoutSec -gt 0) {
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
        while (-not $proc.HasExited) {
            $remainingMs = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
            if ($remainingMs -le 1) {
                $timedOut = $true
                break
            }
            $waitMs = [Math]::Min(150, $remainingMs)
            if ($proc.WaitForExit($waitMs)) { break }
            if (Test-VyxProcessTreeMemoryLimit -RootPid $proc.Id -MemoryLimitMB $MemoryLimitMB -PeakWorkingSetMB ([ref]$peakWorkingSetMB)) {
                $memoryExceeded = $true
                "memory limit exceeded: peak ${peakWorkingSetMB}MB limit ${MemoryLimitMB}MB: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
                Stop-VyxProcessTree -RootPid $proc.Id
                break
            }
            $dialogs = @(Get-VyxCrashDialogs -RootPid $proc.Id -IgnoredHandles $ignoredCrashDialogs -IgnoredReporterPids $ignoredReporterPids -IgnoredTargetPids $ignoredTargetPids -IgnoredExistingPids $ignoredExistingPids -TargetProcessNames $targetProcessNames -WorkingDirectory $WorkingDirectory -AllowGlobal)
            if ($dialogs.Count -gt 0) {
                $dialogCaught = $true
                Save-VyxCrashDialogReport -DialogLog $DialogLog -Header "crash dialog captured: $FilePath $($ArgumentList -join ' ')" -Dialogs $dialogs -KillProcesses
                try { Stop-VyxProcessTree -RootPid $proc.Id } catch {}
                break
            }
        }
        if (-not $memoryExceeded -and (Test-VyxMemoryJobLimit -JobHandle $jobHandle -MemoryLimitMB $MemoryLimitMB -PeakWorkingSetMB ([ref]$peakWorkingSetMB))) {
            $memoryExceeded = $true
            "memory limit exceeded: peak ${peakWorkingSetMB}MB limit ${MemoryLimitMB}MB: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
        }
        if ($timedOut) {
            "timeout after $TimeoutSec sec: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
            try { Stop-VyxProcessTree -RootPid $proc.Id } catch {}
        }
    } else {
        while (-not $proc.HasExited) {
            if ($proc.WaitForExit(150)) { break }
            if (Test-VyxProcessTreeMemoryLimit -RootPid $proc.Id -MemoryLimitMB $MemoryLimitMB -PeakWorkingSetMB ([ref]$peakWorkingSetMB)) {
                $memoryExceeded = $true
                "memory limit exceeded: peak ${peakWorkingSetMB}MB limit ${MemoryLimitMB}MB: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
                Stop-VyxProcessTree -RootPid $proc.Id
                break
            }
            $dialogs = @(Get-VyxCrashDialogs -RootPid $proc.Id -IgnoredHandles $ignoredCrashDialogs -IgnoredReporterPids $ignoredReporterPids -IgnoredTargetPids $ignoredTargetPids -IgnoredExistingPids $ignoredExistingPids -TargetProcessNames $targetProcessNames -WorkingDirectory $WorkingDirectory -AllowGlobal)
            if ($dialogs.Count -gt 0) {
                $dialogCaught = $true
                Save-VyxCrashDialogReport -DialogLog $DialogLog -Header "crash dialog captured: $FilePath $($ArgumentList -join ' ')" -Dialogs $dialogs -KillProcesses
                try { Stop-VyxProcessTree -RootPid $proc.Id } catch {}
                break
            }
        }
    }

    $preExit = 0
    try { $preExit = $proc.ExitCode } catch {}
    if (-not $dialogCaught -and -not $timedOut) {
        $dialogCaught = Wait-VyxCrashDialogsAfterProcessExit -RootPid $proc.Id -IgnoredHandles $ignoredCrashDialogs -IgnoredReporterPids $ignoredReporterPids -IgnoredTargetPids $ignoredTargetPids -IgnoredExistingPids $ignoredExistingPids -TargetProcessNames $targetProcessNames -WorkingDirectory $WorkingDirectory -ExitCode $preExit -DialogLog $DialogLog -Header "crash dialog captured after process exit: $FilePath $($ArgumentList -join ' ')" -KillProcesses
        if ($dialogCaught) {
            try { Stop-VyxProcessTree -RootPid $proc.Id } catch {}
        }
    }

    try { $proc.WaitForExit(2000) | Out-Null } catch {}
    $exit = -902
    if ($timedOut) {
        $exit = -901
    } elseif ($memoryExceeded) {
        $exit = -905
    } elseif ($dialogCaught) {
        $exit = -904
    } else {
        try { $exit = $proc.ExitCode } catch { $exit = -903 }
    }
    if (-not $memoryExceeded -and (Test-VyxMemoryJobLimit -JobHandle $jobHandle -MemoryLimitMB $MemoryLimitMB -PeakWorkingSetMB ([ref]$peakWorkingSetMB))) {
        $memoryExceeded = $true
        "memory limit exceeded: peak ${peakWorkingSetMB}MB limit ${MemoryLimitMB}MB: $FilePath $($ArgumentList -join ' ')" | Set-Content -Encoding utf8 $DialogLog
        $exit = -905
    }

    # Copy the redirected output files into the caller-requested log paths
    # then remove the temp files.
    try {
        if (Test-Path $tmpStdout) { Copy-Item -LiteralPath $tmpStdout -Destination $StdoutLog -Force -ErrorAction SilentlyContinue }
        else { "" | Set-Content -Encoding utf8 $StdoutLog }
    } catch {}
    try {
        if (Test-Path $tmpStderr) { Copy-Item -LiteralPath $tmpStderr -Destination $StderrLog -Force -ErrorAction SilentlyContinue }
        else { "" | Set-Content -Encoding utf8 $StderrLog }
    } catch {}
    if (-not (Test-Path $DialogLog)) { "" | Set-Content -Encoding utf8 $DialogLog }
    foreach ($tmpPath in @($tmpStdout, $tmpStderr)) {
        if (Test-Path $tmpPath) { [System.IO.File]::Delete($tmpPath) }
    }
    Close-VyxMemoryJob -JobHandle $jobHandle
    if (-not [string]::IsNullOrWhiteSpace($stdinRedirectPath)) {
        # Closing the kill-on-close Job releases any inherited stdin handle
        # held by a descendant before the temporary input file is removed.
        if (Test-Path $stdinRedirectPath) { [System.IO.File]::Delete($stdinRedirectPath) }
    }
    try { $proc.Dispose() } catch {}
    Remove-VyxCrashReporterExclusions -Names $crashReporterExclusions

    return [pscustomobject]@{
        ExitCode = $exit
        TimedOut = $timedOut
        DialogCaught = $dialogCaught
        MemoryExceeded = $memoryExceeded
        PeakWorkingSetMB = $peakWorkingSetMB
        JobLimited = $jobLimited
        StdoutLog = $StdoutLog
        StderrLog = $StderrLog
        DialogLog = $DialogLog
    }
}

Initialize-VyxCrashSuppression

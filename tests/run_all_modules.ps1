# 仅扫描本目录树下所有 .vyx；在仓库根目录执行 vyxc --run（以便解析 std/）。
# 脚本位于 tests/ 根目录。
#
# Layout: cases/, projects/, checks/, bootstrap/, snapshots/.
# `cases/` contains loose executable modules. `projects/` contains manifest
# integration fixtures. `checks/` contains explicit PowerShell contract checks
# and is intentionally excluded from this module/project runner; use
# tests/checks/run_checks.ps1 to execute them.
# Usage（在仓库根）:
#   powershell -File tests/run_all_modules.ps1
#   powershell -File tests/run_all_modules.ps1 -ListOnly
#   powershell -File tests/run_all_modules.ps1 -PathFilter "cases\\stress"
#   # Legacy filters such as _adversarial2 and common remain accepted.
#   powershell -File tests/run_all_modules.ps1 -BootstrapCompiler E:\path\to\vyxc.exe
#   powershell -File tests/run_all_modules.ps1 -BootstrapCompiler E:\path\to\vyxc.exe -BootstrapCompileOnly
#   powershell -File tests/run_all_modules.ps1 -BootstrapCompiler E:\path\to\vyxc.exe -ProjectJobs 1
#   powershell -File tests/run_all_modules.ps1 -LogPath tests\.cache\full_tests.log
#   powershell -File tests/run_all_modules.ps1 -RuntimeDir build_yolo_vyxcg\vyx_codegen
#
# Env: VYX_HOST_VYXC, VYX_BOOTSTRAP_VYXC

param(
    [string]$HostCompiler = "",
    [string]$BootstrapCompiler = "",
    [string[]]$HostCompilerArgs = @(),
    [string[]]$BootstrapCompilerArgs = @(),
    [switch]$HostOnly,
    [switch]$BootstrapOnly,
    [switch]$BootstrapCompileOnly,
    [int]$RunTimeoutSec = 30,
    [int]$MemoryLimitMB = 0,
    # Zero preserves each project's Vyx.toml resource policy. Set a positive
    # value only when the caller deliberately needs a global test cap.
    [int]$ProjectJobs = 0,
    [string]$PathFilter = "",
    [string]$LogPath = "",
    [string]$RuntimeDir = "",
    [string]$RuntimeLib = "",
    [switch]$ListOnly
)

$ErrorActionPreference = "Stop"

$script:RunAllModulesIsWindows = $false
if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    $script:RunAllModulesIsWindows = [bool]$IsWindows
} else {
    $script:RunAllModulesIsWindows = ($env:OS -eq "Windows_NT")
}

# tests/ ->仓库根
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$testsRoot = $PSScriptRoot

function Resolve-TestPathFilter {
    param([string]$Filter)
    if ([string]::IsNullOrWhiteSpace($Filter)) { return "" }

    # PathFilter used to expose the pre-layout directory names. Keep those
    # names as CLI aliases so focused local/CI commands remain stable while
    # source files live under their semantic suites.
    $normalized = $Filter.Replace('/', '\')
    # Canonical filters are already valid substrings of repository-relative
    # paths. Return them before legacy aliases can rewrite a segment inside
    # their canonical destination.
    if ($normalized -match '(?i)(^|\\)(cases|projects|checks|snapshots)(\\|$)') {
        return $normalized
    }
    $aliases = @(
        @{ Old = "tests\_adversarial2"; New = "tests\cases\conformance\adversarial\round2" },
        @{ Old = "_adversarial2"; New = "cases\conformance\adversarial\round2" },
        @{ Old = "tests\_adversarial3"; New = "tests\cases\conformance\adversarial\round3" },
        @{ Old = "_adversarial3"; New = "cases\conformance\adversarial\round3" },
        # The legacy unsuffixed filter historically selected every adversarial
        # round through substring matching, so retain the broader suite alias.
        @{ Old = "tests\_adversarial"; New = "tests\cases\conformance\adversarial" },
        @{ Old = "_adversarial"; New = "cases\conformance\adversarial" },
        @{ Old = "tests\_strict"; New = "tests\cases\conformance\strict" },
        @{ Old = "_strict"; New = "cases\conformance\strict" },
        @{ Old = "tests\_generics_industrial"; New = "tests\cases\conformance\generics\industrial" },
        @{ Old = "_generics_industrial"; New = "cases\conformance\generics\industrial" },
        @{ Old = "tests\_beyond_cpp26"; New = "tests\cases\conformance\extensions\beyond_cpp26" },
        @{ Old = "_beyond_cpp26"; New = "cases\conformance\extensions\beyond_cpp26" },
        @{ Old = "tests\_p7_algorithm"; New = "tests\cases\conformance\stdlib\algorithm" },
        @{ Old = "_p7_algorithm"; New = "cases\conformance\stdlib\algorithm" },
        @{ Old = "tests\_p7_iter_trait"; New = "tests\cases\conformance\stdlib\iter_trait" },
        @{ Old = "_p7_iter_trait"; New = "cases\conformance\stdlib\iter_trait" },
        @{ Old = "tests\common"; New = "tests\cases\smoke\common" },
        @{ Old = "common"; New = "cases\smoke\common" },
        @{ Old = "tests\projects\interpreters_io"; New = "tests\cases\interpreters" },
        @{ Old = "projects\interpreters_io"; New = "cases\interpreters" },
        @{ Old = "tests\build_system_smoke"; New = "tests\projects\build_system_smoke" },
        @{ Old = "build_system_smoke"; New = "projects\build_system_smoke" }
    )
    foreach ($alias in $aliases) {
        $old = [string]$alias.Old
        if ($normalized.IndexOf($old, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $normalized = [regex]::Replace($normalized,
                                            "(?i)" + [regex]::Escape($old),
                                            [string]$alias.New)
            return $normalized
        }
    }
    return $normalized
}

$PathFilter = Resolve-TestPathFilter $PathFilter
$cacheRoot = Join-Path $testsRoot ".cache"
if (-not (Test-Path $cacheRoot)) {
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
}
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $cacheRoot ("run_all_modules_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".log")
} elseif (-not [System.IO.Path]::IsPathRooted($LogPath)) {
    $LogPath = Join-Path $repoRoot $LogPath
}
$LogPath = [System.IO.Path]::GetFullPath($LogPath)
$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
}
"" | Set-Content -Encoding utf8 -LiteralPath $LogPath
$script:RunAllModulesLogPath = $LogPath

$processHelper = Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1"
if (Test-Path $processHelper) {
    . $processHelper
}
if (Get-Command Initialize-VyxCrashSuppression -ErrorAction SilentlyContinue) {
    Initialize-VyxCrashSuppression
}

function Write-RunLine {
    param([string]$Message = "")
    Write-Host $Message
    try { [Console]::Out.Flush() } catch {}
    if (-not [string]::IsNullOrWhiteSpace($script:RunAllModulesLogPath)) {
        Add-Content -Encoding utf8 -LiteralPath $script:RunAllModulesLogPath -Value $Message
    }
}

function Add-RunLogFile {
    param([string]$Label, [string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) { return }
    Add-Content -Encoding utf8 -LiteralPath $script:RunAllModulesLogPath -Value ""
    Add-Content -Encoding utf8 -LiteralPath $script:RunAllModulesLogPath -Value ("----- " + $Label + " : " + $Path + " -----")
    Add-Content -Encoding utf8 -LiteralPath $script:RunAllModulesLogPath -Value (Get-Content -Raw -LiteralPath $Path)
}

function Write-ProgressSummary {
    param(
        [int]$Done,
        [int]$Total,
        [int]$Passed,
        [int]$Skipped = 0,
        [System.Collections.Generic.List[string]]$FailedFiles
    )
    Write-RunLine ("进度: {0}/{1}  成功: {2}  跳过: {3}  失败: {4}" -f $Done, $Total, $Passed, $Skipped, $FailedFiles.Count)
    if ($FailedFiles.Count -eq 0) {
        Write-RunLine "失败文件: 无"
        return
    }
    $recent = New-Object System.Collections.Generic.List[string]
    $start = [Math]::Max(0, $FailedFiles.Count - 5)
    for ($i = $start; $i -lt $FailedFiles.Count; $i++) {
        $recent.Add($FailedFiles[$i])
    }
    Write-RunLine ("最近失败: " + ($recent.ToArray() -join ", "))
}

function Quote-RunAllArg {
    param([string]$Arg)
    if ($null -eq $Arg -or $Arg.Length -eq 0) { return '""' }
    if ($Arg -notmatch '[\s"]') { return $Arg }
    return '"' + $Arg.Replace('"', '\"') + '"'
}

function Invoke-RunAllLocalProcess {
    param(
        [Parameter(Mandatory=$true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
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
        if (Test-Path $p) {
            [System.IO.File]::Delete($p)
        }
    }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FilePath
    $psi.Arguments = ($ArgumentList | ForEach-Object { Quote-RunAllArg $_ }) -join ' '
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
            StdoutLog = $StdoutLog
            StderrLog = $StderrLog
            DialogLog = $DialogLog
        }
    } finally {
        try { $proc.Dispose() } catch {}
    }
}

function Resolve-HostCompiler {
    param([string]$Explicit)
    if ($Explicit -ne "") { return (Resolve-Path $Explicit).Path }
    if ($env:VYX_HOST_VYXC -and (Test-Path $env:VYX_HOST_VYXC)) {
        return (Resolve-Path $env:VYX_HOST_VYXC).Path
    }
    foreach ($cand in @(
        (Join-Path $repoRoot "cmake-build-debug\vyxc.exe"),
        (Join-Path $repoRoot "build\vyxc.exe")
    )) {
        if (Test-Path $cand) { return (Resolve-Path $cand).Path }
    }
    return $null
}

function Resolve-BootstrapCompiler {
    param([string]$Explicit)
    if ($Explicit -ne "") { return (Resolve-Path $Explicit).Path }
    if ($env:VYX_BOOTSTRAP_VYXC -and (Test-Path $env:VYX_BOOTSTRAP_VYXC)) {
        return (Resolve-Path $env:VYX_BOOTSTRAP_VYXC).Path
    }
    return $null
}

function Resolve-RuntimeDir {
    param([string]$Explicit)
    if ($Explicit -ne "") { return (Resolve-Path $Explicit).Path }
    foreach ($d in @(
        (Join-Path $repoRoot "bootstrap_compiler\out"),
        (Join-Path $repoRoot "bootstrap_compiler\out\vyx_rt"),
        (Join-Path $repoRoot "build_yolo_vyxcg\vyx_codegen"),
        (Join-Path $repoRoot "build_yolo_vyxcg\vyx_rt"),
        (Join-Path $repoRoot "build_vyxcg\vyx_codegen"),
        (Join-Path $repoRoot "build_vyxcg\vyx_rt"),
        (Join-Path $repoRoot "build\vyx_codegen"),
        (Join-Path $repoRoot "build\vyx_rt"),
        (Join-Path $repoRoot "cmake-build-debug\vyx_codegen"),
        (Join-Path $repoRoot "cmake-build-debug\vyx_rt")
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
            return (Resolve-Path $d).Path
        }
    }
    return $null
}

function Resolve-RuntimeLib {
    param(
        [string]$Explicit,
        [string]$Dir
    )
    if ($Explicit -ne "") { return $Explicit }
    if ([string]::IsNullOrWhiteSpace($Dir)) { return "vyx_runtime" }
    foreach ($lib in @("vyx_runtime", "vyx_rt", "vyx_codegen")) {
        if ((Test-Path (Join-Path $Dir ($lib + ".lib"))) -or
            (Test-Path (Join-Path $Dir ("lib" + $lib + ".a"))) -or
            (Test-Path (Join-Path $Dir ("lib" + $lib + ".dll.a"))) -or
            (Test-Path (Join-Path $Dir ("lib" + $lib + ".so"))) -or
            (Test-Path (Join-Path $Dir ("lib" + $lib + ".dylib")))) {
            return $lib
        }
    }
    return "vyx_runtime"
}

function Sync-RuntimeBinaries {
    param(
        [string]$SourceDir,
        [string]$DestinationDir,
        [string]$RuntimeLibName
    )
    if ([string]::IsNullOrWhiteSpace($SourceDir) -or
        [string]::IsNullOrWhiteSpace($DestinationDir) -or
        -not (Test-Path $SourceDir)) {
        return
    }
    if (-not (Test-Path $DestinationDir)) {
        New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
    }

    $names = New-Object System.Collections.Generic.List[string]
    foreach ($name in @(
        ($RuntimeLibName + ".dll"),
        ("lib" + $RuntimeLibName + ".so"),
        ("lib" + $RuntimeLibName + ".dylib"),
        "vyx_compiler_backend.dll"
    )) {
        if (-not [string]::IsNullOrWhiteSpace($name) -and -not $names.Contains($name)) {
            $names.Add($name)
        }
    }

    foreach ($name in $names) {
        $src = Join-Path $SourceDir $name
        if (Test-Path $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $DestinationDir $name) -Force
        }
    }
}

function Get-RepoRelative {
    param([string]$FullPath)
    $separators = [char[]]@('\', '/')
    $normRepo = $repoRoot.TrimEnd($separators)
    $normFull = $FullPath
    if ($normFull.StartsWith($normRepo, [StringComparison]::OrdinalIgnoreCase)) {
        return $normFull.Substring($normRepo.Length).TrimStart($separators)
    }
    return $FullPath
}

function Test-ExcludedRelative {
    param([string]$Rel)
    $r = $Rel.Replace('/', '\')
    if ($r -match '(?i)(^|\\)[^\\]*cache[^\\]*(\\|$)') { return $true }
    if ($r -match '\\target\\') { return $true }
    # mir2cpp is not a supported backend; its projects are not part of the
    # test suite's supported surface.
    if ($r -match '(?i)(^|\\)mir2cpp[^\\]*($|\\)') { return $true }
    return $false
}

function Test-HasEntryMain {
    param([string]$FilePath)
    foreach ($line in [System.IO.File]::ReadAllLines($FilePath)) {
        $t = $line.TrimStart()
        if ($t.StartsWith("//")) { continue }
        if ($t -match '^(public\s+)?fn\s+main\s*\(') { return $true }
    }
    return $false
}

function Test-ProjectBuildOnly {
    param([string]$FilePath)
    foreach ($line in [System.IO.File]::ReadLines($FilePath)) {
        if ($line.IndexOf("PROJECT-BUILD-ONLY", [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }
    return $false
}

function Find-NearestProjectRoot {
    param([string]$FilePath)
    $dir = Split-Path -Parent ([System.IO.Path]::GetFullPath($FilePath))
    $projectsRoot = Join-Path $testsRoot "projects"
    while (-not [string]::IsNullOrWhiteSpace($dir)) {
        if ($dir.StartsWith($projectsRoot, [StringComparison]::OrdinalIgnoreCase) -and
            (Test-Path (Join-Path $dir "Vyx.toml"))) {
            return $dir
        }
        if ($dir.Equals($testsRoot, [StringComparison]::OrdinalIgnoreCase)) { break }
        $parent = Split-Path -Parent $dir
        if ($parent -eq $dir) { break }
        $dir = $parent
    }
    return ""
}

function Test-UnderProjectManifest {
    param([string]$FilePath)
    return -not [string]::IsNullOrWhiteSpace((Find-NearestProjectRoot $FilePath))
}

function Get-ProjectTestDirs {
    $projectsRoot = Join-Path $testsRoot "projects"
    $list = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path $projectsRoot)) { return $list }
    foreach ($m in @(Get-ChildItem -Path $projectsRoot -Recurse -Filter "Vyx.toml" -File)) {
        $dir = $m.DirectoryName
        $rel = Get-RepoRelative $dir
        $normalizedRel = $rel.Replace('/', '\')
        if (Test-ExcludedRelative $rel) { continue }
        if ($PathFilter -ne "" -and ($normalizedRel.IndexOf($PathFilter, [StringComparison]::OrdinalIgnoreCase) -lt 0)) { continue }
        $list.Add($dir)
    }
    return $list | Sort-Object
}

function Get-ManifestStringValue {
    param([string]$ManifestPath, [string]$Key, [string]$DefaultValue = "")
    foreach ($line in [System.IO.File]::ReadLines($ManifestPath)) {
        $trim = $line.Trim()
        if ($trim -match ("^" + [regex]::Escape($Key) + "\s*=\s*`"([^`"]+)`"")) {
            return $Matches[1]
        }
    }
    return $DefaultValue
}

function Get-ProjectPackageName {
    param([string]$ProjectDir)
    return Get-ManifestStringValue (Join-Path $ProjectDir "Vyx.toml") "name" ([System.IO.Path]::GetFileName($ProjectDir))
}

function Get-ProjectOutputDir {
    param([string]$ProjectDir)
    return Get-ManifestStringValue (Join-Path $ProjectDir "Vyx.toml") "output_dir" "target"
}

function Test-ProjectExpectedFailure {
    param([string]$ProjectDir)
    $rel = (Get-RepoRelative $ProjectDir).Replace('/', '\')
    if ($rel -match '\\negative($|\\)' -or $rel -match '_negative($|\\)') { return $true }
    return Test-ExpectedFailure (Join-Path $ProjectDir "Vyx.toml")
}

function Test-ExpectedFailure {
    param([string]$FilePath)
    if (Test-ExpectedRuntimeFailure $FilePath) { return $false }

    $rel = (Get-RepoRelative $FilePath).Replace('/', '\')
    if ($rel -match '\\negative\\') { return $true }

    $name = [System.IO.Path]::GetFileName($FilePath)
    if ($name -match '^err_') { return $true }

    $text = [System.IO.File]::ReadAllText($FilePath)
    foreach ($marker in @(
        "NEGATIVE:",
        "STRICT-NEGATIVE",
        "negative sample",
        "Expected diagnostic",
        "Expected sema output",
        "expected compile",
        "SHOULD produce a compile error",
        "MUST fail",
        "should fail",
        "Should error gracefully",
        "multi-error harvest",
        "error recovery"
    )) {
        # Expected-failure markers are test annotations. Do not treat an
        # ordinary identifier such as `negative: f64` as `NEGATIVE:`.
        if ([regex]::IsMatch($text,
                              '(?im)^\s*(?://|/\*)[^\r\n]*' + [regex]::Escape($marker))) {
            return $true
        }
    }
    return $false
}

function Test-ExpectedRuntimeFailure {
    param([string]$FilePath)
    $text = [System.IO.File]::ReadAllText($FilePath)
    foreach ($marker in @(
        "RUNTIME-NEGATIVE:",
        "Expected runtime failure",
        "MUST runtime fail",
        "must terminate at runtime"
    )) {
        if ([regex]::IsMatch($text,
                              '(?im)^\s*(?://|/\*)[^\r\n]*' + [regex]::Escape($marker))) {
            return $true
        }
    }
    return $false
}

function Get-ModuleTestFiles {
    $all = @(Get-ChildItem -Path $testsRoot -Recurse -Filter "*.vyx" -File)
    $list = New-Object System.Collections.Generic.List[string]
    foreach ($f in $all) {
        $rel = Get-RepoRelative $f.FullName
        if (Test-ExcludedRelative $rel) { continue }
        $normalizedRel = $rel.Replace('/', '\')
        if ($normalizedRel.StartsWith("tests\checks\", [StringComparison]::OrdinalIgnoreCase)) { continue }
        # Bootstrap probes are explicit contract checks with their own runners;
        # do not reinterpret their helper/negative fixtures as loose modules.
        if ($normalizedRel.StartsWith("tests\bootstrap\", [StringComparison]::OrdinalIgnoreCase)) { continue }
        if (Test-UnderProjectManifest $f.FullName) { continue }
        if (Test-ProjectBuildOnly $f.FullName) { continue }
        if (-not (Test-HasEntryMain $f.FullName)) { continue }
        if ($PathFilter -ne "" -and ($normalizedRel.IndexOf($PathFilter, [StringComparison]::OrdinalIgnoreCase) -lt 0)) { continue }
        $list.Add($f.FullName)
    }
    return $list | Sort-Object
}

$vyxcHost = Resolve-HostCompiler $HostCompiler
$vyxcBoot = Resolve-BootstrapCompiler $BootstrapCompiler
$resolvedRuntimeDir = Resolve-RuntimeDir $RuntimeDir
$resolvedRuntimeLib = Resolve-RuntimeLib $RuntimeLib $resolvedRuntimeDir
$runtimeArgs = @()
if ($resolvedRuntimeDir) {
    if ($script:RunAllModulesIsWindows) {
        $env:Path = $resolvedRuntimeDir + ";" + $env:Path
    } else {
        $env:PATH = $resolvedRuntimeDir + ":" + $env:PATH
        $env:LD_LIBRARY_PATH = $resolvedRuntimeDir + ":" + $env:LD_LIBRARY_PATH
    }
    $runtimeArgs = @("-L", $resolvedRuntimeDir, "-l", $resolvedRuntimeLib)
}

if (-not $BootstrapOnly -and -not $vyxcHost) {
    Write-Error "Host vyxc not found. Build vyxc or set -HostCompiler / `$env:VYX_HOST_VYXC"
}
if ($BootstrapOnly -and -not $vyxcBoot) {
    Write-Error "Bootstrap vyxc not found. Pass -BootstrapCompiler or set `$env:VYX_BOOTSTRAP_VYXC"
}

$moduleFiles = @(Get-ModuleTestFiles)
$projectDirs = @(Get-ProjectTestDirs)
if ($moduleFiles.Count -eq 0 -and $projectDirs.Count -eq 0) {
    Write-Error "No runnable test modules/projects found under tests/ (check -PathFilter)."
}

$bootstrapOutDir = Join-Path $testsRoot ".cache\bootstrap_modules"
$bootstrapRunDir = Join-Path $testsRoot ".cache\bootstrap_runs"
$bootstrapRunLogDir = Join-Path $testsRoot ".cache\bootstrap_run_logs"
$processLogRoot = Join-Path $testsRoot ".cache\run_all_modules_process_logs"
if (-not $HostOnly -and $vyxcBoot) {
    $needDir = $bootstrapRunDir
    if ($BootstrapCompileOnly) { $needDir = $bootstrapOutDir }
    if (-not (Test-Path $needDir)) {
        New-Item -ItemType Directory -Force -Path $needDir | Out-Null
    }
    if (-not $BootstrapCompileOnly -and -not (Test-Path $bootstrapRunLogDir)) {
        New-Item -ItemType Directory -Force -Path $bootstrapRunLogDir | Out-Null
    }
}
if (-not (Test-Path $processLogRoot)) {
    New-Item -ItemType Directory -Force -Path $processLogRoot | Out-Null
}
if (-not $HostOnly -and $vyxcBoot -and -not $BootstrapCompileOnly) {
    Sync-RuntimeBinaries -SourceDir $resolvedRuntimeDir -DestinationDir $bootstrapRunDir -RuntimeLibName $resolvedRuntimeLib
}

function Get-SafeOutputStem {
    param([string]$Rel)
    return ($Rel -replace '[\\/:*?"<>|]', '_')
}

function Invoke-LoggedTestProcess {
    param(
        [Parameter(Mandatory=$true)][string]$Stage,
        [Parameter(Mandatory=$true)][string]$Rel,
        [Parameter(Mandatory=$true)][string]$Exe,
        [string[]]$Args = @(),
        [Parameter(Mandatory=$true)][string]$SafeStem,
        [string]$WorkingDirectory = "",
        [int]$Seconds = 30,
        [int]$MemoryLimitMB = 0
    )

    $stageDir = Join-Path $processLogRoot $Stage
    if (-not (Test-Path $stageDir)) {
        New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
    }
    $stdoutLog = Join-Path $stageDir ($SafeStem + ".stdout.log")
    $stderrLog = Join-Path $stageDir ($SafeStem + ".stderr.log")
    $dialogLog = Join-Path $stageDir ($SafeStem + ".dialog.log")

    Add-Content -Encoding utf8 -LiteralPath $script:RunAllModulesLogPath -Value ""
    Add-Content -Encoding utf8 -LiteralPath $script:RunAllModulesLogPath -Value ("===== [{0}] {1} =====" -f $Stage, $Rel)
    Add-Content -Encoding utf8 -LiteralPath $script:RunAllModulesLogPath -Value ("command: {0} {1}" -f $Exe, ($Args -join " "))

    if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $WorkingDirectory = $repoRoot
    }

    $run = Invoke-RunAllLocalProcess -FilePath $Exe `
        -ArgumentList $Args `
        -WorkingDirectory $WorkingDirectory `
        -StdoutLog $stdoutLog `
        -StderrLog $stderrLog `
        -DialogLog $dialogLog `
        -TimeoutSec $Seconds `
        -MemoryLimitMB $MemoryLimitMB
    Add-Content -Encoding utf8 -LiteralPath $script:RunAllModulesLogPath -Value ("exit: {0} timeout: {1} dialog: {2} memory_exceeded: {3} peak_mb: {4}" -f $run.ExitCode, $run.TimedOut, $run.DialogCaught, $run.MemoryExceeded, $run.PeakWorkingSetMB)
    Add-RunLogFile -Label "stdout" -Path $run.StdoutLog
    Add-RunLogFile -Label "stderr" -Path $run.StderrLog
    Add-RunLogFile -Label "dialog" -Path $run.DialogLog
    return $run
}

if ($ListOnly) {
    foreach ($p in $moduleFiles) { Write-RunLine (Get-RepoRelative $p) }
    foreach ($p in $projectDirs) { Write-RunLine ((Get-RepoRelative $p) + " [project]") }
    Write-RunLine ("完整日志: " + $LogPath)
    exit 0
}

Write-RunLine "Discovered $($moduleFiles.Count) independent module(s) under tests/"
Write-RunLine "Discovered $($projectDirs.Count) project(s) under tests/projects/"
Write-RunLine ("完整日志: " + $LogPath)
if ($resolvedRuntimeDir) {
    Write-RunLine ("runtime: " + $resolvedRuntimeDir + " / " + $resolvedRuntimeLib)
} else {
    Write-RunLine "runtime: 未找到，命令不会追加 -L/-l"
}
Write-RunLine ""

$stageCount = 0
if (-not $BootstrapOnly) { $stageCount++ }
if (-not $HostOnly -and $vyxcBoot) { $stageCount++ }
$totalRuns = ($moduleFiles.Count + $projectDirs.Count) * $stageCount
$doneRuns = 0
$passedRuns = 0
$skippedRuns = 0
$failed = 0
$failedFiles = New-Object System.Collections.Generic.List[string]

function Invoke-ProjectCase {
    param(
        [Parameter(Mandatory=$true)][string]$Stage,
        [Parameter(Mandatory=$true)][string]$ProjectDir,
        [Parameter(Mandatory=$true)][string]$CompilerExe,
        [string[]]$CompilerArgs = @()
    )

    $rel = Get-RepoRelative $ProjectDir
    $safeStem = Get-SafeOutputStem ($rel + "\project")
    $expectedFailure = Test-ProjectExpectedFailure $ProjectDir
    $runScript = Join-Path $ProjectDir "run.ps1"

    if ((Test-Path $runScript) -and -not $BootstrapCompileOnly) {
        $psArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $runScript)
        $scriptText = [System.IO.File]::ReadAllText($runScript)
        if ($scriptText.IndexOf('BootstrapCompiler', [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $psArgs += @("-BootstrapCompiler", $CompilerExe)
        } elseif ($scriptText.IndexOf('[string]$Compiler', [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $psArgs += @("-Compiler", $CompilerExe)
        }
        if ($resolvedRuntimeDir -and $scriptText.IndexOf('RuntimeDir', [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $psArgs += @("-RuntimeDir", $resolvedRuntimeDir)
        }
        $run = Invoke-LoggedTestProcess -Stage ($Stage + "-project") `
            -Rel ($rel + " [project:run.ps1]") `
            -Exe "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" `
            -Args $psArgs `
            -SafeStem $safeStem `
            -WorkingDirectory $ProjectDir `
            -Seconds ([Math]::Max($RunTimeoutSec, 300)) `
            -MemoryLimitMB $MemoryLimitMB
        $ok = $false
        $skipped = ($run.ExitCode -eq 77 -and -not $run.TimedOut -and
                    -not $run.DialogCaught -and -not $run.MemoryExceeded)
        if ($skipped) {
            $ok = $true
        } elseif ($expectedFailure) {
            $ok = ($run.ExitCode -eq 1)
        } else {
            $ok = ($run.ExitCode -eq 0 -and -not $run.TimedOut -and -not $run.DialogCaught -and -not $run.MemoryExceeded)
        }
        return [pscustomobject]@{ Ok = $ok; Skipped = $skipped; ExitCode = $run.ExitCode; Run = $run }
    }

    $buildArgs = @("build")
    if ($ProjectJobs -gt 0) {
        $buildArgs += @("-j", $ProjectJobs.ToString())
    }
    $buildArgs += $CompilerArgs
    $build = Invoke-LoggedTestProcess -Stage ($Stage + "-project") `
        -Rel ($rel + " [project:build]") `
        -Exe $CompilerExe `
        -Args $buildArgs `
        -SafeStem $safeStem `
        -WorkingDirectory $ProjectDir `
        -Seconds ([Math]::Max($RunTimeoutSec, 300)) `
        -MemoryLimitMB $MemoryLimitMB

    if ($expectedFailure) {
        return [pscustomobject]@{ Ok = ($build.ExitCode -eq 1); Skipped = $false; ExitCode = $build.ExitCode; Run = $build }
    }
    if ($build.ExitCode -ne 0 -or $build.TimedOut -or $build.DialogCaught -or $build.MemoryExceeded) {
        return [pscustomobject]@{ Ok = $false; Skipped = $false; ExitCode = $build.ExitCode; Run = $build }
    }
    if ($BootstrapCompileOnly) {
        return [pscustomobject]@{ Ok = $true; Skipped = $false; ExitCode = 0; Run = $build }
    }

    $pkg = Get-ProjectPackageName $ProjectDir
    $outDirName = Get-ProjectOutputDir $ProjectDir
    $exeSuffix = if ($script:RunAllModulesIsWindows) { ".exe" } else { "" }
    $exePath = Join-Path (Join-Path $ProjectDir $outDirName) ($pkg + $exeSuffix)
    if (-not (Test-Path $exePath)) {
        return [pscustomobject]@{ Ok = $true; Skipped = $false; ExitCode = 0; Run = $build }
    }

    $runtimePathEntries = New-Object System.Collections.Generic.List[string]
    $exeDir = Split-Path -Parent $exePath
    if ($exeDir -and -not $runtimePathEntries.Contains($exeDir)) { $runtimePathEntries.Add($exeDir) }
    foreach ($dll in @(Get-ChildItem -Path $ProjectDir -Recurse -Filter "*.dll" -File -ErrorAction SilentlyContinue)) {
        $dllDir = Split-Path -Parent $dll.FullName
        if ($dllDir -and -not $runtimePathEntries.Contains($dllDir)) { $runtimePathEntries.Add($dllDir) }
    }
    $oldPath = $env:Path
    if ($runtimePathEntries.Count -gt 0) {
        $env:Path = (($runtimePathEntries.ToArray() -join ";") + ";" + $env:Path)
    }
    try {
        $runExe = Invoke-LoggedTestProcess -Stage ($Stage + "-project") `
            -Rel ($rel + " [project:run]") `
            -Exe $exePath `
            -Args @() `
            -SafeStem ($safeStem + "_run") `
            -WorkingDirectory $ProjectDir `
            -Seconds ([Math]::Max($RunTimeoutSec, 120)) `
            -MemoryLimitMB $MemoryLimitMB
    } finally {
        $env:Path = $oldPath
    }
    $okRun = ($runExe.ExitCode -eq 0 -and -not $runExe.TimedOut -and -not $runExe.DialogCaught -and -not $runExe.MemoryExceeded)
    return [pscustomobject]@{ Ok = $okRun; Skipped = $false; ExitCode = $runExe.ExitCode; Run = $runExe }
}

Push-Location $repoRoot
try {
    foreach ($full in $moduleFiles) {
        $rel = Get-RepoRelative $full
        $expectedFailure = Test-ExpectedFailure $full
        $expectedRuntimeFailure = Test-ExpectedRuntimeFailure $full
        $safeStem = Get-SafeOutputStem $rel

        if (-not $BootstrapOnly) {
            Write-RunLine "[host] $rel"
            $run = Invoke-LoggedTestProcess -Stage "host" `
                -Rel $rel `
                -Exe $vyxcHost `
                -Args (@("--src=file", $full, "--run=aot") + $HostCompilerArgs + $runtimeArgs) `
                -SafeStem $safeStem `
                -Seconds $RunTimeoutSec `
                -MemoryLimitMB $MemoryLimitMB
            $exitCode = $run.ExitCode
            $ok = $false
            if ($expectedFailure) {
                if ($exitCode -eq 1) {
                    $ok = $true
                    Write-RunLine "  成功: expected failure observed (host exit 1)"
                } else {
                    Write-RunLine "  失败: host expected diagnostic exit 1, got $exitCode"
                }
            } elseif ($expectedRuntimeFailure) {
                if ($exitCode -ne 0 -and -not $run.TimedOut -and -not $run.DialogCaught -and -not $run.MemoryExceeded) {
                    $ok = $true
                    Write-RunLine "  成功: expected runtime failure observed (host exit $exitCode)"
                } else {
                    Write-RunLine "  失败: host expected runtime failure, got exit $exitCode timeout=$($run.TimedOut) dialog=$($run.DialogCaught) memory=$($run.MemoryExceeded) peak=$($run.PeakWorkingSetMB)MB"
                }
            } elseif ($exitCode -ne 0) {
                Write-RunLine "  失败: host exit $exitCode timeout=$($run.TimedOut) dialog=$($run.DialogCaught) memory=$($run.MemoryExceeded) peak=$($run.PeakWorkingSetMB)MB"
            } else {
                $ok = $true
                Write-RunLine "  成功"
            }
            if ($ok) {
                $passedRuns++
            } else {
                $failed++
                $failedFiles.Add("[host] " + $rel)
            }
            $doneRuns++
            Write-ProgressSummary -Done $doneRuns -Total $totalRuns -Passed $passedRuns -Skipped $skippedRuns -FailedFiles $failedFiles
        }

        if (-not $HostOnly -and $vyxcBoot) {
            Write-RunLine "[bootstrap] $rel"
            if ($BootstrapCompileOnly) {
                $outLl = Join-Path $bootstrapOutDir ((Get-SafeOutputStem $rel) + ".ll")
                if (Test-Path $outLl) { [System.IO.File]::Delete($outLl) }
                $run = Invoke-LoggedTestProcess -Stage "bootstrap" `
                    -Rel $rel `
                    -Exe $vyxcBoot `
                    -Args (@("--src=file", $full, "--emit=ir") + $BootstrapCompilerArgs + @("-o", $outLl) + $runtimeArgs) `
                    -SafeStem $safeStem `
                    -Seconds $RunTimeoutSec `
                    -MemoryLimitMB $MemoryLimitMB
            } else {
                $outExe = Join-Path $bootstrapRunDir ($safeStem + ".exe")
                if (Test-Path $outExe) { [System.IO.File]::Delete($outExe) }
                if (Test-Path ($outExe + ".tmp.obj")) { [System.IO.File]::Delete(($outExe + ".tmp.obj")) }
                $run = Invoke-LoggedTestProcess -Stage "bootstrap" `
                    -Rel $rel `
                    -Exe $vyxcBoot `
                    -Args (@("--src=file", $full, "--run=aot") + $BootstrapCompilerArgs + @("-o", $outExe) + $runtimeArgs) `
                    -SafeStem $safeStem `
                    -Seconds $RunTimeoutSec `
                    -MemoryLimitMB $MemoryLimitMB
            }
            $exitCode = $run.ExitCode
            if ($run.TimedOut) {
                Write-RunLine "  bootstrap timeout after ${RunTimeoutSec}s"
            } elseif ($run.DialogCaught) {
                Write-RunLine "  bootstrap crash dialog captured"
            } elseif ($run.MemoryExceeded) {
                Write-RunLine "  bootstrap memory limit exceeded at $($run.PeakWorkingSetMB)MB"
            }
            $ok = $false
            if ($expectedFailure) {
                if ($exitCode -eq 1) {
                    $ok = $true
                    Write-RunLine "  成功: expected failure observed (bootstrap exit 1)"
                } else {
                    Write-RunLine "  失败: bootstrap expected diagnostic exit 1, got $exitCode"
                }
            } elseif ($expectedRuntimeFailure -and -not $BootstrapCompileOnly) {
                if ($exitCode -ne 0 -and -not $run.TimedOut -and -not $run.DialogCaught -and -not $run.MemoryExceeded) {
                    $ok = $true
                    Write-RunLine "  成功: expected runtime failure observed (bootstrap exit $exitCode)"
                } else {
                    Write-RunLine "  失败: bootstrap expected runtime failure, got exit $exitCode timeout=$($run.TimedOut) dialog=$($run.DialogCaught) memory=$($run.MemoryExceeded) peak=$($run.PeakWorkingSetMB)MB"
                }
            } elseif ($exitCode -ne 0) {
                Write-RunLine "  失败: bootstrap exit $exitCode timeout=$($run.TimedOut) dialog=$($run.DialogCaught) memory=$($run.MemoryExceeded) peak=$($run.PeakWorkingSetMB)MB"
            } else {
                $ok = $true
                Write-RunLine "  成功"
            }
            if ($ok) {
                $passedRuns++
            } else {
                $failed++
                $failedFiles.Add("[bootstrap] " + $rel)
            }
            $doneRuns++
            Write-ProgressSummary -Done $doneRuns -Total $totalRuns -Passed $passedRuns -Skipped $skippedRuns -FailedFiles $failedFiles
        }
    }

    foreach ($projectDir in $projectDirs) {
        $rel = Get-RepoRelative $projectDir
        if (-not $BootstrapOnly) {
            Write-RunLine "[host-project] $rel"
            $result = Invoke-ProjectCase -Stage "host" -ProjectDir $projectDir -CompilerExe $vyxcHost -CompilerArgs $HostCompilerArgs
            if ($result.Skipped) {
                $skippedRuns++
                Write-RunLine "  跳过: 当前平台不适用"
            } elseif ($result.Ok) {
                $passedRuns++
                Write-RunLine "  成功"
            } else {
                $failed++
                $failedFiles.Add("[host-project] " + $rel)
                Write-RunLine "  失败: host project exit $($result.ExitCode) timeout=$($result.Run.TimedOut) dialog=$($result.Run.DialogCaught) memory=$($result.Run.MemoryExceeded) peak=$($result.Run.PeakWorkingSetMB)MB"
            }
            $doneRuns++
            Write-ProgressSummary -Done $doneRuns -Total $totalRuns -Passed $passedRuns -Skipped $skippedRuns -FailedFiles $failedFiles
        }

        if (-not $HostOnly -and $vyxcBoot) {
            Write-RunLine "[bootstrap-project] $rel"
            $result = Invoke-ProjectCase -Stage "bootstrap" -ProjectDir $projectDir -CompilerExe $vyxcBoot -CompilerArgs $BootstrapCompilerArgs
            if ($result.Skipped) {
                $skippedRuns++
                Write-RunLine "  跳过: 当前平台不适用"
            } elseif ($result.Ok) {
                $passedRuns++
                Write-RunLine "  成功"
            } else {
                $failed++
                $failedFiles.Add("[bootstrap-project] " + $rel)
                Write-RunLine "  失败: bootstrap project exit $($result.ExitCode) timeout=$($result.Run.TimedOut) dialog=$($result.Run.DialogCaught) memory=$($result.Run.MemoryExceeded) peak=$($result.Run.PeakWorkingSetMB)MB"
            }
            $doneRuns++
            Write-ProgressSummary -Done $doneRuns -Total $totalRuns -Passed $passedRuns -Skipped $skippedRuns -FailedFiles $failedFiles
        }
    }
} finally {
    Pop-Location
}

Write-RunLine ""
if ($failed -gt 0) {
    Write-RunLine "run_all_modules: $failed run(s) failed."
    Write-ProgressSummary -Done $doneRuns -Total $totalRuns -Passed $passedRuns -Skipped $skippedRuns -FailedFiles $failedFiles
    Write-RunLine ("全部失败文件: " + ($failedFiles.ToArray() -join ", "))
    Write-RunLine ("完整日志: " + $LogPath)
    exit 1
}
Write-RunLine "run_all_modules: $passedRuns passed, $skippedRuns skipped ($doneRuns total)."
Write-RunLine ("完整日志: " + $LogPath)

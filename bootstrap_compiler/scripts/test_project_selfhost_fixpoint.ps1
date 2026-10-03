param(
    [string]$SeedCompiler = "",
    [ValidateRange(1, 3600)][int]$TimeoutSec = 1800,
    [ValidateRange(3, 12)][int]$Generations = 3,
    [ValidateRange(1, 256)][int]$Jobs = 10,
    [string]$Target = "",
    [string]$WorkRoot = "",
    [string]$LlvmRoot = "",
    [switch]$CompareOnly,
    [switch]$HelloGate,
    [string]$HelloSource = "",
    [string]$InstallSeed = ""
)

$ErrorActionPreference = "Stop"

$bootstrapRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $bootstrapRoot
$helper = Join-Path $PSScriptRoot "VyxTestProcess.ps1"
. $helper

$jobs = $Jobs
$isWindowsPlatform = $false
if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    $isWindowsPlatform = [bool]$IsWindows
} else {
    $isWindowsPlatform = ($env:OS -eq "Windows_NT")
}
$compilerFileName = $(if ($isWindowsPlatform) { "boot.exe" } else { "boot" })

function Get-AbsolutePath {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$BasePath
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Get-RelativeChildPath {
    param(
        [Parameter(Mandatory=$true)][string]$RootPath,
        [Parameter(Mandatory=$true)][string]$ChildPath
    )

    $rootFull = [System.IO.Path]::GetFullPath($RootPath).TrimEnd([char[]]@('\', '/'))
    $childFull = [System.IO.Path]::GetFullPath($ChildPath)
    $prefix = $rootFull + [System.IO.Path]::DirectorySeparatorChar
    if (-not $childFull.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "path is outside root: root=$rootFull child=$childFull"
    }
    return $childFull.Substring($prefix.Length)
}

function Test-ExcludedSnapshotPath {
    param([Parameter(Mandatory=$true)][string]$RelativePath)

    foreach ($part in ($RelativePath -split '[\\/]')) {
        if ($part -match '^(?i:\.?cache|target|out)$') {
            return $true
        }
    }
    return $false
}

function Copy-FilteredTree {
    param(
        [Parameter(Mandatory=$true)][string]$Source,
        [Parameter(Mandatory=$true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "required source directory is missing: $Source"
    }
    if (Test-Path -LiteralPath $Destination) {
        throw "copy destination already exists: $Destination"
    }

    [void][System.IO.Directory]::CreateDirectory($Destination)
    $sourceFull = (Get-Item -LiteralPath $Source).FullName
    foreach ($file in @(Get-ChildItem -LiteralPath $sourceFull -File -Force -Recurse)) {
        $relative = Get-RelativeChildPath -RootPath $sourceFull -ChildPath $file.FullName
        if (Test-ExcludedSnapshotPath -RelativePath $relative) {
            continue
        }
        $target = Join-Path $Destination $relative
        $targetParent = Split-Path -Parent $target
        [void][System.IO.Directory]::CreateDirectory($targetParent)
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
}

function Copy-RequiredFile {
    param(
        [Parameter(Mandatory=$true)][string]$Source,
        [Parameter(Mandatory=$true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "required source file is missing: $Source"
    }
    if (Test-Path -LiteralPath $Destination) {
        throw "copy destination already exists: $Destination"
    }
    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $Destination))
    Copy-Item -LiteralPath $Source -Destination $Destination
}

function New-ProjectSnapshot {
    param([Parameter(Mandatory=$true)][string]$DestinationRoot)

    if (Test-Path -LiteralPath $DestinationRoot) {
        throw "snapshot destination already exists: $DestinationRoot"
    }
    [void][System.IO.Directory]::CreateDirectory($DestinationRoot)

    $snapshotBootstrap = Join-Path $DestinationRoot "bootstrap_compiler"
    Copy-FilteredTree -Source (Join-Path $bootstrapRoot "src") -Destination (Join-Path $snapshotBootstrap "src")
    Copy-FilteredTree -Source (Join-Path $bootstrapRoot "std") -Destination (Join-Path $snapshotBootstrap "std")
    # std_packages is part of the canonical bootstrap project.  Omitting it
    # silently turns a cold stage into a different project and breaks targets
    # that consume the json/vendor or package-local source closure.
    Copy-FilteredTree -Source (Join-Path $bootstrapRoot "std_packages") -Destination (Join-Path $snapshotBootstrap "std_packages")
    Copy-RequiredFile -Source (Join-Path $bootstrapRoot "Vyx.toml") -Destination (Join-Path $snapshotBootstrap "Vyx.toml")

    # The runtime sources include generated/companion .inc files (for example
    # vyx_pointer_handle_rt.inc).  Copy the complete source/include trees,
    # while Copy-FilteredTree still strips caches and build outputs.
    Copy-FilteredTree `
        -Source (Join-Path $repoRoot "vyx_codegen/src") `
        -Destination (Join-Path $DestinationRoot "vyx_codegen/src")
    Copy-FilteredTree `
        -Source (Join-Path $repoRoot "vyx_codegen/include") `
        -Destination (Join-Path $DestinationRoot "vyx_codegen/include")
    Copy-FilteredTree `
        -Source (Join-Path $repoRoot "runtime") `
        -Destination (Join-Path $DestinationRoot "runtime")
    Copy-FilteredTree `
        -Source (Join-Path $repoRoot "third_party/cjson") `
        -Destination (Join-Path $DestinationRoot "third_party/cjson")
}

function Assert-ColdStage {
    param([Parameter(Mandatory=$true)][string]$StageRoot)

    $forbidden = @(Get-ChildItem -LiteralPath $StageRoot -Directory -Force -Recurse | Where-Object {
        $_.Name -match '^(?i:\.?cache|target|out)$'
    })
    if ($forbidden.Count -gt 0) {
        throw "stage is not cold; forbidden directory found: $($forbidden[0].FullName)"
    }
}

function Get-OptionalProperty {
    param(
        [Parameter(Mandatory=$true)]$Object,
        [Parameter(Mandatory=$true)][string]$Name,
        $DefaultValue
    )

    if ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]) {
        return $Object.$Name
    }
    return $DefaultValue
}

function Format-LoggedArgument {
    param([Parameter(Mandatory=$true)][string]$Value)

    if ($Value -notmatch "[\s']") {
        return $Value
    }
    return "'" + $Value.Replace("'", "''") + "'"
}

function Get-CompilerFingerprint {
    param([Parameter(Mandatory=$true)][string]$Path)

    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $item.FullName
        length = [int64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
    }
}

function Get-StageCompilerPath {
    param([Parameter(Mandatory=$true)][string]$ProjectDirectory)
    return Join-Path (Join-Path $ProjectDirectory "out") $compilerFileName
}

function Assert-HelloGate {
    param(
        [Parameter(Mandatory=$true)][string]$Compiler,
        [Parameter(Mandatory=$true)][string]$Source
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "hello gate source is missing: $Source"
    }
    $compilerFull = (Get-Item -LiteralPath $Compiler).FullName
    $compilerDirectory = Split-Path -Parent $compilerFull
    $sourceFull = (Get-Item -LiteralPath $Source).FullName
    # A --src=file directory is an implicit module registry root. The canonical
    # hello has no local imports; give it its own directory so Gate A does not
    # recursively index every unrelated fixture below probes/gates.
    $canonicalHello = [IO.Path]::GetFullPath((Join-Path $repoRoot 'probes/gates/hello_gate.vyx'))
    if ($sourceFull -eq $canonicalHello) {
        $helloDirectory = Join-Path $runRoot 'hello-gate'
        [void][IO.Directory]::CreateDirectory($helloDirectory)
        $isolatedHello = Join-Path $helloDirectory 'hello_gate.vyx'
        Copy-Item -LiteralPath $sourceFull -Destination $isolatedHello -Force
        $sourceFull = $isolatedHello
        Write-Host "[hello] isolated copy of $canonicalHello -> $sourceFull"
    }
    $stdoutLog = Join-Path $logRoot "hello.dump-mir2.stdout.log"
    $stderrLog = Join-Path $logRoot "hello.dump-mir2.stderr.log"
    $dialogLog = Join-Path $logRoot "hello.dump-mir2.dialog.log"
    $originalPath = $env:PATH
    $originalLdLibraryPath = $env:LD_LIBRARY_PATH
    $llvmBin = Join-Path $llvmRootPath "bin"
    if ($isWindowsPlatform) {
        $env:PATH = $compilerDirectory + [System.IO.Path]::PathSeparator + $llvmBin + [System.IO.Path]::PathSeparator + $originalPath
    } else {
        $env:LD_LIBRARY_PATH = if ([string]::IsNullOrWhiteSpace($originalLdLibraryPath)) {
            $compilerDirectory
        } else {
            $compilerDirectory + [System.IO.Path]::PathSeparator + $originalLdLibraryPath
        }
    }
    try {
        $run = Invoke-VyxProcess `
            -FilePath $compilerFull `
            -ArgumentList @("--src=file", $sourceFull, "--dump-mir2") `
            -WorkingDirectory $repoRoot `
            -StdoutLog $stdoutLog `
            -StderrLog $stderrLog `
            -DialogLog $dialogLog `
            -TimeoutSec $TimeoutSec
    } finally {
        $env:PATH = $originalPath
        if (-not $isWindowsPlatform) {
            $env:LD_LIBRARY_PATH = $originalLdLibraryPath
        }
    }
    $exitCode = [int](Get-OptionalProperty -Object $run -Name "ExitCode" -DefaultValue -903)
    if ($exitCode -ne 0) {
        throw "hello gate compile failed: exit=$exitCode stdout=$stdoutLog stderr=$stderrLog"
    }
    $dump = Get-Content -LiteralPath $stdoutLog -Raw
    if ($dump -notmatch 'mir2\.unit functions=1 blocks=1 locals=1 places=1 values=6 instrs=2') {
        throw "hello gate MIR baseline drifted; see $stdoutLog"
    }
    Write-Host "[hello] PASS mir2.unit values=6 instrs=2 compiler=$compilerFull"
}

function Install-NamedSeed {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$StageOut,
        [Parameter(Mandatory=$true)][object]$Fingerprints
    )

    if ($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        throw "InstallSeed name is not a safe directory name: $Name"
    }
    $dest = Join-Path $bootstrapRoot $Name
    if (Test-Path -LiteralPath $dest) {
        Remove-Item -LiteralPath $dest -Recurse -Force
    }
    [void][System.IO.Directory]::CreateDirectory($dest)

    $buildTarget = $Target
    $copied = New-Object System.Collections.Generic.List[object]
    $names = @(
        $compilerFileName,
        $(if ($isWindowsPlatform) { "vyxc.exe" } else { "vyxc" }),
        $(if ($isWindowsPlatform) { "vyx_compiler_backend.dll" } else { "vyx_compiler_backend.so" }),
        "vyx_compiler_backend.lib",
        "libvyx_compiler_backend.so",
        "vyx_runtime.lib",
        "libvyx_runtime.a",
        "vyx_runtime.vyi"
    )
    $seen = @{}
    foreach ($fileName in $names) {
        if ($seen.ContainsKey($fileName)) { continue }
        $seen[$fileName] = $true
        $src = Join-Path $StageOut $fileName
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }
        $destFile = Join-Path $dest $fileName
        Copy-Item -LiteralPath $src -Destination $destFile -Force
        [void]$copied.Add((Get-CompilerFingerprint -Path $destFile))
    }
    $bootSrc = Join-Path $StageOut $compilerFileName
    if (-not (Test-Path -LiteralPath $bootSrc -PathType Leaf)) {
        throw "last generation did not produce $compilerFileName"
    }
    $aliasName = $(if ($isWindowsPlatform) { "vyxc.exe" } else { "vyxc" })
    $aliasDest = Join-Path $dest $aliasName
    if (-not (Test-Path -LiteralPath $aliasDest -PathType Leaf)) {
        Copy-Item -LiteralPath $bootSrc -Destination $aliasDest -Force
        [void]$copied.Add((Get-CompilerFingerprint -Path $aliasDest))
    }

    $bootFp = Get-CompilerFingerprint -Path (Join-Path $dest $compilerFileName)
    $sidecar = @(
        "$Name/$compilerFileName  sha256=$($bootFp.sha256)",
        "bytes=$($bootFp.length)",
        "role=multi-gen selfhost fixpoint seed",
        "generations=$Generations",
        "target=$(if ([string]::IsNullOrWhiteSpace($buildTarget)) { '<all>' } else { $buildTarget })",
        "seed_compiler=$seedPath",
        "work_root=$runRoot",
        "validated=S2..S$Generations out+cache byte identity; optional hello dump-mir2 values=6 instrs=2"
    )
    $sidecarText = ($sidecar -join [Environment]::NewLine) + [Environment]::NewLine
    Set-Content -LiteralPath (Join-Path $dest "SHA256.txt") -Value $sidecarText -Encoding ascii
    Set-Content -LiteralPath (Join-Path $bootstrapRoot ($Name + ".sha256.txt")) -Value $sidecarText -Encoding ascii
    Save-Json -Value ([ordered]@{
        name = $Name
        destination = $dest
        files = $copied.ToArray()
        fingerprints = $Fingerprints
    }) -Path (Join-Path $dest "install.manifest.json")
    Write-Host "[seed] installed $Name sha256=$($bootFp.sha256) bytes=$($bootFp.length) dest=$dest"
}

function Save-Json {
    param(
        [Parameter(Mandatory=$true)]$Value,
        [Parameter(Mandatory=$true)][string]$Path
    )

    ConvertTo-Json -InputObject $Value -Depth 8 | Set-Content -LiteralPath $Path -Encoding utf8
}

$stageRecords = New-Object System.Collections.Generic.List[object]
$compilerFingerprints = New-Object System.Collections.Generic.List[object]
$comparisonRecord = $null
$gateStatus = "initializing"
$gateError = ""
$runRoot = ""
$logRoot = ""
$seedPath = ""
$llvmRootPath = ""

function Save-GateSummary {
    if ([string]::IsNullOrWhiteSpace($logRoot) -or -not (Test-Path -LiteralPath $logRoot)) {
        return
    }
    $summary = [ordered]@{
        status = $gateStatus
        error = $gateError
        work_root = $runRoot
        repository = $repoRoot
        seed_compiler = $seedPath
        llvm_root = $llvmRootPath
        jobs = $jobs
        nondeterministic_metadata = @('*.vyx-provenance (stage source root)', '*.vyx-cost (compiler path/mtime and measured memory)')
        build_target = $Target
        stage_timeout_seconds = $TimeoutSec
        generations = $Generations
        # Deliberately no Job Object memory cap: peak memory is evidence, not
        # a policy limit for a self-host stability gate.
        windows_job_memory_limit_mb = 0
        stages = $stageRecords.ToArray()
        compilers = $compilerFingerprints.ToArray()
        comparison = $comparisonRecord
        install_seed = $InstallSeed
        hello_gate = [bool]$HelloGate
    }
    Save-Json -Value $summary -Path (Join-Path $logRoot "gate.summary.json")
}

function Invoke-BuildStage {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Compiler,
        [Parameter(Mandatory=$true)][string]$ProjectDirectory
    )

    if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
        throw "$Name compiler is missing: $Compiler"
    }
    $compilerFull = (Get-Item -LiteralPath $Compiler).FullName
    $projectFull = (Get-Item -LiteralPath $ProjectDirectory).FullName
    $arguments = if ([string]::IsNullOrWhiteSpace($Target)) {
        [string[]]@("build", "-j$jobs")
    } else {
        [string[]]@("build", "--target", $Target, "-j$jobs")
    }
    $command = ((@($compilerFull) + $arguments | ForEach-Object { Format-LoggedArgument -Value $_ }) -join " ")
    $safeName = $Name.ToLowerInvariant()
    $stdoutLog = Join-Path $logRoot ($safeName + ".stdout.log")
    $stderrLog = Join-Path $logRoot ($safeName + ".stderr.log")
    $dialogLog = Join-Path $logRoot ($safeName + ".dialog.log")

    Write-Host "[$Name] cwd=$projectFull"
    Write-Host "[$Name] compiler=$compilerFull"
    Write-Host "[$Name] command=$command"

    $invokeArgs = @{
        FilePath = $compilerFull
        ArgumentList = $arguments
        WorkingDirectory = $projectFull
        StdoutLog = $stdoutLog
        StderrLog = $stderrLog
        DialogLog = $dialogLog
        TimeoutSec = $TimeoutSec
    }
    $originalLdLibraryPath = $env:LD_LIBRARY_PATH
    $originalPath = $env:PATH
    $compilerDirectory = Split-Path -Parent $compilerFull
    if (-not $isWindowsPlatform) {
        $env:LD_LIBRARY_PATH = if ([string]::IsNullOrWhiteSpace($originalLdLibraryPath)) {
            $compilerDirectory
        } else {
            $compilerDirectory + [System.IO.Path]::PathSeparator + $originalLdLibraryPath
        }
    } else {
        $llvmBin = Join-Path $llvmRootPath "bin"
        $env:PATH = $compilerDirectory + [System.IO.Path]::PathSeparator + $llvmBin + [System.IO.Path]::PathSeparator + $originalPath
    }
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $run = Invoke-VyxProcess @invokeArgs
    } finally {
        $timer.Stop()
        if (-not $isWindowsPlatform) {
            $env:LD_LIBRARY_PATH = $originalLdLibraryPath
        } else {
            $env:PATH = $originalPath
        }
    }

    $exitCode = [int](Get-OptionalProperty -Object $run -Name "ExitCode" -DefaultValue -903)
    $peakMB = [double](Get-OptionalProperty -Object $run -Name "PeakWorkingSetMB" -DefaultValue 0.0)
    $jobLimited = [bool](Get-OptionalProperty -Object $run -Name "JobLimited" -DefaultValue $false)
    $record = [pscustomobject][ordered]@{
        stage = $Name
        working_directory = $projectFull
        command = $command
        arguments = $arguments
        compiler = Get-CompilerFingerprint -Path $compilerFull
        exit_code = $exitCode
        timed_out = [bool](Get-OptionalProperty -Object $run -Name "TimedOut" -DefaultValue $false)
        memory_exceeded = [bool](Get-OptionalProperty -Object $run -Name "MemoryExceeded" -DefaultValue $false)
        windows_job_limited = $jobLimited
        peak_working_set_mb = $peakMB
        elapsed_seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
        stdout_log = $stdoutLog
        stderr_log = $stderrLog
        dialog_log = $dialogLog
    }
    [void]$stageRecords.Add($record)
    Save-Json -Value $record -Path (Join-Path $logRoot ($safeName + ".result.json"))
    Save-GateSummary

    Write-Host "[$Name] exit=$exitCode peak_mb=$peakMB elapsed_s=$($record.elapsed_seconds) job_limited=$jobLimited"
    if ($exitCode -ne 0) {
        throw "$Name build failed: exit=$exitCode stdout=$stdoutLog stderr=$stderrLog dialog=$dialogLog"
    }
    return $record
}

function Test-ClangTimestampArtifact {
    param([Parameter(Mandatory=$true)][string]$RelativePath)

    $name = [System.IO.Path]::GetFileName($RelativePath)
    if ($name -eq "vyx_runtime.lib" -or $name -eq "libvyx_runtime.a") {
        return $true
    }
    # clang COFF objects embed TimeDateStamp unless compiled with
    # -mno-incremental-linker-compatible. These C units are not the
    # self-host compiler; boot.exe / backend already match without them.
    if ($name -match '(?i)_c_c\.obj$' -or $name -match '(?i)_c\.o$') {
        return $true
    }
    return $false
}

function Get-OutManifest {
    param([Parameter(Mandatory=$true)][string]$OutDirectory)

    if (-not (Test-Path -LiteralPath $OutDirectory -PathType Container)) {
        throw "output directory is missing: $OutDirectory"
    }
    $outFull = (Get-Item -LiteralPath $OutDirectory).FullName
    $entries = New-Object System.Collections.Generic.List[object]
    foreach ($file in @(Get-ChildItem -LiteralPath $outFull -File -Force -Recurse)) {
        # Provenance files intentionally record the stage-local absolute
        # source root. They are diagnostic metadata, not compiler outputs;
        # exclude them from S2/S3 artifact comparison.
        if ($file.Name.EndsWith(".vyx-provenance", [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        # Action-cost sidecars contain the stage compiler's absolute path and
        # mtime, plus measured resident/commit bytes. Preserve them in logs/out,
        # but do not treat this scheduling telemetry as deterministic code.
        if ($file.Name.EndsWith(".vyx-cost", [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $relative = (Get-RelativeChildPath -RootPath $outFull -ChildPath $file.FullName).Replace('\', '/')
        if (Test-ClangTimestampArtifact -RelativePath $relative) {
            continue
        }
        [void]$entries.Add([pscustomobject][ordered]@{
            path = $relative
            length = [int64]$file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        })
    }
    if ($entries.Count -eq 0) {
        throw "output directory contains no files: $outFull"
    }
    return @($entries | Sort-Object -Property path)
}

function Get-DeterministicCacheManifest {
    param([Parameter(Mandatory=$true)][string]$CacheDirectory)

    if (-not (Test-Path -LiteralPath $CacheDirectory -PathType Container)) {
        throw "cache directory is missing: $CacheDirectory"
    }
    $cacheFull = (Get-Item -LiteralPath $CacheDirectory -Force).FullName
    $entries = New-Object System.Collections.Generic.List[object]
    foreach ($file in @(Get-ChildItem -LiteralPath $cacheFull -File -Force -Recurse)) {
        $isObject = $file.Extension -in @(".obj", ".o")
        $isRootManifest = $file.Name.StartsWith("roots_", [System.StringComparison]::Ordinal) `
            -and $file.Extension -eq ".txt"
        $isExportArtifact = $file.Extension -in @(".exports", ".def")
        if (-not ($isObject -or $isRootManifest -or $isExportArtifact)) {
            continue
        }
        $relative = (Get-RelativeChildPath -RootPath $cacheFull -ChildPath $file.FullName).Replace('\', '/')
        if (Test-ClangTimestampArtifact -RelativePath $relative) {
            continue
        }
        $length = [int64]$file.Length
        $sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        if ($file.Extension -eq ".exports") {
            # The first line is an invalidation key containing stage-local
            # absolute paths and mtimes. The cached EXPORTS body is the
            # deterministic compiler artifact that must reach a fixpoint.
            $text = [System.IO.File]::ReadAllText($file.FullName)
            $newline = $text.IndexOf("`n", [System.StringComparison]::Ordinal)
            $body = $(if ($newline -ge 0) { $text.Substring($newline + 1) } else { "" })
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
            $hasher = [System.Security.Cryptography.SHA256]::Create()
            try {
                $digest = $hasher.ComputeHash($bytes)
                # Windows PowerShell 5.1 has no Convert.ToHexString().  Keep
                # the gate usable from both pwsh and the built-in shell.
                $sha256 = [BitConverter]::ToString($digest).Replace("-", "")
            } finally {
                $hasher.Dispose()
            }
            $length = [int64]$bytes.Length
        }
        [void]$entries.Add([pscustomobject][ordered]@{
            path = $relative
            length = $length
            sha256 = $sha256
        })
    }
    if ($entries.Count -eq 0) {
        throw "cache contains no deterministic build artifacts: $cacheFull"
    }
    return @($entries | Sort-Object -Property path)
}

function Compare-OutManifests {
    param(
        [Parameter(Mandatory=$true)][object[]]$S2Manifest,
        [Parameter(Mandatory=$true)][object[]]$S3Manifest
    )

    $s2ByPath = [System.Collections.Hashtable]::new([System.StringComparer]::Ordinal)
    $s3ByPath = [System.Collections.Hashtable]::new([System.StringComparer]::Ordinal)
    foreach ($entry in $S2Manifest) { $s2ByPath.Add([string]$entry.path, $entry) }
    foreach ($entry in $S3Manifest) { $s3ByPath.Add([string]$entry.path, $entry) }

    $missing = New-Object System.Collections.Generic.List[string]
    $extra = New-Object System.Collections.Generic.List[string]
    $different = New-Object System.Collections.Generic.List[object]
    foreach ($path in $s2ByPath.Keys) {
        if (-not $s3ByPath.ContainsKey($path)) {
            [void]$missing.Add($path)
            continue
        }
        $left = $s2ByPath[$path]
        $right = $s3ByPath[$path]
        if ([int64]$left.length -ne [int64]$right.length -or [string]$left.sha256 -cne [string]$right.sha256) {
            [void]$different.Add([pscustomobject][ordered]@{
                path = $path
                s2_length = [int64]$left.length
                s3_length = [int64]$right.length
                s2_sha256 = [string]$left.sha256
                s3_sha256 = [string]$right.sha256
            })
        }
    }
    foreach ($path in $s3ByPath.Keys) {
        if (-not $s2ByPath.ContainsKey($path)) {
            [void]$extra.Add($path)
        }
    }

    return [pscustomobject][ordered]@{
        equal = ($missing.Count -eq 0 -and $extra.Count -eq 0 -and $different.Count -eq 0)
        s2_file_count = $S2Manifest.Count
        s3_file_count = $S3Manifest.Count
        missing_from_s3 = @($missing | Sort-Object)
        extra_in_s3 = @($extra | Sort-Object)
        changed = @($different | Sort-Object -Property path)
    }
}

function Compare-StageOutputs {
    param(
        [Parameter(Mandatory=$true)][string]$LeftProject,
        [Parameter(Mandatory=$true)][string]$RightProject,
        [Parameter(Mandatory=$true)][string]$LogDirectory,
        [string]$LeftName = "s2",
        [string]$RightName = "s3"
    )

    $leftManifest = @(Get-OutManifest -OutDirectory (Join-Path $LeftProject "out"))
    $rightManifest = @(Get-OutManifest -OutDirectory (Join-Path $RightProject "out"))
    $leftCacheManifest = @(Get-DeterministicCacheManifest -CacheDirectory (Join-Path $LeftProject ".cache"))
    $rightCacheManifest = @(Get-DeterministicCacheManifest -CacheDirectory (Join-Path $RightProject ".cache"))
    Save-Json -Value $leftManifest -Path (Join-Path $LogDirectory ($LeftName + ".out.manifest.json"))
    Save-Json -Value $rightManifest -Path (Join-Path $LogDirectory ($RightName + ".out.manifest.json"))
    Save-Json -Value $leftCacheManifest -Path (Join-Path $LogDirectory ($LeftName + ".cache.manifest.json"))
    Save-Json -Value $rightCacheManifest -Path (Join-Path $LogDirectory ($RightName + ".cache.manifest.json"))
    $outCmp = Compare-OutManifests -S2Manifest $leftManifest -S3Manifest $rightManifest
    $cacheCmp = Compare-OutManifests -S2Manifest $leftCacheManifest -S3Manifest $rightCacheManifest
    return [pscustomobject][ordered]@{
        left = $LeftName
        right = $RightName
        equal = ($outCmp.equal -and $cacheCmp.equal)
        out = $outCmp
        deterministic_cache = $cacheCmp
    }
}

$originalLlvmRoot = $env:LLVM_ROOT
try {
    $cwd = (Get-Location).Path
    if ([string]::IsNullOrWhiteSpace($SeedCompiler)) {
        $seedPath = Join-Path (Join-Path $bootstrapRoot "seed") "vyxc.exe"
    } else {
        $seedPath = Get-AbsolutePath -Path $SeedCompiler -BasePath $cwd
    }
    if (-not (Test-Path -LiteralPath $seedPath -PathType Leaf)) {
        throw "seed compiler is missing: $seedPath"
    }
    $seedPath = (Get-Item -LiteralPath $seedPath).FullName

    if ([string]::IsNullOrWhiteSpace($LlvmRoot)) {
        $llvmRootPath = Join-Path $repoRoot "clang"
    } else {
        $llvmRootPath = Get-AbsolutePath -Path $LlvmRoot -BasePath $cwd
    }
    if (-not (Test-Path -LiteralPath $llvmRootPath -PathType Container)) {
        throw "LLVM root is missing: $llvmRootPath"
    }
    $llvmRootPath = (Get-Item -LiteralPath $llvmRootPath).FullName
    $env:LLVM_ROOT = $llvmRootPath

    if ([string]::IsNullOrWhiteSpace($WorkRoot)) {
        do {
            $candidate = Join-Path ([System.IO.Path]::GetTempPath()) ("vyx-project-fixpoint-" + [Guid]::NewGuid().ToString("N"))
        } while (Test-Path -LiteralPath $candidate)
        $runRoot = $candidate
    } else {
        $runRoot = Get-AbsolutePath -Path $WorkRoot -BasePath $cwd
        if ((Test-Path -LiteralPath $runRoot) -and -not $CompareOnly) {
            throw "WorkRoot must not already exist: $runRoot"
        }
    }
    [void][System.IO.Directory]::CreateDirectory($runRoot)
    $logRoot = Join-Path $runRoot "logs"
    [void][System.IO.Directory]::CreateDirectory($logRoot)

    if ($CompareOnly) {
        $s2Project = Join-Path $runRoot "s2/bootstrap_compiler"
        $lastName = "s" + $Generations
        $lastProject = Join-Path $runRoot ($lastName + "/bootstrap_compiler")
        if (-not (Test-Path -LiteralPath $s2Project -PathType Container) -or
            -not (Test-Path -LiteralPath $lastProject -PathType Container)) {
            throw "CompareOnly requires existing s2/$lastName bootstrap project stages under $runRoot"
        }
        $gateStatus = "compare"
        Save-GateSummary
        $comparisonRecord = Compare-StageOutputs -LeftProject $s2Project -RightProject $lastProject -LogDirectory $logRoot -LeftName "s2" -RightName $lastName
        Save-Json -Value $comparisonRecord -Path (Join-Path $logRoot "fixpoint.comparison.json")
        if (-not $comparisonRecord.equal) {
            throw "S2/$($lastName.ToUpperInvariant()) mismatch: out_changed=$($comparisonRecord.out.changed.Count) cache_changed=$($comparisonRecord.deterministic_cache.changed.Count)"
        }
        for ($g = 1; $g -le $Generations; $g++) {
            $stageProject = Join-Path $runRoot ("s$g/bootstrap_compiler")
            $compilerPath = Get-StageCompilerPath -ProjectDirectory $stageProject
            if (Test-Path -LiteralPath $compilerPath -PathType Leaf) {
                $fp = Get-CompilerFingerprint -Path $compilerPath
                [void]$compilerFingerprints.Add([pscustomobject][ordered]@{ stage = ("S$g"); compiler = $fp })
                Write-Host "[S$g] compiler sha256=$($fp.sha256) bytes=$($fp.length)"
            }
        }
        Save-Json -Value $compilerFingerprints.ToArray() -Path (Join-Path $logRoot "compilers.json")
        $prevCompiler = Get-StageCompilerPath -ProjectDirectory $lastProject
        if ($HelloGate) {
            $helloSrc = if ([string]::IsNullOrWhiteSpace($HelloSource)) {
                Join-Path $repoRoot "probes\gates\hello_gate.vyx"
            } else {
                Get-AbsolutePath -Path $HelloSource -BasePath $cwd
            }
            Assert-HelloGate -Compiler $prevCompiler -Source $helloSrc
        }
        if (-not [string]::IsNullOrWhiteSpace($InstallSeed)) {
            Install-NamedSeed -Name $InstallSeed -StageOut (Join-Path $lastProject "out") -Fingerprints $compilerFingerprints.ToArray()
        }
        $gateStatus = "passed"
        Save-GateSummary
        Write-Host "[fixpoint] PASS out_files=$($comparisonRecord.out.s2_file_count) cache_files=$($comparisonRecord.deterministic_cache.s2_file_count)"
        Write-Host "[fixpoint] summary=$(Join-Path $logRoot 'gate.summary.json')"
        return
    }

    Write-Host "[fixpoint] work_root=$runRoot"
    Write-Host "[fixpoint] seed=$seedPath"
    Write-Host "[fixpoint] llvm_root=$llvmRootPath"
    Write-Host "[fixpoint] generations=$Generations target=$(if ([string]::IsNullOrWhiteSpace($Target)) { '<all>' } else { $Target }) jobs=$jobs timeout_s=$TimeoutSec windows_job_limit_mb=0"

    $gateStatus = "snapshot"
    Save-GateSummary
    $snapshotRoot = Join-Path $runRoot "snapshot"
    New-ProjectSnapshot -DestinationRoot $snapshotRoot

    $stageProjects = @{}
    for ($g = 1; $g -le $Generations; $g++) {
        $stageName = "s$g"
        $stageRoot = Join-Path $runRoot $stageName
        Copy-FilteredTree -Source $snapshotRoot -Destination $stageRoot
        Assert-ColdStage -StageRoot $stageRoot
        $stageProjects[$g] = Join-Path $stageRoot "bootstrap_compiler"
    }

    $gateStatus = "s1"
    Save-GateSummary
    [void](Invoke-BuildStage -Name "S1" -Compiler $seedPath -ProjectDirectory $stageProjects[1])

    $prevCompiler = Get-StageCompilerPath -ProjectDirectory $stageProjects[1]
    if (-not (Test-Path -LiteralPath $prevCompiler -PathType Leaf)) {
        throw "S1 did not produce its compiler: $prevCompiler"
    }
    $s1Fp = Get-CompilerFingerprint -Path $prevCompiler
    [void]$compilerFingerprints.Add([pscustomobject][ordered]@{ stage = "S1"; compiler = $s1Fp })
    Write-Host "[S1] compiler sha256=$($s1Fp.sha256) bytes=$($s1Fp.length)"

    for ($g = 2; $g -le $Generations; $g++) {
        $gateStatus = "s$g"
        Save-GateSummary
        [void](Invoke-BuildStage -Name ("S$g") -Compiler $prevCompiler -ProjectDirectory $stageProjects[$g])
        $prevCompiler = Get-StageCompilerPath -ProjectDirectory $stageProjects[$g]
        if (-not (Test-Path -LiteralPath $prevCompiler -PathType Leaf)) {
            throw "S$g did not produce its compiler: $prevCompiler"
        }
        $fp = Get-CompilerFingerprint -Path $prevCompiler
        [void]$compilerFingerprints.Add([pscustomobject][ordered]@{ stage = ("S$g"); compiler = $fp })
        Write-Host "[S$g] compiler sha256=$($fp.sha256) bytes=$($fp.length)"
    }

    $gateStatus = "compare"
    Save-GateSummary
    $pairRecords = New-Object System.Collections.Generic.List[object]
    $allEqual = $true
    $failDetail = ""
    for ($g = 3; $g -le $Generations; $g++) {
        $leftName = "s2"
        $rightName = "s$g"
        $pair = Compare-StageOutputs `
            -LeftProject $stageProjects[2] `
            -RightProject $stageProjects[$g] `
            -LogDirectory $logRoot `
            -LeftName $leftName `
            -RightName $rightName
        [void]$pairRecords.Add($pair)
        Write-Host "[fixpoint] $leftName vs $rightName equal=$($pair.equal) out_changed=$($pair.out.changed.Count) cache_changed=$($pair.deterministic_cache.changed.Count)"
        if (-not $pair.equal) {
            $allEqual = $false
            if ([string]::IsNullOrWhiteSpace($failDetail)) {
                $failDetail = "S2/S$g mismatch: out_changed=$($pair.out.changed.Count) cache_changed=$($pair.deterministic_cache.changed.Count)"
            }
        }
    }
    $comparisonRecord = [pscustomobject][ordered]@{
        equal = $allEqual
        generations = $Generations
        pairs = $pairRecords.ToArray()
        out = $pairRecords[$pairRecords.Count - 1].out
        deterministic_cache = $pairRecords[$pairRecords.Count - 1].deterministic_cache
    }
    Save-Json -Value $comparisonRecord -Path (Join-Path $logRoot "fixpoint.comparison.json")
    if (-not $allEqual) {
        throw $failDetail
    }

    Save-Json -Value $compilerFingerprints.ToArray() -Path (Join-Path $logRoot "compilers.json")

    if ($HelloGate) {
        $gateStatus = "hello"
        Save-GateSummary
        $helloSrc = if ([string]::IsNullOrWhiteSpace($HelloSource)) {
            Join-Path $repoRoot "probes\gates\hello_gate.vyx"
        } else {
            Get-AbsolutePath -Path $HelloSource -BasePath ((Get-Location).Path)
        }
        Assert-HelloGate -Compiler $prevCompiler -Source $helloSrc
    }

    if (-not [string]::IsNullOrWhiteSpace($InstallSeed)) {
        $gateStatus = "install-seed"
        Save-GateSummary
        $lastOut = Join-Path $stageProjects[$Generations] "out"
        Install-NamedSeed -Name $InstallSeed -StageOut $lastOut -Fingerprints $compilerFingerprints.ToArray()
    }

    $gateStatus = "passed"
    Save-GateSummary
    Write-Host "[fixpoint] PASS generations=$Generations out_files=$($comparisonRecord.out.s2_file_count) cache_files=$($comparisonRecord.deterministic_cache.s2_file_count)"
    Write-Host "[fixpoint] summary=$(Join-Path $logRoot 'gate.summary.json')"
} catch {
    $gateStatus = "failed"
    $gateError = $_.Exception.Message
    Save-GateSummary
    Write-Host "[fixpoint] FAIL $gateError"
    if (-not [string]::IsNullOrWhiteSpace($runRoot)) {
        Write-Host "[fixpoint] retained_work_root=$runRoot"
    }
    throw
} finally {
    $env:LLVM_ROOT = $originalLlvmRoot
}

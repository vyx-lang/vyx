[CmdletBinding()]
param(
    [string]$SeedCompiler = "",
    [Parameter(Mandatory = $true)][string]$RunRoot,
    [int]$Jobs = 10,
    [int]$TimeoutSec = 600,
    [int]$PerfWarmup = 1,
    [int]$PerfReps = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Jobs -lt 1) { throw "Jobs must be at least 1." }
if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) {
    throw "TimeoutSec must be between 1 and 600."
}
if ($PerfWarmup -lt 0) { throw "PerfWarmup must be at least 0." }
if ($PerfReps -lt 1) { throw "PerfReps must be at least 1." }

$bootstrapRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $bootstrapRoot
$processHelper = Join-Path $PSScriptRoot "VyxTestProcess.ps1"
if (-not (Test-Path -LiteralPath $processHelper -PathType Leaf)) {
    throw "Process helper not found: $processHelper"
}
. $processHelper

if ([string]::IsNullOrWhiteSpace($SeedCompiler)) {
    $SeedCompiler = Join-Path (Join-Path $bootstrapRoot "seed") "vyxc.exe"
}
if (-not (Test-Path -LiteralPath $SeedCompiler -PathType Leaf)) {
    throw "Seed compiler not found: $SeedCompiler"
}
$SeedCompiler = (Resolve-Path -LiteralPath $SeedCompiler).Path

$runRootPath = [System.IO.Path]::GetFullPath($RunRoot)
if (Test-Path -LiteralPath $runRootPath) {
    throw "RunRoot must not already exist: $runRootPath"
}

function Test-PathIsSameOrChild {
    param([Parameter(Mandatory = $true)][string]$Candidate,
          [Parameter(Mandatory = $true)][string]$Parent)
    $candidatePath = [System.IO.Path]::GetFullPath($Candidate).TrimEnd('\', '/')
    $parentPath = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    if ($candidatePath.Equals($parentPath, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    $prefix = $parentPath + [System.IO.Path]::DirectorySeparatorChar
    return $candidatePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Test-RegistryIgnoredPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $separators = [char[]]@([System.IO.Path]::DirectorySeparatorChar,
                            [System.IO.Path]::AltDirectorySeparatorChar)
    foreach ($segment in $fullPath.Split($separators, [StringSplitOptions]::RemoveEmptyEntries)) {
        if ($segment -eq ".git" -or $segment -eq ".cache" -or
            $segment -eq "target" -or $segment -eq "build" -or
            $segment.StartsWith("build_", [StringComparison]::OrdinalIgnoreCase) -or
            $segment.StartsWith("cmake-build", [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

$snapshotSourceTrees = @(
    (Join-Path $bootstrapRoot "src"),
    (Join-Path $bootstrapRoot "std"),
    (Join-Path $repoRoot "vyx_codegen\src"),
    (Join-Path $repoRoot "vyx_codegen\include"),
    (Join-Path $repoRoot "third_party\cjson")
)
foreach ($sourceTree in $snapshotSourceTrees) {
    if (Test-PathIsSameOrChild -Candidate $runRootPath -Parent $sourceTree) {
        throw "RunRoot must not be inside a copied source tree: $runRootPath (source: $sourceTree)"
    }
}
if (Test-RegistryIgnoredPath -Path $runRootPath) {
    throw "RunRoot contains a path segment ignored by the Vyx module registry; use a normal directory outside .cache/target/build trees: $runRootPath"
}

$llvmRoot = Join-Path $repoRoot "clang"
$clangC = Join-Path $llvmRoot "bin\clang.exe"
$clangCxx = Join-Path $llvmRoot "bin\clang++.exe"
foreach ($required in @($llvmRoot, $clangC, $clangCxx)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required LLVM path not found: $required"
    }
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction Stop
$ninjaCommand = Get-Command ninja.exe -ErrorAction Stop
$cmakeExe = $cmakeCommand.Source
$ninjaExe = $ninjaCommand.Source

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$stepRecords = [System.Collections.Generic.List[object]]::new()

function Write-Utf8File {
    param([Parameter(Mandatory = $true)][string]$Path,
          [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Append-Utf8Line {
    param([Parameter(Mandatory = $true)][string]$Path,
          [Parameter(Mandatory = $true)][string]$Text)
    [System.IO.File]::AppendAllText($Path, $Text + [Environment]::NewLine, $utf8NoBom)
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        $bytes = $sha.ComputeHash($stream)
        return [System.BitConverter]::ToString($bytes).Replace("-", "")
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Get-ArtifactFingerprint {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return [pscustomobject]@{ Path = ""; Kind = "none"; Exists = $false; FileCount = 0; Bytes = 0; Sha256 = "" }
    }
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath)) {
        return [pscustomobject]@{ Path = $fullPath; Kind = "missing"; Exists = $false; FileCount = 0; Bytes = 0; Sha256 = "" }
    }
    if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
        $item = Get-Item -LiteralPath $fullPath
        return [pscustomobject]@{
            Path = $fullPath
            Kind = "file"
            Exists = $true
            FileCount = 1
            Bytes = [int64]$item.Length
            Sha256 = Get-FileSha256 -Path $fullPath
        }
    }

    $files = @(Get-ChildItem -LiteralPath $fullPath -File -Recurse | Sort-Object FullName)
    $manifest = [System.Text.StringBuilder]::new()
    [int64]$totalBytes = 0
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($fullPath.Length).TrimStart([char[]]@("\", "/")).Replace("\", "/")
        $fileHash = Get-FileSha256 -Path $file.FullName
        $totalBytes += [int64]$file.Length
        [void]$manifest.Append($relative).Append("`t").Append($file.Length).Append("`t").Append($fileHash).Append("`n")
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $manifestBytes = [System.Text.Encoding]::UTF8.GetBytes($manifest.ToString())
        $digest = [System.BitConverter]::ToString($sha.ComputeHash($manifestBytes)).Replace("-", "")
    } finally {
        $sha.Dispose()
    }
    return [pscustomobject]@{
        Path = $fullPath
        Kind = "directory-tree"
        Exists = $true
        FileCount = $files.Count
        Bytes = $totalBytes
        Sha256 = $digest
    }
}

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Format-CommandArgument {
    param([AllowEmptyString()][string]$Value)
    if ($Value -match '^[A-Za-z0-9_./:\\=+,-]+$') { return $Value }
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Format-CommandLine {
    param([string]$FilePath, [string[]]$ArgumentList)
    $parts = [System.Collections.Generic.List[string]]::new()
    [void]$parts.Add((Format-CommandArgument -Value $FilePath))
    foreach ($argument in $ArgumentList) {
        [void]$parts.Add((Format-CommandArgument -Value $argument))
    }
    return $parts -join " "
}

function Get-OptionalProperty {
    param([object]$Object, [string]$Name, [object]$DefaultValue)
    if ($null -eq $Object) { return $DefaultValue }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $DefaultValue }
    return $property.Value
}

function Invoke-MultigenStep {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [string]$ArtifactPath = "",
        [int]$StepTimeoutSec = $TimeoutSec
    )

    $stdoutLog = Join-Path $logsRoot ($Name + ".stdout.log")
    $stderrLog = Join-Path $logsRoot ($Name + ".stderr.log")
    $dialogLog = Join-Path $logsRoot ($Name + ".dialog.log")
    $commandPath = Join-Path $logsRoot ($Name + ".command.txt")
    $commandLine = Format-CommandLine -FilePath $FilePath -ArgumentList $ArgumentList
    $toolFingerprint = Get-ArtifactFingerprint -Path $FilePath
    Write-Utf8File -Path $commandPath -Text $commandLine

    $invokeError = ""
    $processResult = $null
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $processOutput = @(Invoke-VyxProcess -FilePath $FilePath `
                            -ArgumentList $ArgumentList `
                            -WorkingDirectory $WorkingDirectory `
                            -StdoutLog $stdoutLog `
                            -StderrLog $stderrLog `
                            -DialogLog $dialogLog `
                            -TimeoutSec $StepTimeoutSec)
        if ($processOutput.Count -gt 0) { $processResult = $processOutput[-1] }
    } catch {
        $invokeError = $_.Exception.ToString()
    } finally {
        $watch.Stop()
    }

    $exitCode = [int](Get-OptionalProperty -Object $processResult -Name "ExitCode" -DefaultValue -999)
    if ($invokeError.Length -gt 0) { $exitCode = -999 }
    $artifact = Get-ArtifactFingerprint -Path $ArtifactPath
    $record = [pscustomobject][ordered]@{
        Name = $Name
        Command = $commandLine
        CompilerOrTool = [System.IO.Path]::GetFullPath($FilePath)
        ToolFingerprint = $toolFingerprint
        WorkingDirectory = [System.IO.Path]::GetFullPath($WorkingDirectory)
        TimeoutSec = $StepTimeoutSec
        ExitCode = $exitCode
        ElapsedMs = [Math]::Round($watch.Elapsed.TotalMilliseconds, 3)
        TimedOut = [bool](Get-OptionalProperty -Object $processResult -Name "TimedOut" -DefaultValue $false)
        DialogCaught = [bool](Get-OptionalProperty -Object $processResult -Name "DialogCaught" -DefaultValue $false)
        MemoryExceeded = [bool](Get-OptionalProperty -Object $processResult -Name "MemoryExceeded" -DefaultValue $false)
        PeakWorkingSetMB = [double](Get-OptionalProperty -Object $processResult -Name "PeakWorkingSetMB" -DefaultValue 0.0)
        StdoutLog = $stdoutLog
        StderrLog = $stderrLog
        DialogLog = $dialogLog
        CommandFile = $commandPath
        Artifact = $artifact
        InvokeError = $invokeError
    }
    [void]$stepRecords.Add($record)
    Append-Utf8Line -Path $resultsJsonl -Text ($record | ConvertTo-Json -Depth 6 -Compress)

    Write-Host ("[{0}] exit={1} elapsed_ms={2} artifact_sha256={3}" -f `
                $Name, $exitCode, $record.ElapsedMs, $artifact.Sha256)
    if ($exitCode -ne 0) {
        throw "Step '$Name' failed with exit code $exitCode. Logs and artifacts remain under $runRootPath"
    }
    if (-not [string]::IsNullOrWhiteSpace($ArtifactPath) -and -not $artifact.Exists) {
        throw "Step '$Name' succeeded but its expected artifact is missing: $ArtifactPath"
    }
    return $record
}

function New-MinimalSnapshot {
    param([Parameter(Mandatory = $true)][string]$Destination)
    New-Item -ItemType Directory -Path $Destination | Out-Null
    $snapshotBootstrap = Join-Path $Destination "bootstrap_compiler"
    $snapshotCodegen = Join-Path $Destination "vyx_codegen"
    $snapshotThirdParty = Join-Path $Destination "third_party"
    New-Item -ItemType Directory -Path $snapshotBootstrap | Out-Null
    New-Item -ItemType Directory -Path $snapshotCodegen | Out-Null
    New-Item -ItemType Directory -Path $snapshotThirdParty | Out-Null
    Copy-Item -LiteralPath (Join-Path $bootstrapRoot "Vyx.toml") -Destination $snapshotBootstrap
    Copy-Item -LiteralPath (Join-Path $bootstrapRoot "src") -Destination $snapshotBootstrap -Recurse
    Copy-Item -LiteralPath (Join-Path $bootstrapRoot "std") -Destination $snapshotBootstrap -Recurse
    Copy-Item -LiteralPath (Join-Path $repoRoot "vyx_codegen\src") -Destination $snapshotCodegen -Recurse
    Copy-Item -LiteralPath (Join-Path $repoRoot "vyx_codegen\include") -Destination $snapshotCodegen -Recurse
    Copy-Item -LiteralPath (Join-Path $repoRoot "third_party\cjson") -Destination $snapshotThirdParty -Recurse
}

function Copy-SnapshotToStage {
    param([Parameter(Mandatory = $true)][string]$SnapshotRoot,
          [Parameter(Mandatory = $true)][string]$StageRoot)
    New-Item -ItemType Directory -Path $StageRoot | Out-Null
    foreach ($directory in @("bootstrap_compiler", "vyx_codegen", "third_party")) {
        Copy-Item -LiteralPath (Join-Path $SnapshotRoot $directory) -Destination $StageRoot -Recurse
    }
}

New-Item -ItemType Directory -Path $runRootPath | Out-Null
$logsRoot = Join-Path $runRootPath "logs"
$snapshotRoot = Join-Path $runRootPath "source_snapshot"
$resultsJsonl = Join-Path $runRootPath "steps.jsonl"
New-Item -ItemType Directory -Path $logsRoot | Out-Null
New-MinimalSnapshot -Destination $snapshotRoot

$s1Root = Join-Path $runRootPath "s1"
$s2Root = Join-Path $runRootPath "s2"
$s3Root = Join-Path $runRootPath "s3"
Copy-SnapshotToStage -SnapshotRoot $snapshotRoot -StageRoot $s1Root
Copy-SnapshotToStage -SnapshotRoot $snapshotRoot -StageRoot $s2Root
Copy-SnapshotToStage -SnapshotRoot $snapshotRoot -StageRoot $s3Root

$s1Project = Join-Path $s1Root "bootstrap_compiler"
$s2Project = Join-Path $s2Root "bootstrap_compiler"
$s3Project = Join-Path $s3Root "bootstrap_compiler"
$c1Root = Join-Path $runRootPath "c1_cpp"
$s1Compiler = Join-Path $s1Project "out\boot.exe"
$c1Compiler = Join-Path $c1Root "build\ninja-release\vyx_boot.exe"
$s2Compiler = Join-Path $s2Project "out\boot.exe"
$s3Compiler = Join-Path $s3Project "out\boot.exe"

$setup = [pscustomobject][ordered]@{
    CreatedUtc = [DateTime]::UtcNow.ToString("o")
    RepoRoot = $repoRoot
    RunRoot = $runRootPath
    SeedCompiler = Get-ArtifactFingerprint -Path $SeedCompiler
    LLVM_ROOT = $llvmRoot
    CMake = Get-ArtifactFingerprint -Path $cmakeExe
    Ninja = Get-ArtifactFingerprint -Path $ninjaExe
    Jobs = $Jobs
    TimeoutSec = $TimeoutSec
    PerfWarmup = $PerfWarmup
    PerfReps = $PerfReps
    SourceSnapshot = Get-ArtifactFingerprint -Path $snapshotRoot
}
Write-Utf8File -Path (Join-Path $runRootPath "setup.json") -Text ($setup | ConvertTo-Json -Depth 6)

$oldLlvmRoot = [Environment]::GetEnvironmentVariable("LLVM_ROOT", "Process")
$oldBootstrapProfile = [Environment]::GetEnvironmentVariable("VYX_BOOTSTRAP_PROFILE", "Process")
$failure = $null
try {
    $env:LLVM_ROOT = $llvmRoot

    [void](Invoke-MultigenStep -Name "01_s0_build_s1" -FilePath $SeedCompiler `
        -ArgumentList @("build", "-j$Jobs") -WorkingDirectory $s1Project -ArtifactPath $s1Compiler)

    [void](Invoke-MultigenStep -Name "02_s1_emit_c1_cpp" -FilePath $s1Compiler `
        -ArgumentList @("--src=project", $s1Project, "--emit=cpp", "-j", "$Jobs", "-o", $c1Root) `
        -WorkingDirectory $s1Project -ArtifactPath $c1Root)

    $cmakeCache = Join-Path $c1Root "build\ninja-release\CMakeCache.txt"
    [void](Invoke-MultigenStep -Name "03_cmake_configure_c1" -FilePath $cmakeExe `
        -ArgumentList @("--preset", "ninja-release",
                        "-DCMAKE_C_COMPILER:FILEPATH=$clangC",
                        "-DCMAKE_CXX_COMPILER:FILEPATH=$clangCxx",
                        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninjaExe") `
        -WorkingDirectory $c1Root -ArtifactPath $cmakeCache)

    [void](Invoke-MultigenStep -Name "04_cmake_build_c1" -FilePath $cmakeExe `
        -ArgumentList @("--build", "--preset", "ninja-release", "--parallel", "$Jobs") `
        -WorkingDirectory $c1Root -ArtifactPath $c1Compiler)

    [void](Invoke-MultigenStep -Name "05_c1_build_s2" -FilePath $c1Compiler `
        -ArgumentList @("build", "-j$Jobs") -WorkingDirectory $s2Project -ArtifactPath $s2Compiler)

    [void](Invoke-MultigenStep -Name "06_s2_build_s3" -FilePath $s2Compiler `
        -ArgumentList @("build", "-j$Jobs") -WorkingDirectory $s3Project -ArtifactPath $s3Compiler)

    $compilers = [ordered]@{
        s1 = $s1Compiler
        c1 = $c1Compiler
        s2 = $s2Compiler
        s3 = $s3Compiler
    }
    $helpIndex = 7
    foreach ($entry in $compilers.GetEnumerator()) {
        $stepName = "{0:D2}_{1}_help" -f $helpIndex, $entry.Key
        [void](Invoke-MultigenStep -Name $stepName -FilePath $entry.Value `
            -ArgumentList @("--help") -WorkingDirectory $s1Project `
            -ArtifactPath $entry.Value -StepTimeoutSec ([Math]::Min(60, $TimeoutSec)))
        $helpIndex++
    }

    $rootSource = Join-Path $s1Project "src\main.vyx"
    $interfaceRoot = Join-Path $s1Project "out\.vyx_interfaces\boot"
    $exportRoots = Join-Path $s1Project ".cache\roots_module_bootstrap_export.txt"
    $rootEmitCandidates = @(Get-ChildItem -LiteralPath (Join-Path $s1Project ".cache") `
                            -File -Filter "roots_module_bootstrap_rootchunk_0_*_emit.txt" |
                            Sort-Object Name)
    foreach ($requiredPath in @($rootSource, $interfaceRoot, $exportRoots)) {
        if (-not (Test-Path -LiteralPath $requiredPath)) {
            throw "Fresh S1 build did not produce the rootchunk prerequisite: $requiredPath"
        }
    }
    if ($rootEmitCandidates.Count -eq 0) {
        throw "Fresh S1 build did not produce a rootchunk-0 emit roots file under $s1Project\.cache"
    }
    $mainRootCandidate = @($rootEmitCandidates | Where-Object { $_.Name -eq "roots_module_bootstrap_rootchunk_0_main_emit.txt" })
    if ($mainRootCandidate.Count -gt 0) {
        $emitRoots = $mainRootCandidate[0].FullName
    } else {
        $emitRoots = $rootEmitCandidates[0].FullName
    }

    $irRoot = Join-Path $runRootPath "rootchunk_ir"
    $perfWorkRoot = Join-Path $runRootPath "perf_work"
    New-Item -ItemType Directory -Path $irRoot | Out-Null
    New-Item -ItemType Directory -Path $perfWorkRoot | Out-Null
    $perfProjects = [ordered]@{}
    foreach ($key in @($compilers.Keys)) {
        $perfStageRoot = Join-Path $perfWorkRoot $key
        Copy-SnapshotToStage -SnapshotRoot $snapshotRoot -StageRoot $perfStageRoot
        $perfProjects[$key] = Join-Path $perfStageRoot "bootstrap_compiler"
    }

    $env:VYX_BOOTSTRAP_PROFILE = "1"
    $irResults = [ordered]@{}
    foreach ($entry in $compilers.GetEnumerator()) {
        $irResults[$entry.Key] = [pscustomobject][ordered]@{
            Compiler = $entry.Value
            WorkingDirectory = $perfProjects[$entry.Key]
            WarmupMs = @()
            MeasureMs = @()
            MedianElapsedMs = 0.0
            RatioToS1 = 0.0
            IR = $null
            Hashes = @()
        }
    }

    function Invoke-RootchunkRun {
        param([string]$Key, [string]$Phase, [int]$Iteration)
        $irPath = Join-Path $irRoot ("{0}_{1}_{2:D2}.ll" -f $Key, $Phase, $Iteration)
        $irArguments = @(
            "--src=file", $rootSource,
            "--emit=ir", "-O2",
            "-L", (Join-Path $s1Project "out"), "-l", "vyx_compiler_backend",
            "--interface-root", $interfaceRoot,
            "--export-top-level-roots", "--export-method-roots",
            "--export-root-names-file", $exportRoots,
            "--emit-root-names-file", $emitRoots,
            "-o", $irPath
        )
        $stepName = "ir_{0}_{1}_{2:D2}" -f $Phase, $Key, $Iteration
        return Invoke-MultigenStep -Name $stepName -FilePath $compilers[$Key] `
            -ArgumentList $irArguments -WorkingDirectory $perfProjects[$Key] -ArtifactPath $irPath
    }

    $compilerKeys = @($compilers.Keys)
    for ($warmup = 1; $warmup -le $PerfWarmup; $warmup++) {
        foreach ($key in $compilerKeys) {
            $record = Invoke-RootchunkRun -Key $key -Phase "warmup" -Iteration $warmup
            $irResults[$key].WarmupMs += [double]$record.ElapsedMs
            $irResults[$key].Hashes += $record.Artifact.Sha256
        }
    }

    for ($rep = 1; $rep -le $PerfReps; $rep++) {
        $offset = ($rep - 1) % $compilerKeys.Count
        $order = @()
        for ($slot = 0; $slot -lt $compilerKeys.Count; $slot++) {
            $order += $compilerKeys[($slot + $offset) % $compilerKeys.Count]
        }
        foreach ($key in $order) {
            $record = Invoke-RootchunkRun -Key $key -Phase "measure" -Iteration $rep
            $irResults[$key].MeasureMs += [double]$record.ElapsedMs
            $irResults[$key].Hashes += $record.Artifact.Sha256
            $irResults[$key].IR = $record.Artifact
        }
    }

    foreach ($key in $compilerKeys) {
        $irResults[$key].MedianElapsedMs = [Math]::Round((Get-Median -Values $irResults[$key].MeasureMs), 3)
        $irResults[$key].Hashes = @($irResults[$key].Hashes | Sort-Object -Unique)
    }
    $s1Elapsed = [double]$irResults["s1"].MedianElapsedMs
    foreach ($key in @($irResults.Keys)) {
        if ($s1Elapsed -gt 0.0) {
            $irResults[$key].RatioToS1 = [Math]::Round(([double]$irResults[$key].MedianElapsedMs / $s1Elapsed), 4)
        }
    }
    $c1NotSlowerThanS1 = ([double]$irResults["c1"].MedianElapsedMs -le $s1Elapsed)
    $uniqueIrHashes = @($irResults.Values | ForEach-Object { $_.Hashes } | Sort-Object -Unique)
    $summary = [pscustomobject][ordered]@{
        CompletedUtc = [DateTime]::UtcNow.ToString("o")
        RunRoot = $runRootPath
        Compilers = [pscustomobject]$compilers
        CompilerArtifacts = [pscustomobject][ordered]@{
            s1 = Get-ArtifactFingerprint -Path $s1Compiler
            c1 = Get-ArtifactFingerprint -Path $c1Compiler
            s2 = Get-ArtifactFingerprint -Path $s2Compiler
            s3 = Get-ArtifactFingerprint -Path $s3Compiler
        }
        RootchunkSource = $rootSource
        ExportRoots = Get-ArtifactFingerprint -Path $exportRoots
        EmitRoots = Get-ArtifactFingerprint -Path $emitRoots
        IR = [pscustomobject]$irResults
        AllIrHashesEqual = ($uniqueIrHashes.Count -eq 1)
        C1NotSlowerThanS1 = $c1NotSlowerThanS1
        ResultsJsonl = $resultsJsonl
    }
    Write-Utf8File -Path (Join-Path $runRootPath "summary.json") -Text ($summary | ConvertTo-Json -Depth 8)
    if ($uniqueIrHashes.Count -ne 1) {
        throw "S1/C1/S2/S3 emitted different LLVM IR hashes for the same rootchunk."
    }
    if (-not $c1NotSlowerThanS1) {
        throw "MIR2CPP C1 median rootchunk compile time exceeded native S1."
    }
    Write-Host "MIR2CPP multigeneration chain passed. Summary: $(Join-Path $runRootPath 'summary.json')"
} catch {
    $failure = [pscustomobject][ordered]@{
        FailedUtc = [DateTime]::UtcNow.ToString("o")
        Message = $_.Exception.Message
        Detail = $_.Exception.ToString()
        CompletedSteps = $stepRecords.Count
        RunRoot = $runRootPath
        ResultsJsonl = $resultsJsonl
    }
    Write-Utf8File -Path (Join-Path $runRootPath "failure.json") -Text ($failure | ConvertTo-Json -Depth 5)
    throw
} finally {
    [Environment]::SetEnvironmentVariable("LLVM_ROOT", $oldLlvmRoot, "Process")
    [Environment]::SetEnvironmentVariable("VYX_BOOTSTRAP_PROFILE", $oldBootstrapProfile, "Process")
}

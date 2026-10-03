[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [int]$TimeoutSec = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) {
    throw "TimeoutSec must be between 1 and 600."
}

$testsRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testsRoot "..")).Path
. (Join-Path $repoRoot "bootstrap_compiler/scripts/VyxTestProcess.ps1")

$windowsHost = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    [bool]$IsWindows
} else {
    $env:OS -eq "Windows_NT"
}
$compilerName = if ($windowsHost) { "boot.exe" } else { "boot" }
$objectSuffix = if ($windowsHost) { ".obj" } else { ".o" }
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$runtimeDir = Split-Path -Parent $BootstrapCompiler
$pathSeparator = if ($windowsHost) { ";" } else { ":" }
$source = Join-Path $testsRoot "cases/mir_inferred_local_types.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_inferred_local_types_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH
$originalProfile = $env:VYX_BOOTSTRAP_PROFILE

function Invoke-Compiler {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $invokeArgs = @{
        FilePath = $BootstrapCompiler
        ArgumentList = $Arguments
        WorkingDirectory = $repoRoot
        StdoutLog = Join-Path $runRoot ($Name + ".stdout.log")
        StderrLog = Join-Path $runRoot ($Name + ".stderr.log")
        DialogLog = Join-Path $runRoot ($Name + ".dialog.log")
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $invokeArgs.MemoryLimitMB = 2048 }
    $result = Invoke-VyxProcess @invokeArgs
    if ($result.ExitCode -ne 0 -or $result.TimedOut -or
        $result.MemoryExceeded -or $result.DialogCaught) {
        throw "$Name failed: exit=$($result.ExitCode) timeout=$($result.TimedOut) memory=$($result.MemoryExceeded)"
    }
}

function Read-Log([string]$Name) {
    return [IO.File]::ReadAllText((Join-Path $runRoot ($Name + ".stdout.log")))
}

function Get-MirDump([string]$Text) {
    $start = $Text.IndexOf("mir2.unit ", [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "compiler output is missing mir2.unit dump" }
    return $Text.Substring($start).Replace("`r`n", "`n").Trim()
}

function Get-LocalRecord([string]$Dump, [string]$Name) {
    $pattern = '(?m)^\s+local #(\d+) fn=#(\d+) hir_local=#\d+ name=' +
        [regex]::Escape($Name) + ' type=#(\d+):([^\s]+) flags=(\d+) '
    $matches = [regex]::Matches($Dump, $pattern)
    if ($matches.Count -ne 1) {
        throw "expected exactly one MIR local named $Name, found $($matches.Count)"
    }
    $match = $matches[0]
    return [pscustomobject]@{
        Id = [int]$match.Groups[1].Value
        FunctionId = [int]$match.Groups[2].Value
        Type = $match.Groups[4].Value
        Flags = [int]$match.Groups[5].Value
    }
}

function Assert-InferredLocalContract([string]$Dump, [string]$Level) {
    $bytes = Get-LocalRecord $Dump "bytes"
    $allocBytes = Get-LocalRecord $Dump "alloc_bytes"
    foreach ($entry in @(
        @{ Label = "bytes"; Record = $bytes },
        @{ Label = "alloc_bytes"; Record = $allocBytes }
    )) {
        $record = $entry.Record
        if ($record.Type -cne "i64") {
            throw "$Level $($entry.Label) expected MIR type i64, found $($record.Type)"
        }
        if (($record.Flags -band 2048) -ne 0) {
            throw "$Level $($entry.Label) leaked HIR inferred-type bit into MIR stack-class storage flags=$($record.Flags)"
        }
        if ($record.Flags -ne 0) {
            throw "$Level $($entry.Label) expected ordinary MIR local flags=0, found $($record.Flags)"
        }
    }
    if ($bytes.FunctionId -ne $allocBytes.FunctionId) {
        throw "$Level bytes and alloc_bytes were lowered into different functions"
    }

    $ifPattern = '(?m)^\s+local #\d+ fn=#' + $bytes.FunctionId +
        ' hir_local=#0 name=ifexpr\$[^\s]+ type=#\d+:i64 flags=(\d+) '
    $ifMatches = [regex]::Matches($Dump, $ifPattern)
    if ($ifMatches.Count -ne 1) {
        throw "$Level expected one i64 if-expression result local, found $($ifMatches.Count)"
    }
    $ifFlags = [int]$ifMatches[0].Groups[1].Value
    if (($ifFlags -band 128) -eq 0 -or ($ifFlags -band 2048) -ne 0) {
        throw "$Level if-expression result has invalid MIR flags=$ifFlags"
    }
}

try {
    $env:Path = $runtimeDir + $pathSeparator + $originalPath
    if (-not $windowsHost) {
        $env:LD_LIBRARY_PATH = $runtimeDir + $pathSeparator + $originalLdLibraryPath
        if (Get-Variable -Name IsMacOS -ErrorAction SilentlyContinue) {
            if ([bool]$IsMacOS) {
                $env:DYLD_LIBRARY_PATH = $runtimeDir + $pathSeparator + $originalDyldLibraryPath
            }
        }
    }
    $env:VYX_BOOTSTRAP_PROFILE = "0"

    foreach ($level in @("O0", "O2")) {
        $flag = "-" + $level
        $dumpName = $level.ToLowerInvariant() + "_dump"
        Invoke-Compiler -Name $dumpName -Arguments @(
            "--src=file", $source, "--dump-mir2", "--mir-verify-strict", $flag
        )
        $log = Read-Log $dumpName
        if ($log -notmatch '(?m)^\[mir2-verify:strict\] summary .* noncanonical_type=0\r?$' -or
            $log -notmatch '(?m)^\[mir2-verify\] functions=\d+ .* errors=0\r?$') {
            throw "$level strict MIR verification evidence is incomplete"
        }
        Assert-InferredLocalContract (Get-MirDump $log) $level

        $object = Join-Path $runRoot ("mir_inferred_local_types_" + $level.ToLowerInvariant() + $objectSuffix)
        $emitName = $level.ToLowerInvariant() + "_object"
        Invoke-Compiler -Name $emitName -Arguments @(
            "--src=file", $source, "--emit=obj", $flag, "-o", $object
        )
        if (-not (Test-Path -LiteralPath $object -PathType Leaf) -or
            (Get-Item -LiteralPath $object).Length -le 0) {
            throw "$level object emission did not produce a non-empty object"
        }
        # Direct object emission is intentionally quiet on this driver path;
        # the non-empty output file is the authoritative emission evidence.
    }

    Write-Host "mir_inferred_local_types: OK"
    Write-Host "mir_inferred_local_types evidence=$runRoot"
} finally {
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

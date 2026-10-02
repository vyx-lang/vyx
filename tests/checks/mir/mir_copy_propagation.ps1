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
. (Join-Path $repoRoot "bootstrap_compiler\scripts\VyxTestProcess.ps1")

$windowsHost = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    [bool]$IsWindows
} else {
    $env:OS -eq "Windows_NT"
}
$compilerName = if ($windowsHost) { "boot.exe" } else { "boot" }
$exeSuffix = if ($windowsHost) { ".exe" } else { "" }
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$runtimeDir = Split-Path -Parent $BootstrapCompiler
$pathSeparator = if ($windowsHost) { ";" } else { ":" }
$source = Join-Path $testsRoot "cases/mir_copy_propagation.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_copy_propagation_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH
$originalAudit = $env:VYX_MIR_PASS_AUDIT
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

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [int]$MemoryLimitMB = 2048
    )
    $invokeArgs = @{
        FilePath = $FilePath
        ArgumentList = $Arguments
        WorkingDirectory = $WorkingDirectory
        StdoutLog = Join-Path $runRoot ($Name + ".stdout.log")
        StderrLog = Join-Path $runRoot ($Name + ".stderr.log")
        DialogLog = Join-Path $runRoot ($Name + ".dialog.log")
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $invokeArgs.MemoryLimitMB = $MemoryLimitMB }
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

function Get-FunctionId([string]$Dump, [string]$Name) {
    $pattern = '(?m)^\s+fn #(\d+) .* name=' + [regex]::Escape($Name) + ' '
    $match = [regex]::Match($Dump, $pattern)
    if (-not $match.Success) { throw "MIR dump is missing function $Name" }
    return [int]$match.Groups[1].Value
}

function Get-LocalPlace([string]$Dump, [int]$FunctionId, [string]$Name) {
    $pattern = '(?m)^\s+local #\d+ fn=#' + $FunctionId + ' .* name=' + [regex]::Escape($Name) + ' .* place=#(\d+) '
    $match = [regex]::Match($Dump, $pattern)
    if (-not $match.Success) { throw "MIR dump is missing local $Name in function #$FunctionId" }
    return [int]$match.Groups[1].Value
}

function Get-ReadPlaces([string]$Dump, [int]$FunctionId, [string]$Name) {
    $pattern = '(?m)^\s+value #\d+ fn=#' + $FunctionId + ' kind=6 .* place=#(\d+) .* name=' + [regex]::Escape($Name) + ' '
    $matches = [regex]::Matches($Dump, $pattern)
    if ($matches.Count -eq 0) { throw "MIR dump is missing read-place value $Name in function #$FunctionId" }
    return @($matches | ForEach-Object { [int]$_.Groups[1].Value })
}

function Assert-ReadPlaces([string]$Dump, [string]$FunctionName, [string]$ReadName, [int]$ExpectedPlace) {
    $functionId = Get-FunctionId $Dump $FunctionName
    $actual = Get-ReadPlaces $Dump $functionId $ReadName
    foreach ($place in $actual) {
        if ($place -ne $ExpectedPlace) {
            throw "$FunctionName read $ReadName uses place #$place, expected #$ExpectedPlace"
        }
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
    $env:VYX_MIR_PASS_AUDIT = "1"

    Invoke-Compiler -Name "o0_dump" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O0"
    )
    Invoke-Compiler -Name "o2_dump_a" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2"
    )
    Invoke-Compiler -Name "o2_dump_b" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2"
    )

    $o0Log = Read-Log "o0_dump"
    $o2Log = Read-Log "o2_dump_a"
    $o0Dump = Get-MirDump $o0Log
    $o2Dump = Get-MirDump $o2Log
    if ((Get-MirDump (Read-Log "o2_dump_b")) -cne $o2Dump) {
        throw "two O2 copy propagation runs produced different MIR dumps"
    }
    if ($o2Log -notmatch '(?m)^\[mir-analysis\] begin pass=copy-propagation ' -or
        $o2Log -notmatch '(?m)^\[mir-analysis\] copy-prop function=\d+ .* rewrites=[1-9]\d* ' -or
        $o2Log -notmatch '(?m)^\[mir-analysis\] copy-prop idempotent=1 ') {
        throw "copy propagation audit evidence is incomplete"
    }

    $chainId = Get-FunctionId $o0Dump "chain_copy"
    $chainInput = Get-LocalPlace $o0Dump $chainId "input"
    $chainThird = Get-LocalPlace $o0Dump $chainId "third"
    Assert-ReadPlaces $o0Dump "chain_copy" "third" $chainThird
    Assert-ReadPlaces $o2Dump "chain_copy" "third" $chainInput

    $crossId = Get-FunctionId $o0Dump "cross_block_copy"
    $crossSource = Get-LocalPlace $o0Dump $crossId "source"
    $crossCopied = Get-LocalPlace $o0Dump $crossId "copied"
    Assert-ReadPlaces $o0Dump "cross_block_copy" "copied" $crossCopied
    Assert-ReadPlaces $o2Dump "cross_block_copy" "copied" $crossSource
    $crossBlocks = [regex]::Matches($o2Dump, '(?m)^\s+block #\d+ fn=#' + $crossId + '\s+removed=0\b.*?$') |
        ForEach-Object { $_.Value }
    if (($crossBlocks -join "`n") -notmatch ' term=3 ') {
        throw "cross_block_copy no longer contains an active CFG branch"
    }

    $writtenId = Get-FunctionId $o0Dump "source_written_after_copy"
    $writtenInput = Get-LocalPlace $o0Dump $writtenId "input"
    Assert-ReadPlaces $o2Dump "source_written_after_copy" "snapshot" $writtenInput

    foreach ($case in @(
        @{ Function = "escaped_source_copy"; Read = "snapshot" },
        @{ Function = "captured_source_copy"; Read = "snapshot" }
    )) {
        $functionId = Get-FunctionId $o0Dump $case.Function
        $snapshotPlace = Get-LocalPlace $o0Dump $functionId $case.Read
        Assert-ReadPlaces $o2Dump $case.Function $case.Read $snapshotPlace
    }

    $callId = Get-FunctionId $o0Dump "copy_into_call"
    $callInput = Get-LocalPlace $o0Dump $callId "input"
    Assert-ReadPlaces $o2Dump "copy_into_call" "copied" $callInput

    $env:VYX_MIR_PASS_AUDIT = "0"
    foreach ($level in @("-O0", "-O2")) {
        $name = $level.Substring(1).ToLowerInvariant() + "_jit"
        Invoke-Compiler -Name $name -Arguments @("--src=file", $source, "--run=jit", $level)
        if ((Read-Log $name).Trim() -cne "mir_copy_propagation OK") {
            throw "$level JIT output mismatch: $((Read-Log $name).Trim())"
        }
    }

    $cppOut = Join-Path $runRoot "o2_cpp"
    Invoke-Compiler -Name "o2_cpp_emit" -Arguments @(
        "--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut
    )
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    Invoke-External -Name "o2_cpp_configure" -FilePath $cmake `
        -Arguments @("--preset", "ninja-release") -WorkingDirectory $cppOut
    Invoke-External -Name "o2_cpp_build" -FilePath $cmake `
        -Arguments @("--build", "--preset", "ninja-release") -WorkingDirectory $cppOut
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_mir_copy_propagation" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "MIR2CPP build did not produce $cppExe"
    }
    Invoke-External -Name "o2_cpp_run" -FilePath $cppExe `
        -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
    if ((Read-Log "o2_cpp_run").Trim() -cne "mir_copy_propagation OK") {
        throw "O2 MIR2CPP output mismatch: $((Read-Log 'o2_cpp_run').Trim())"
    }

    Write-Host "mir_copy_propagation: OK"
    Write-Host "mir_copy_propagation evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

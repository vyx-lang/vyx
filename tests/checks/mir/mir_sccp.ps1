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
$exeSuffix = if ($windowsHost) { ".exe" } else { "" }
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$runtimeDir = Split-Path -Parent $BootstrapCompiler
$pathSeparator = if ($windowsHost) { ";" } else { ":" }
$source = Join-Path $testsRoot "cases/mir_sccp.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_sccp_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
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

function Get-ActiveBranchCount([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    $pattern = '(?m)^\s+block #\d+ fn=#' + $functionId + ' removed=0 .* term=3 '
    return [regex]::Matches($Dump, $pattern).Count
}

function Get-ActiveMatchCount([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    $pattern = '(?m)^\s+block #\d+ fn=#' + $functionId + ' removed=0 .* term=4 '
    return [regex]::Matches($Dump, $pattern).Count
}

function Get-AssignedValueKind([string]$Dump, [string]$InstructionName) {
    $instrPattern = '(?m)^\s+instr #\d+ .* kind=2 .* value=#(\d+) .* name=' +
        [regex]::Escape($InstructionName) + '\r?$'
    $instr = [regex]::Match($Dump, $instrPattern)
    if (-not $instr.Success) { throw "MIR dump is missing assignment $InstructionName" }
    $valueId = $instr.Groups[1].Value
    $value = [regex]::Match($Dump, '(?m)^\s+value #' + $valueId + ' .* kind=(\d+) ')
    if (-not $value.Success) { throw "MIR dump is missing value #$valueId for $InstructionName" }
    return [int]$value.Groups[1].Value
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
    Invoke-Compiler -Name "o2_dump" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2"
    )
    $o0Log = Read-Log "o0_dump"
    $o2Log = Read-Log "o2_dump"
    $o0Dump = Get-MirDump $o0Log
    $o2Dump = Get-MirDump $o2Log

    if ($o2Log -notmatch '(?m)^\[mir-analysis\] begin pass=sccp required=1 ' -or
        $o2Log -notmatch '(?m)^\[mir-analysis\] end pass=sccp changes=[1-9]\d* ' -or
        $o2Log -notmatch '(?m)^\[mir-analysis\] sccp function=\d+ .* rewrites=[1-9]\d* ') {
        throw "SCCP audit evidence is incomplete"
    }

    $positiveFunctions = @("same_join", "same_width_join", "same_null_join", "loop_same")
    foreach ($functionName in $positiveFunctions) {
        $o0Branches = Get-ActiveBranchCount $o0Dump $functionName
        $o2Branches = Get-ActiveBranchCount $o2Dump $functionName
        if ($o2Branches -ge $o0Branches) {
            throw "$functionName did not lose its SCCP-proven branch: O0=$o0Branches O2=$o2Branches"
        }
    }

    $negativeFunctions = @("different_join", "loop_changes", "escaped_local", "captured_local", "call_result")
    foreach ($functionName in $negativeFunctions) {
        $o0Branches = Get-ActiveBranchCount $o0Dump $functionName
        $o2Branches = Get-ActiveBranchCount $o2Dump $functionName
        if ($o0Branches -le 0 -or $o2Branches -ne $o0Branches) {
            throw "$functionName lost a conservative branch: O0=$o0Branches O2=$o2Branches"
        }
    }

    $plainEnumO0Matches = Get-ActiveMatchCount $o0Dump "plain_enum_same_join"
    $plainEnumO2Matches = Get-ActiveMatchCount $o2Dump "plain_enum_same_join"
    if ($plainEnumO0Matches -le 0 -or $plainEnumO2Matches -ne 0) {
        throw "known zero-payload enum match was not fully SCCP-folded: O0=$plainEnumO0Matches O2=$plainEnumO2Matches"
    }

    $differentEnumO0Matches = Get-ActiveMatchCount $o0Dump "plain_enum_different_join"
    $differentEnumO2Matches = Get-ActiveMatchCount $o2Dump "plain_enum_different_join"
    if ($differentEnumO0Matches -le 0 -or $differentEnumO2Matches -le 0) {
        throw "different enum tags at a join were incorrectly collapsed: O0=$differentEnumO0Matches O2=$differentEnumO2Matches"
    }

    foreach ($functionName in @(
        "plain_enum_constructor_call_stays_dynamic",
        "payload_enum_stays_dynamic",
        "plain_enum_guard_stays_dynamic",
        "plain_enum_default_stays_dynamic"
    )) {
        $o0Matches = Get-ActiveMatchCount $o0Dump $functionName
        $o2Matches = Get-ActiveMatchCount $o2Dump $functionName
        if ($o0Matches -le 0 -or $o2Matches -le 0) {
            throw "$functionName left the conservative SCCP domain: O0=$o0Matches O2=$o2Matches"
        }
    }

    if ((Get-AssignedValueKind $o0Dump "same_copy") -ne 6 -or
        (Get-AssignedValueKind $o2Dump "same_copy") -ne 1) {
        throw "cross-block i32 copy was not rewritten from read_place to const_int"
    }
    if ((Get-AssignedValueKind $o2Dump "width_copy") -ne 1) {
        throw "fixed-width integer copy was not rewritten to const_int"
    }
    if ((Get-AssignedValueKind $o0Dump "null_copy") -ne 6 -or
        (Get-AssignedValueKind $o2Dump "null_copy") -ne 5) {
        throw "cross-block null copy was not rewritten from read_place to null"
    }
    if ((Get-AssignedValueKind $o2Dump "different_copy") -ne 6 -or
        (Get-AssignedValueKind $o2Dump "escaped_copy") -ne 6 -or
        (Get-AssignedValueKind $o2Dump "captured_copy") -ne 6 -or
        (Get-AssignedValueKind $o2Dump "call_value") -ne 10) {
        throw "SCCP rewrote a join/escape/capture/call value outside its conservative domain"
    }
    if ((Get-AssignedValueKind $o0Dump "enum_observed") -ne 6 -or
        (Get-AssignedValueKind $o2Dump "enum_observed") -ne 6) {
        throw "SCCP materialized a plain enum tag instead of retaining its target reference/read"
    }

    $env:VYX_MIR_PASS_AUDIT = "0"
    foreach ($level in @("-O0", "-O2")) {
        $name = $level.Substring(1).ToLowerInvariant() + "_jit"
        Invoke-Compiler -Name $name -Arguments @("--src=file", $source, "--run=jit", $level)
        $output = (Read-Log $name).Trim()
        if ($output -cne "mir_sccp OK") { throw "$level JIT output mismatch: $output" }
    }

    $aotExe = Join-Path $runRoot ("o2_aot" + $exeSuffix)
    Invoke-Compiler -Name "o2_aot_emit" -Arguments @(
        "--src=file", $source, "--emit=exe", "-O2", "-o", $aotExe
    )
    if (-not (Test-Path -LiteralPath $aotExe -PathType Leaf)) {
        throw "O2 AOT emission did not produce $aotExe"
    }
    Invoke-External -Name "o2_aot_run" -FilePath $aotExe `
        -WorkingDirectory (Split-Path -Parent $aotExe) -MemoryLimitMB 512
    $aotOutput = (Read-Log "o2_aot_run").Trim()
    if ($aotOutput -cne "mir_sccp OK") { throw "O2 AOT output mismatch: $aotOutput" }

    $cppOut = Join-Path $runRoot "o2_cpp"
    Invoke-Compiler -Name "o2_cpp_emit" -Arguments @(
        "--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut
    )
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    Invoke-External -Name "o2_cpp_configure" -FilePath $cmake `
        -Arguments @("--preset", "ninja-release") -WorkingDirectory $cppOut
    Invoke-External -Name "o2_cpp_build" -FilePath $cmake `
        -Arguments @("--build", "--preset", "ninja-release") -WorkingDirectory $cppOut
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_mir_sccp" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "MIR2CPP build did not produce $cppExe"
    }
    Invoke-External -Name "o2_cpp_run" -FilePath $cppExe `
        -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
    $cppOutput = (Read-Log "o2_cpp_run").Trim()
    if ($cppOutput -cne "mir_sccp OK") { throw "O2 MIR2CPP output mismatch: $cppOutput" }

    Write-Host "mir_sccp: OK"
    Write-Host "mir_sccp evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

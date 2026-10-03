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
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$runtimeDir = Split-Path -Parent $BootstrapCompiler
$pathSeparator = if ($windowsHost) { ";" } else { ":" }
$source = Join-Path $testsRoot "cases/mir_local_facts.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_local_facts_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH
$originalPassAudit = $env:VYX_MIR_PASS_AUDIT
$originalLocalFactsAudit = $env:VYX_MIR_LOCAL_FACTS_AUDIT
$originalProfile = $env:VYX_BOOTSTRAP_PROFILE

$flagCapture = 1
$flagReferenceCapture = 2
$flagDirectAddress = 4
$flagDerivedAddress = 8
$flagMayEscape = 16
$flagCall = 32
$flagReturn = 64
$flagAssignment = 128
$flagAggregate = 256
$flagUnknownIndirection = 512
$cfgNone = 0
$cfgClosed = 1
$cfgOpenExit = 2
$cfgBarrier = 4

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

function Get-FunctionIds([string]$Dump) {
    $ids = @{}
    foreach ($match in [regex]::Matches($Dump, '(?m)^\s+fn #(\d+) .* name=([^\s]+)\s')) {
        $ids[$match.Groups[2].Value] = [int]$match.Groups[1].Value
    }
    return $ids
}

function Get-LocalFacts([string]$Text) {
    $facts = @{}
    $pattern = '(?m)^\[mir-analysis\] local-facts local=#(\d+) fn=#(\d+) name=([^\s]+) live=(\d+) first_live=#(\d+) dead=(\d+) interval=(\d+) cfg=(\d+) flags=(\d+)\r?$'
    foreach ($match in [regex]::Matches($Text, $pattern)) {
        $key = $match.Groups[2].Value + ":" + $match.Groups[3].Value
        if ($facts.ContainsKey($key)) { throw "duplicate local-facts audit record for $key" }
        $facts[$key] = [pscustomobject]@{
            LocalId = [int]$match.Groups[1].Value
            FunctionId = [int]$match.Groups[2].Value
            Name = $match.Groups[3].Value
            Live = [int]$match.Groups[4].Value
            FirstLive = [int]$match.Groups[5].Value
            Dead = [int]$match.Groups[6].Value
            Interval = [int]$match.Groups[7].Value
            Cfg = [int]$match.Groups[8].Value
            Flags = [int]$match.Groups[9].Value
        }
    }
    if ($facts.Count -eq 0) {
        throw "VYX_MIR_LOCAL_FACTS_AUDIT=1 produced no local-facts audit records"
    }
    return $facts
}

function Get-LocalFact($Facts, $FunctionIds, [string]$FunctionName, [string]$LocalName) {
    if (-not $FunctionIds.ContainsKey($FunctionName)) {
        throw "MIR dump is missing function $FunctionName"
    }
    $key = $FunctionIds[$FunctionName].ToString() + ":" + $LocalName
    if (-not $Facts.ContainsKey($key)) {
        throw "local-facts audit is missing $FunctionName::$LocalName"
    }
    return $Facts[$key]
}

function Assert-FlagSet($Fact, [int]$Flag, [string]$Label) {
    if (($Fact.Flags -band $Flag) -eq 0) {
        throw "$Label flags=$($Fact.Flags) is missing bit $Flag"
    }
}

function Assert-FlagClear($Fact, [int]$Flag, [string]$Label) {
    if (($Fact.Flags -band $Flag) -ne 0) {
        throw "$Label flags=$($Fact.Flags) unexpectedly includes bit $Flag"
    }
}

function Assert-Storage($Fact, [string]$Label, [int]$MinLive = 1, [int]$MinDead = 1) {
    if ($Fact.Live -lt $MinLive) { throw "$Label expected live >= $MinLive, found $($Fact.Live)" }
    if ($Fact.FirstLive -le 0) { throw "$Label expected a positive first_live instruction id" }
    if ($Fact.Dead -lt $MinDead) { throw "$Label expected dead >= $MinDead, found $($Fact.Dead)" }
}

function Assert-Cfg($Fact, [int]$Expected, [string]$Label) {
    if ($Fact.Cfg -ne $Expected) {
        throw "$Label expected cfg=$Expected, found cfg=$($Fact.Cfg)"
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
    $env:VYX_MIR_LOCAL_FACTS_AUDIT = "1"

    # This deliberately stops at the MIR dump: opaque_local_sink has no
    # implementation and this regression must not depend on native linking.
    Invoke-Compiler -Name "dump" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O0"
    )
    $audit = Read-Log "dump"
    $functionIds = Get-FunctionIds (Get-MirDump $audit)
    $facts = Get-LocalFacts $audit

    $noEscapeFlags = $flagCapture -bor $flagReferenceCapture -bor $flagDirectAddress -bor
        $flagDerivedAddress -bor $flagMayEscape -bor $flagCall -bor $flagReturn -bor
        $flagAssignment -bor $flagAggregate -bor $flagUnknownIndirection

    foreach ($case in @(
        @{ Function = "scalar_nested_scope"; Local = "outer_scalar" },
        @{ Function = "scalar_nested_scope"; Local = "nested_scalar" },
        @{ Function = "loop_local"; Local = "iteration_local" }
    )) {
        $fact = Get-LocalFact $facts $functionIds $case.Function $case.Local
        Assert-Storage $fact ($case.Function + "::" + $case.Local)
        Assert-FlagClear $fact $noEscapeFlags ($case.Function + "::" + $case.Local)
    }

    $early = Get-LocalFact $facts $functionIds "early_return_local" "early_local"
    Assert-Storage $early "early_return_local::early_local"
    Assert-FlagClear $early $noEscapeFlags "early_return_local::early_local"
    if ($early.Dead -ne 2 -or $early.Interval -ne 2) {
        throw "early_return_local::early_local expected dead=2 interval=2, found dead=$($early.Dead) interval=$($early.Interval)"
    }
    Assert-Cfg $early $cfgBarrier "early_return_local::early_local"

    foreach ($case in @(
        @{ Function = "scalar_nested_scope"; Local = "nested_scalar" },
        @{ Function = "cfg_closed_simple"; Local = "cfg_closed_local" },
        @{ Function = "cfg_early_closed"; Local = "cfg_early_local" },
        @{ Function = "loop_local"; Local = "iteration_local" }
    )) {
        $fact = Get-LocalFact $facts $functionIds $case.Function $case.Local
        Assert-Cfg $fact $cfgClosed ($case.Function + "::" + $case.Local)
    }

    $seed = Get-LocalFact $facts $functionIds "scalar_nested_scope" "seed"
    Assert-Cfg $seed $cfgNone "scalar_nested_scope::seed"

    $open = Get-LocalFact $facts $functionIds "cfg_open_empty_match" "cfg_open_local"
    Assert-Cfg $open $cfgOpenExit "cfg_open_empty_match::cfg_open_local"

    foreach ($case in @(
        @{ Function = "opaque_by_value"; Local = "by_value_local" },
        @{ Function = "aggregate_transport"; Local = "aggregate_source" },
        @{ Function = "deep_projection_transport"; Local = "deep_projection_local" }
    )) {
        $fact = Get-LocalFact $facts $functionIds $case.Function $case.Local
        Assert-Storage $fact ($case.Function + "::" + $case.Local)
        Assert-FlagClear $fact $noEscapeFlags ($case.Function + "::" + $case.Local)
        Assert-Cfg $fact $cfgBarrier ($case.Function + "::" + $case.Local)
    }

    $direct = Get-LocalFact $facts $functionIds "opaque_direct_address" "direct_local"
    Assert-Storage $direct "opaque_direct_address::direct_local"
    foreach ($flag in @($flagDirectAddress, $flagMayEscape, $flagCall)) {
        Assert-FlagSet $direct $flag "opaque_direct_address::direct_local"
    }
    Assert-FlagClear $direct $flagDerivedAddress "opaque_direct_address::direct_local"
    Assert-Cfg $direct $cfgBarrier "opaque_direct_address::direct_local"

    foreach ($case in @(
        @{ Function = "record_field_address"; Local = "record_local" },
        @{ Function = "array_index_address"; Local = "array_local" }
    )) {
        $fact = Get-LocalFact $facts $functionIds $case.Function $case.Local
        Assert-Storage $fact ($case.Function + "::" + $case.Local)
        foreach ($flag in @($flagDerivedAddress, $flagMayEscape, $flagCall)) {
            Assert-FlagSet $fact $flag ($case.Function + "::" + $case.Local)
        }
        Assert-FlagClear $fact $flagDirectAddress ($case.Function + "::" + $case.Local)
        Assert-Cfg $fact $cfgBarrier ($case.Function + "::" + $case.Local)
    }

    $derefValue = Get-LocalFact $facts $functionIds "dereference_address" "dereference_value"
    Assert-Storage $derefValue "dereference_address::dereference_value"
    foreach ($flag in @($flagDirectAddress, $flagMayEscape, $flagAssignment)) {
        Assert-FlagSet $derefValue $flag "dereference_address::dereference_value"
    }
    Assert-Cfg $derefValue $cfgBarrier "dereference_address::dereference_value"

    $derefPtr = Get-LocalFact $facts $functionIds "dereference_address" "dereference_ptr"
    Assert-Storage $derefPtr "dereference_address::dereference_ptr"
    foreach ($flag in @($flagDerivedAddress, $flagMayEscape, $flagCall, $flagUnknownIndirection)) {
        Assert-FlagSet $derefPtr $flag "dereference_address::dereference_ptr"
    }
    Assert-Cfg $derefPtr $cfgBarrier "dereference_address::dereference_ptr"

    $returnAddress = Get-LocalFact $facts $functionIds "return_local_addr" "return_address_local"
    Assert-Storage $returnAddress "return_local_addr::return_address_local"
    foreach ($flag in @($flagDirectAddress, $flagMayEscape, $flagReturn)) {
        Assert-FlagSet $returnAddress $flag "return_local_addr::return_address_local"
    }
    Assert-Cfg $returnAddress $cfgBarrier "return_local_addr::return_address_local"

    $byValue = Get-LocalFact $facts $functionIds "closure_value_capture" "value_capture_source"
    Assert-Storage $byValue "closure_value_capture::value_capture_source"
    Assert-FlagSet $byValue $flagCapture "closure_value_capture::value_capture_source"
    foreach ($flag in @($flagReferenceCapture, $flagDirectAddress, $flagDerivedAddress, $flagMayEscape,
                         $flagCall, $flagAssignment, $flagAggregate, $flagUnknownIndirection)) {
        Assert-FlagClear $byValue $flag "closure_value_capture::value_capture_source"
    }

    $byReference = Get-LocalFact $facts $functionIds "closure_reference_capture" "reference_capture_source"
    Assert-Storage $byReference "closure_reference_capture::reference_capture_source"
    foreach ($flag in @($flagCapture, $flagReferenceCapture, $flagMayEscape)) {
        Assert-FlagSet $byReference $flag "closure_reference_capture::reference_capture_source"
    }
    Assert-Cfg $byValue $cfgBarrier "closure_value_capture::value_capture_source"
    Assert-Cfg $byReference $cfgBarrier "closure_reference_capture::reference_capture_source"

    $env:VYX_MIR_PASS_AUDIT = "0"
    $env:VYX_MIR_LOCAL_FACTS_AUDIT = "0"
    Invoke-Compiler -Name "dump_disabled" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O0"
    )
    if ((Read-Log "dump_disabled").Contains("[mir-analysis] local-facts")) {
        throw "VYX_MIR_LOCAL_FACTS_AUDIT=0 emitted local-facts audit output"
    }

    Write-Host "mir_local_facts: OK"
    Write-Host "mir_local_facts evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalPassAudit
    $env:VYX_MIR_LOCAL_FACTS_AUDIT = $originalLocalFactsAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

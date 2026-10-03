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
$source = Join-Path $testsRoot "cases/mir_pass_analysis.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_analysis_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
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

function Read-Log([string]$Name) {
    return [IO.File]::ReadAllText((Join-Path $runRoot ($Name + ".stdout.log")))
}

function Get-MirDump([string]$Text) {
    $start = $Text.IndexOf("mir2.unit ", [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "compiler output is missing mir2.unit dump" }
    return $Text.Substring($start).Replace("`r`n", "`n").Trim()
}

function Assert-InOrder([string]$Text, [string[]]$Needles) {
    $offset = 0
    foreach ($needle in $Needles) {
        $next = $Text.IndexOf($needle, $offset, [StringComparison]::Ordinal)
        if ($next -lt 0) { throw "missing ordered audit event: $needle" }
        $offset = $next + $needle.Length
    }
}

function Get-AuditMetric([string]$Text, [string]$Analysis, [int]$Generation, [string]$Metric) {
    $pattern = ('(?m)^\[mir-analysis\] build ' + [regex]::Escape($Analysis) +
                ' generation=' + $Generation + ' ' + [regex]::Escape($Metric) + '=(\d+)\r?$')
    $match = [regex]::Match($Text, $pattern)
    if (-not $match.Success) {
        throw "missing $Analysis generation=$Generation $Metric audit metric"
    }
    return [int]$match.Groups[1].Value
}

function Assert-StrictStage([string]$Text, [string]$Stage) {
    $pattern = '(?m)^\[mir-analysis\] strict verify stage=' +
        [regex]::Escape($Stage) + ' errors=0\r?$'
    if (-not [regex]::IsMatch($Text, $pattern)) {
        throw "missing successful strict verifier audit for stage: $Stage"
    }
}

function Assert-StrictScope([string]$Text, [string]$Scope, [string]$Stage) {
    $pattern = '(?m)^\[mir-analysis\] strict verify scope=' +
        [regex]::Escape($Scope) + ' stage=' + [regex]::Escape($Stage) + '\r?$'
    if (-not [regex]::IsMatch($Text, $pattern)) {
        throw "missing strict verifier scope=$Scope audit for stage: $Stage"
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

    $env:VYX_MIR_PASS_AUDIT = "0"
    Invoke-Compiler -Name "o0_plain" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O0"
    )
    $o0Plain = Read-Log "o0_plain"
    if ($o0Plain.Contains("[mir-analysis]")) {
        throw "VYX_MIR_PASS_AUDIT=0 must disable analysis auditing"
    }

    $env:VYX_MIR_PASS_AUDIT = "1"
    Invoke-Compiler -Name "o0_audit_no_strict" -Arguments @(
        "--src=file", $source, "--dump-mir2", "-O0"
    )
    $o0AuditNoStrict = Read-Log "o0_audit_no_strict"
    if ($o0AuditNoStrict -match '(?m)^\[mir-analysis\] strict verify stage=') {
        throw "pipeline ran strict stage verification without --mir-verify-strict"
    }

    Invoke-Compiler -Name "o0_audit" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O0"
    )
    $o0Audit = Read-Log "o0_audit"
    if ((Get-MirDump $o0Plain) -cne (Get-MirDump $o0Audit)) {
        throw "O0 MIR dump changed when read-only analysis auditing was enabled"
    }
    if ((Get-MirDump $o0AuditNoStrict) -cne (Get-MirDump $o0Audit)) {
        throw "--mir-verify-strict changed the O0 MIR dump"
    }
    Assert-StrictStage $o0Audit "pipeline input"
    Assert-StrictScope $o0Audit "unit" "pipeline input"

    Invoke-Compiler -Name "o2_audit" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2"
    )
    $o2Audit = Read-Log "o2_audit"
    foreach ($stage in @(
        "pipeline input",
        "after fold-constants",
        "after fold-constant-branches",
        "after sccp",
        "after copy-propagation",
        "after drop-aware-dce",
        "after select-formation",
        "after cfg-simplify"
    )) {
        Assert-StrictStage $o2Audit $stage
        Assert-StrictScope $o2Audit "unit" $stage
    }

    $streamingIr = Join-Path $runRoot "o2_streaming_strict.ll"
    Invoke-Compiler -Name "o2_streaming_strict" -Arguments @(
        "--src=file", $source, "--emit=ir", "--mir-verify-strict", "-O2", "-o", $streamingIr
    )
    if (-not (Test-Path -LiteralPath $streamingIr -PathType Leaf)) {
        throw "strict streaming LLVM lowering did not produce IR"
    }
    $streamingAudit = Read-Log "o2_streaming_strict"
    foreach ($stage in @(
        "pipeline input",
        "after fold-constants",
        "after fold-constant-branches",
        "after sccp",
        "after copy-propagation",
        "after drop-aware-dce",
        "after select-formation",
        "after cfg-simplify"
    )) {
        Assert-StrictStage $streamingAudit $stage
        Assert-StrictScope $streamingAudit "function" $stage
    }

    Invoke-Compiler -Name "o2_streaming_strict_jit" -Arguments @(
        "--src=file", $source, "--run=jit", "--mir-verify-strict", "-O2"
    )
    $streamingStrictJit = (Read-Log "o2_streaming_strict_jit").Trim()
    if ($streamingStrictJit -notmatch '(?m)^mir_pass_analysis OK$') {
        throw "strict streaming JIT output mismatch: $streamingStrictJit"
    }
    Assert-InOrder -Text $o2Audit -Needles @(
        "[mir-analysis] build cfg generation=0",
        "[mir-analysis] build use-def generation=0",
        "[mir-analysis] begin pass=fold-constants",
        "[mir-analysis] preserve cfg generation=1",
        "[mir-analysis] invalidate use-def generation=1",
        "[mir-analysis] reuse cfg generation=1",
        "[mir-analysis] build use-def generation=1",
        "[mir-analysis] begin pass=fold-constant-branches",
        "[mir-analysis] invalidate cfg generation=2",
        "[mir-analysis] invalidate use-def generation=2",
        "[mir-analysis] build cfg generation=2",
        "[mir-analysis] build use-def generation=2",
        "[mir-analysis] begin pass=sccp required=1 preserved=0 generation=2",
        "[mir-analysis] reuse cfg generation=2",
        "[mir-analysis] invalidate cfg generation=3",
        "[mir-analysis] invalidate use-def generation=3",
        "[mir-analysis] build cfg generation=3",
        "[mir-analysis] build use-def generation=3",
        "[mir-analysis] begin pass=copy-propagation required=1 preserved=1 generation=3",
        "[mir-analysis] reuse cfg generation=3",
        "[mir-analysis] end pass=copy-propagation changes=0 generation=3 valid=3",
        "[mir-analysis] begin pass=drop-aware-dce required=0 preserved=1 generation=3",
        "[mir-analysis] preserve cfg generation=4",
        "[mir-analysis] invalidate use-def generation=4",
        "[mir-analysis] drop-aware-dce removed=1 idempotent=1",
        "[mir-analysis] build use-def generation=4",
        "[mir-analysis] begin pass=select-formation required=1 preserved=0 generation=4",
        "[mir-analysis] reuse cfg generation=4",
        "[mir-analysis] select-formation candidates=0 skipped=0 idempotent=1",
        "[mir-analysis] end pass=select-formation changes=0 generation=4 valid=3",
        "[mir-analysis] begin pass=cfg-simplify required=0 preserved=0 generation=4",
        "[mir-analysis] invalidate cfg generation=5",
        "[mir-analysis] invalidate use-def generation=5",
        "[mir-analysis] build cfg generation=5",
        "[mir-analysis] build use-def generation=5"
    )

    $cfg0 = Get-AuditMetric $o2Audit "cfg" 0 "edges"
    $cfg2 = Get-AuditMetric $o2Audit "cfg" 2 "edges"
    $uses0 = Get-AuditMetric $o2Audit "use-def" 0 "uses"
    $uses1 = Get-AuditMetric $o2Audit "use-def" 1 "uses"
    $uses2 = Get-AuditMetric $o2Audit "use-def" 2 "uses"
    $uses4 = Get-AuditMetric $o2Audit "use-def" 4 "uses"
    if ($cfg0 -le 0 -or $cfg2 -ge $cfg0) {
        throw "constant branch folding did not rebuild a smaller CFG: before=$cfg0 after=$cfg2"
    }
    if ($uses0 -le 0 -or $uses1 -ge $uses0 -or $uses2 -ge $uses1) {
        throw "use-def rebuild counts did not shrink across the two mutating passes: $uses0/$uses1/$uses2"
    }
    if ($uses4 -ge $uses2) {
        throw "drop-aware DCE did not shrink use-def after SCCP: $uses2/$uses4"
    }

    $env:VYX_MIR_PASS_AUDIT = "0"
    foreach ($level in @("-O0", "-O2")) {
        $name = $level.Substring(1).ToLowerInvariant() + "_jit"
        Invoke-Compiler -Name $name -Arguments @("--src=file", $source, "--run=jit", $level)
        $output = (Read-Log $name).Trim()
        if ($output -cne "mir_pass_analysis OK") {
            throw "$level JIT output mismatch: $output"
        }
    }

    Write-Host "mir_analysis: OK"
    Write-Host "mir_analysis evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

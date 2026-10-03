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
$source = Join-Path $testsRoot "cases/mir_dce.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_dce_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH
$originalAudit = $env:VYX_MIR_PASS_AUDIT
$originalProfile = $env:VYX_BOOTSTRAP_PROFILE

function Invoke-Checked {
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

function Get-EvalValues([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    $pattern = '(?m)^\s+instr #\d+ fn=#' + $functionId + '\s+block=#\d+\s+kind=1\s+.*?\svalue=#(\d+)\s'
    return @([regex]::Matches($Dump, $pattern) | ForEach-Object { [int]$_.Groups[1].Value })
}

function Assert-EvalValues {
    param(
        [Parameter(Mandatory = $true)][string]$Dump,
        [Parameter(Mandatory = $true)][string]$FunctionName,
        [Parameter(Mandatory = $true)][int]$ExpectedCount,
        [Parameter(Mandatory = $true)][bool]$ExpectTombstone
    )
    $values = @(Get-EvalValues $Dump $FunctionName)
    if ($values.Count -ne $ExpectedCount) {
        throw "$FunctionName expected $ExpectedCount eval instruction(s), found $($values.Count)"
    }
    foreach ($value in $values) {
        if ($ExpectTombstone -and $value -ne 0) {
            throw "$FunctionName expected a cleared eval tombstone, found value #$value"
        }
        if (-not $ExpectTombstone -and $value -eq 0) {
            throw "$FunctionName effectful eval was incorrectly cleared"
        }
    }
    return $values
}

function Assert-ValueKind([string]$Dump, [int]$ValueId, [int]$ExpectedKind, [string]$Label) {
    $pattern = '(?m)^\s+value #' + $ValueId + '\s+.*\s+kind=' + $ExpectedKind + '\s'
    if (-not [regex]::IsMatch($Dump, $pattern)) {
        throw "$Label expected value #$ValueId to have kind=$ExpectedKind"
    }
}

function Assert-DropInstruction([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    $pattern = '(?m)^\s+instr #\d+ fn=#' + $functionId + '\s+block=#\d+\s+kind=8\s+'
    if ([regex]::Matches($Dump, $pattern).Count -lt 1) {
        throw "$FunctionName is missing an active MIR drop instruction"
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

    foreach ($run in @(
        @{ Name = "o0_dump"; Args = @("--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O0") },
        @{ Name = "o2_dump_a"; Args = @("--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2") },
        @{ Name = "o2_dump_b"; Args = @("--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2") }
    )) {
        Invoke-Checked -Name $run.Name -FilePath $BootstrapCompiler -Arguments $run.Args -WorkingDirectory $repoRoot
    }

    $o0Log = Read-Log "o0_dump"
    $o2Log = Read-Log "o2_dump_a"
    $o0Dump = Get-MirDump $o0Log
    $o2Dump = Get-MirDump $o2Log
    if ((Get-MirDump (Read-Log "o2_dump_b")) -cne $o2Dump) {
        throw "two O2 DCE runs produced different MIR dumps"
    }
    if ($o2Log -notmatch '(?m)^\[mir-analysis\] begin pass=drop-aware-dce required=0 preserved=1 ' -or
        $o2Log -notmatch 'drop-aware-dce removed=[1-9]\d* idempotent=1') {
        throw "drop-aware DCE audit evidence is incomplete"
    }

    Assert-EvalValues -Dump $o0Dump -FunctionName "dead_param_compare" -ExpectedCount 3 -ExpectTombstone $false | Out-Null
    Assert-EvalValues -Dump $o2Dump -FunctionName "dead_param_compare" -ExpectedCount 3 -ExpectTombstone $true | Out-Null
    Assert-EvalValues -Dump $o2Dump -FunctionName "local_read_must_remain" -ExpectedCount 1 -ExpectTombstone $false | Out-Null
    $directValues = Assert-EvalValues -Dump $o2Dump -FunctionName "keep_direct_call" -ExpectedCount 1 -ExpectTombstone $false
    Assert-ValueKind -Dump $o2Dump -ValueId $directValues[0] -ExpectedKind 10 -Label "keep_direct_call"
    $binaryValues = Assert-EvalValues -Dump $o2Dump -FunctionName "keep_binary_call" -ExpectedCount 1 -ExpectTombstone $false
    Assert-ValueKind -Dump $o2Dump -ValueId $binaryValues[0] -ExpectedKind 9 -Label "keep_binary_call"
    Assert-DropInstruction -Dump $o2Dump -FunctionName "keep_auto_drop"

    $env:VYX_MIR_PASS_AUDIT = "0"
    foreach ($level in @("-O0", "-O2")) {
        $name = $level.Substring(1).ToLowerInvariant() + "_jit"
        Invoke-Checked -Name $name -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--run=jit", $level) -WorkingDirectory $repoRoot
        if ((Read-Log $name).Trim() -cne "mir_dce OK") {
            throw "$level JIT output mismatch: $((Read-Log $name).Trim())"
        }
    }

    $aotExe = Join-Path $runRoot ("mir_dce" + $exeSuffix)
    Invoke-Checked -Name "o2_aot_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=exe", "-O2", "-o", $aotExe) -WorkingDirectory $repoRoot
    Invoke-Checked -Name "o2_aot_run" -FilePath $aotExe -WorkingDirectory $runRoot -MemoryLimitMB 512
    if ((Read-Log "o2_aot_run").Trim() -cne "mir_dce OK") {
        throw "O2 AOT output mismatch: $((Read-Log 'o2_aot_run').Trim())"
    }

    $cppOut = Join-Path $runRoot "o2_cpp"
    Invoke-Checked -Name "o2_cpp_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut) -WorkingDirectory $repoRoot
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    Invoke-Checked -Name "o2_cpp_configure" -FilePath $cmake `
        -Arguments @("--preset", "ninja-release") -WorkingDirectory $cppOut
    Invoke-Checked -Name "o2_cpp_build" -FilePath $cmake `
        -Arguments @("--build", "--preset", "ninja-release") -WorkingDirectory $cppOut
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_mir_dce" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "MIR2CPP build did not produce $cppExe"
    }
    Invoke-Checked -Name "o2_cpp_run" -FilePath $cppExe `
        -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
    if ((Read-Log "o2_cpp_run").Trim() -cne "mir_dce OK") {
        throw "O2 MIR2CPP output mismatch: $((Read-Log 'o2_cpp_run').Trim())"
    }

    Write-Host "mir_dce: OK"
    Write-Host "mir_dce evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

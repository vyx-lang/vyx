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
$source = Join-Path $testsRoot "cases/mir_select_formation.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_select_formation_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)
$ir = Join-Path $runRoot "select.ll"

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

function Get-ControlCount([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    return [regex]::Matches($Dump, '(?m)^\s+value #\d+ fn=#' + $functionId + '\s+kind=14\s+').Count
}

function Get-ActiveBranchCount([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    return [regex]::Matches($Dump, '(?m)^\s+block #\d+ fn=#' + $functionId + '\s+removed=0\s+.*\s+term=3\s+').Count
}

function Get-RemovedBlockCount([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    return [regex]::Matches($Dump, '(?m)^\s+block #\d+ fn=#' + $functionId + '\s+removed=1\s+').Count
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
        throw "two O2 select-formation runs produced different MIR dumps"
    }
    if ($o2Log -notmatch '(?m)^\[mir-analysis\] begin pass=select-formation required=1 preserved=0 ' -or
        $o2Log -notmatch 'select-formation candidates=1 skipped=0 idempotent=1') {
        throw "select formation audit evidence is incomplete"
    }
    if ((Get-ControlCount $o0Dump "choose") -ne 0) {
        throw "O0 choose unexpectedly contains a control-result value"
    }
    if ((Get-ControlCount $o2Dump "choose") -ne 1) {
        throw "O2 choose did not form exactly one control-result value"
    }
    if ((Get-RemovedBlockCount $o2Dump "choose") -lt 2) {
        throw "O2 choose did not tombstone both former if-expression arms"
    }
    if ((Get-ControlCount $o2Dump "call_arm_must_remain_branch") -ne 0 -or
        (Get-ActiveBranchCount $o2Dump "call_arm_must_remain_branch") -lt 1) {
        throw "effectful if-expression arms were incorrectly formed into a select"
    }

    $env:VYX_MIR_PASS_AUDIT = "0"
    Invoke-Checked -Name "emit_ir" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "-O2", "-o", $ir) -WorkingDirectory $repoRoot
    if (([IO.File]::ReadAllText($ir)) -notmatch '(?m)^\s*%[^=]+ = select i1 .*?, i32 .*?, i32 ') {
        throw "O2 LLVM IR is missing the scalar select instruction"
    }

    foreach ($level in @("-O0", "-O2")) {
        $name = $level.Substring(1).ToLowerInvariant() + "_jit"
        Invoke-Checked -Name $name -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--run=jit", $level) -WorkingDirectory $repoRoot
        if ((Read-Log $name).Trim() -cne "mir_select_formation OK") {
            throw "$level JIT output mismatch: $((Read-Log $name).Trim())"
        }
    }

    $aotExe = Join-Path $runRoot ("mir_select_formation" + $exeSuffix)
    Invoke-Checked -Name "o2_aot_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=exe", "-O2", "-o", $aotExe) -WorkingDirectory $repoRoot
    Invoke-Checked -Name "o2_aot_run" -FilePath $aotExe -WorkingDirectory $runRoot -MemoryLimitMB 512
    if ((Read-Log "o2_aot_run").Trim() -cne "mir_select_formation OK") {
        throw "O2 AOT output mismatch: $((Read-Log 'o2_aot_run').Trim())"
    }

    $cppOut = Join-Path $runRoot "o2_cpp"
    Invoke-Checked -Name "o2_cpp_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut) -WorkingDirectory $repoRoot
    $generatedCpp = (Get-ChildItem -LiteralPath (Join-Path $cppOut "src") -Filter "*.cpp" -File | ForEach-Object {
        Get-Content -Raw -LiteralPath $_.FullName
    }) -join "`n"
    if ($generatedCpp -notmatch '\?\s*\(') {
        throw "O2 MIR2CPP output is missing the control-result conditional expression"
    }
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    Invoke-Checked -Name "o2_cpp_configure" -FilePath $cmake `
        -Arguments @("--preset", "ninja-release") -WorkingDirectory $cppOut
    Invoke-Checked -Name "o2_cpp_build" -FilePath $cmake `
        -Arguments @("--build", "--preset", "ninja-release") -WorkingDirectory $cppOut
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_mir_select_formation" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "MIR2CPP build did not produce $cppExe"
    }
    Invoke-Checked -Name "o2_cpp_run" -FilePath $cppExe `
        -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
    if ((Read-Log "o2_cpp_run").Trim() -cne "mir_select_formation OK") {
        throw "O2 MIR2CPP output mismatch: $((Read-Log 'o2_cpp_run').Trim())"
    }

    Write-Host "mir_select_formation: OK"
    Write-Host "mir_select_formation evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

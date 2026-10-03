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
$source = Join-Path $testsRoot "cases/mir_cfg_simplify.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_cfg_simplify_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
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

function Get-FunctionBlocks([string]$Dump, [int]$FunctionId) {
    $pattern = '(?m)^\s+block #\d+ fn=#' + $FunctionId + ' .*?$'
    return @([regex]::Matches($Dump, $pattern) | ForEach-Object { $_.Value })
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
    if ($o0Dump -match '(?m)^\s+block #\d+ .* removed=1 ') {
        throw "O0 unexpectedly tombstoned a MIR block"
    }
    $removed = [regex]::Matches($o2Dump, '(?m)^\s+block #\d+ .* removed=1 ').Count
    if ($removed -le 0) { throw "O2 CFG simplify produced no removed block tombstones" }
    $o0Active = [regex]::Matches($o0Dump, '(?m)^\s+block #\d+ .* removed=0 ').Count
    $o2Active = [regex]::Matches($o2Dump, '(?m)^\s+block #\d+ .* removed=0 ').Count
    if ($o2Active -ge $o0Active) {
        throw "O2 did not reduce the active CFG: O0=$o0Active O2=$o2Active"
    }
    if ($o2Log -notmatch '(?m)^\[mir-analysis\] begin pass=cfg-simplify ' -or
        $o2Log -notmatch '(?m)^\[mir-analysis\] end pass=cfg-simplify changes=[1-9]\d* ' -or
        $o2Log -notmatch '(?m)^\[mir-analysis\] cfg-simplify function=\d+ .* idempotent=1\r?$') {
        throw "CFG simplify audit evidence is incomplete"
    }
    if ((Get-MirDump (Read-Log "o2_dump_b")) -cne $o2Dump) {
        throw "two O2 CFG simplify runs produced different MIR dumps"
    }

    $pureId = Get-FunctionId $o2Dump "pure_empty_branch"
    $impureId = Get-FunctionId $o2Dump "impure_empty_branch"
    $defaultId = Get-FunctionId $o2Dump "default_only"
    $pureBlocks = (Get-FunctionBlocks $o2Dump $pureId) -join "`n"
    $impureBlocks = (Get-FunctionBlocks $o2Dump $impureId) -join "`n"
    $defaultBlocks = (Get-FunctionBlocks $o2Dump $defaultId) -join "`n"
    if ($pureBlocks -match ' removed=0 .* term=3 ') {
        throw "pure same-target branch was not folded"
    }
    if ($impureBlocks -notmatch ' removed=0 .* term=3 .* target=#(\d+) false=#\1 ') {
        throw "impure same-target branch was incorrectly folded or not exposed"
    }
    if ($defaultBlocks -match ' removed=0 .* term=4 ') {
        throw "single default-only match was not folded"
    }

    $env:VYX_MIR_PASS_AUDIT = "0"
    foreach ($level in @("-O0", "-O2")) {
        $name = $level.Substring(1).ToLowerInvariant() + "_jit"
        Invoke-Compiler -Name $name -Arguments @("--src=file", $source, "--run=jit", $level)
        $output = (Read-Log $name).Trim()
        if ($output -cne "mir_cfg_simplify OK") {
            throw "$level JIT output mismatch: $output"
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
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_mir_cfg_simplify" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "MIR2CPP build did not produce $cppExe"
    }
    Invoke-External -Name "o2_cpp_run" -FilePath $cppExe `
        -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
    $cppOutput = (Read-Log "o2_cpp_run").Trim()
    if ($cppOutput -cne "mir_cfg_simplify OK") {
        throw "O2 MIR2CPP output mismatch: $cppOutput"
    }

    Write-Host "mir_cfg_simplify: OK"
    Write-Host "mir_cfg_simplify evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

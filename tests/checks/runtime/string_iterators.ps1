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
$source = Join-Path $testsRoot "cases/string_iterators.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/string_iterators_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

$originalPath = $env:Path
$pathSeparator = if ($windowsHost) { ";" } else { ":" }

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string[]]$Arguments = @(),
        [string]$FilePath = $BootstrapCompiler,
        [string]$WorkingDirectory = $repoRoot,
        [int]$MemoryLimitMB = 4096
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
        $stderr = [IO.File]::ReadAllText($invokeArgs.StderrLog)
        throw "$Name failed: exit=$($result.ExitCode) timeout=$($result.TimedOut) memory=$($result.MemoryExceeded)`n$stderr"
    }
    return $invokeArgs.StdoutLog
}

try {
    $env:Path = $runtimeDir + $pathSeparator + $originalPath
    foreach ($level in @("-O0", "-O2")) {
        $suffix = $level.Substring(1).ToLowerInvariant()
        $ir = Join-Path $runRoot ("string_iterators_" + $suffix + ".ll")
        Invoke-Checked -Name ("ir_" + $suffix) `
            -Arguments @("--src=file", $source, "--emit=ir", $level, "-o", $ir) | Out-Null
        if (-not (Test-Path -LiteralPath $ir -PathType Leaf)) {
            throw "$level did not produce LLVM IR"
        }

        foreach ($mode in @("aot", "jit")) {
            $output = Join-Path $runRoot ("string_iterators_" + $suffix + $exeSuffix)
            $stdout = Invoke-Checked -Name ($mode + "_" + $suffix) `
                -Arguments @("--src=file", $source, ("--run=" + $mode), $level, "-o", $output)
            $actual = [IO.File]::ReadAllText($stdout).Trim()
            if ($actual -cne "string_iterators OK") {
                throw "$level $mode output mismatch: $actual"
            }
        }
        Write-Host "string_iterators $level IR/AOT/JIT: OK"
    }

    $cppOut = Join-Path $runRoot "mir2cpp"
    Invoke-Checked -Name "mir2cpp_emit" `
        -Arguments @("--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut) | Out-Null
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    Invoke-Checked -Name "mir2cpp_configure" -FilePath $cmake `
        -WorkingDirectory $cppOut -Arguments @("--preset", "ninja-release") | Out-Null
    Invoke-Checked -Name "mir2cpp_build" -FilePath $cmake `
        -WorkingDirectory $cppOut -Arguments @("--build", "--preset", "ninja-release") | Out-Null
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_string_iterators" + $exeSuffix)
    $cppStdout = Invoke-Checked -Name "mir2cpp_run" -FilePath $cppExe `
        -WorkingDirectory $cppOut -Arguments @() -MemoryLimitMB 512
    $cppActual = [IO.File]::ReadAllText($cppStdout).Trim()
    if ($cppActual -cne "string_iterators OK") {
        throw "MIR2CPP output mismatch: $cppActual"
    }
    Write-Host "string_iterators MIR2CPP emit/configure/build/run: OK"
    Write-Host "string_iterators evidence=$runRoot"
} finally {
    $env:Path = $originalPath
}

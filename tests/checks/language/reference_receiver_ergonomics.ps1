[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [int]$TimeoutSec = 180
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 900) {
    throw "TimeoutSec must be between 1 and 900."
}

$testsRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\.." )).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testsRoot "..")).Path
. (Join-Path $repoRoot "bootstrap_compiler/scripts/VyxTestProcess.ps1")

$windowsHost = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    [bool]$IsWindows
} else {
    $env:OS -eq "Windows_NT"
}
$exeSuffix = if ($windowsHost) { ".exe" } else { "" }
$compilerName = if ($windowsHost) { "boot.exe" } else { "boot" }
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/" + $compilerName)
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$runtimeDir = Split-Path -Parent $BootstrapCompiler
$source = Join-Path $testsRoot "cases/language/references/reference_receiver_ergonomics.vyx"
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "fixture not found: $source"
}

$runRoot = Join-Path $repoRoot ("tests/.cache/reference_receiver_ergonomics_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)
$originalPath = $env:Path
$pathSeparator = if ($windowsHost) { ";" } else { ":" }

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $repoRoot
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
    # Deliberately omit MemoryLimitMB: this contract must observe the real
    # compiler/runtime behavior and must not install an artificial Job limit.
    $result = Invoke-VyxProcess @invokeArgs
    if ($result.ExitCode -ne 0 -or $result.TimedOut -or
        $result.MemoryExceeded -or $result.DialogCaught) {
        $stderr = if (Test-Path -LiteralPath $invokeArgs.StderrLog) {
            [IO.File]::ReadAllText($invokeArgs.StderrLog)
        } else { "" }
        throw "$Name failed: exit=$($result.ExitCode) timeout=$($result.TimedOut) memory=$($result.MemoryExceeded)`n$stderr"
    }
    return $invokeArgs.StdoutLog
}

function Assert-Output {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $text = [IO.File]::ReadAllText((Join-Path $runRoot ($Name + ".stdout.log")))
    if ($text.Replace("`r`n", "`n").Trim() -cne "reference receiver ergonomics OK") {
        throw "$Stage output mismatch: $text"
    }
}

try {
    $env:Path = $runtimeDir + $pathSeparator + $originalPath
    foreach ($mode in @("jit", "aot")) {
        $name = $mode + "_o2"
        $args = @("--src=file", $source, ("--run=" + $mode), "-O2")
        if ($mode -eq "aot") {
            $args += @("-o", (Join-Path $runRoot ("reference_receiver_ergonomics" + $exeSuffix)))
        }
        $stdout = Invoke-Checked -Name $name -FilePath $BootstrapCompiler -Arguments $args
        Assert-Output -Name $name -Stage ("O2 " + $mode.ToUpperInvariant())
        Write-Host ("reference_receiver_ergonomics O2 " + $mode + ": OK")
    }

    $cppOut = Join-Path $runRoot "mir2cpp"
    [void](Invoke-Checked -Name "mir2cpp_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut))
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    [void](Invoke-Checked -Name "mir2cpp_configure" -FilePath $cmake `
        -Arguments @("--preset", "ninja-release") -WorkingDirectory $cppOut)
    [void](Invoke-Checked -Name "mir2cpp_build" -FilePath $cmake `
        -Arguments @("--build", "--preset", "ninja-release") -WorkingDirectory $cppOut)
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_reference_receiver_ergonomics" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "MIR2CPP executable not found: $cppExe"
    }
    [void](Invoke-Checked -Name "mir2cpp_run" -FilePath $cppExe -WorkingDirectory $cppOut)
    Assert-Output -Name "mir2cpp_run" -Stage "O2 MIR2CPP"
    Write-Host "reference_receiver_ergonomics O2 MIR2CPP: OK"
    Write-Host "reference_receiver_ergonomics evidence=$runRoot"
} finally {
    $env:Path = $originalPath
}

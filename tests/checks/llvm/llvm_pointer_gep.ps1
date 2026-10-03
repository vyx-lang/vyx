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
$source = Join-Path $testsRoot "cases/llvm_pointer_gep.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/llvm_pointer_gep_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)
$ir = Join-Path $runRoot "pointer_gep.ll"
$wasm32Ir = Join-Path $runRoot "pointer_gep_wasm32.ll"
$armv7Ir = Join-Path $runRoot "pointer_gep_armv7.ll"
$exe = Join-Path $runRoot ("pointer_gep" + $exeSuffix)

$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH
$pathSeparator = if ($windowsHost) { ";" } else { ":" }

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
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
        throw "$Name failed: exit=$($result.ExitCode) timeout=$($result.TimedOut) memory=$($result.MemoryExceeded)"
    }
}

function Assert-ByteGepIndexWidth {
    param(
        [Parameter(Mandatory = $true)][string]$IrPath,
        [Parameter(Mandatory = $true)][int]$ExpectedBits,
        [Parameter(Mandatory = $true)][string]$TargetName
    )
    $irText = [IO.File]::ReadAllText($IrPath)
    $mainMatch = [regex]::Match(
        $irText,
        '(?ms)^define\s+i32\s+@main\([^\r\n]*\)[^{]*\{\r?\n(?<body>.*?)^\}\r?$')
    if (-not $mainMatch.Success) {
        throw "$TargetName IR is missing the main function body"
    }
    $mainBody = $mainMatch.Groups['body'].Value
    $pattern = '(?m)getelementptr\s+i8,\s+ptr\s+[^,]+,\s+i{0}\s+' -f $ExpectedBits
    $byteGeps = [regex]::Matches($mainBody, $pattern)
    if ($byteGeps.Count -ne 8) {
        throw "$TargetName expected eight i$ExpectedBits byte GEP pointer arithmetic operations in main, found $($byteGeps.Count)"
    }
    if ($mainBody -match '(?m)inttoptr\s+i(?:32|64)') {
        throw "$TargetName pointer arithmetic regressed to inttoptr"
    }
    if ($ExpectedBits -eq 32 -and $mainBody -notmatch '(?ms)trunc\s+i64\s+%[^\s,]+\s+to\s+i32\s*\r?\n\s*%[^\s=]+\s+=\s+getelementptr\s+i8,\s+ptr\s+[^,]+,\s+i32\s+') {
        throw "$TargetName is missing the i64-to-i32 byte GEP index truncation"
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

    Invoke-Checked -Name "emit_ir" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "-O0", "-o", $ir) `
        -WorkingDirectory $repoRoot
    $hostIrText = [IO.File]::ReadAllText($ir)
    $hostMainMatch = [regex]::Match(
        $hostIrText,
        '(?ms)^define\s+i32\s+@main\([^\r\n]*\)[^{]*\{\r?\n(?<body>.*?)^\}\r?$')
    if (-not $hostMainMatch.Success) { throw "host IR is missing the main function body" }
    $hostMainBody = $hostMainMatch.Groups['body'].Value
    $hostByteGeps = [regex]::Matches(
        $hostMainBody,
        '(?m)getelementptr\s+i8,\s+ptr\s+[^,]+,\s+i(?:32|64)\s+')
    if ($hostByteGeps.Count -ne 8) {
        throw "host expected eight i32/i64 byte GEP pointer arithmetic operations in main, found $($hostByteGeps.Count)"
    }
    if ($hostMainBody -match '(?m)inttoptr\s+i(?:32|64)') {
        throw "host pointer arithmetic regressed to inttoptr"
    }
    if ($hostMainBody -notmatch '(?ms)sext\s+i32\s+%[^\s,]+\s+to\s+i64\s*\r?\n\s*%[^\s=]+\s+=\s+getelementptr\s+i8,\s+ptr\s+[^,]+,\s+i64\s+') {
        throw "host is missing the signed i32-to-i64 byte GEP index extension"
    }
    if ($hostMainBody -notmatch '(?ms)zext\s+i32\s+%[^\s,]+\s+to\s+i64\s*\r?\n\s*%[^\s=]+\s+=\s+getelementptr\s+i8,\s+ptr\s+[^,]+,\s+i64\s+') {
        throw "host is missing the unsigned i32-to-i64 byte GEP index extension"
    }

    Invoke-Checked -Name "emit_ir_wasm32" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "--target=wasm32-unknown-unknown", "-O0", "-o", $wasm32Ir) `
        -WorkingDirectory $repoRoot
    Assert-ByteGepIndexWidth -IrPath $wasm32Ir -ExpectedBits 32 -TargetName "wasm32"

    Invoke-Checked -Name "emit_ir_armv7" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "--target=armv7-unknown-linux-gnueabihf", "-O0", "-o", $armv7Ir) `
        -WorkingDirectory $repoRoot
    Assert-ByteGepIndexWidth -IrPath $armv7Ir -ExpectedBits 32 -TargetName "armv7"

    Invoke-Checked -Name "compile" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=exe", "-O0", "-o", $exe) `
        -WorkingDirectory $repoRoot
    Invoke-Checked -Name "run" -FilePath $exe -WorkingDirectory $runRoot -MemoryLimitMB 512
    $output = [IO.File]::ReadAllText((Join-Path $runRoot "run.stdout.log")).Trim()
    if ($output -cne "llvm_pointer_gep OK") {
        throw "execution output mismatch: $output"
    }

    Write-Host "llvm_pointer_gep: OK"
    Write-Host "llvm_pointer_gep evidence=$runRoot"
} finally {
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

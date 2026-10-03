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
$source = Join-Path $testsRoot "cases/box_stack_promotion.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/box_stack_promotion_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

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

function Get-IrFunctionBody([string]$Text, [string]$SourceName) {
    $mangled = $SourceName.Replace("_", "_5F")
    $match = [regex]::Match($Text,
        '(?m)^define\b[^\r\n]*@[^\r\n]*' + [regex]::Escape($mangled) + '[^\r\n]*\{')
    if (-not $match.Success) { throw "LLVM IR is missing function $SourceName" }
    $openBrace = $match.Index + $match.Length - 1
    $depth = 0
    for ($index = $openBrace; $index -lt $Text.Length; $index++) {
        if ($Text[$index] -eq '{') { $depth++ }
        elseif ($Text[$index] -eq '}') {
            $depth--
            if ($depth -eq 0) { return $Text.Substring($openBrace, $index - $openBrace + 1) }
        }
    }
    throw "LLVM IR function $SourceName has an unclosed body"
}

function Assert-StackPromotion([string]$IrText) {
    # Under the value-class ABI a `Box<T>` wrapper is a struct value type
    # (C++ unique_ptr semantics): the wrapper travels by value and never goes
    # through the class-object allocator. Only its owned payload lives on the
    # heap. A non-escaping Box therefore has no allocation of any kind, and an
    # escaping (returned) Box allocates its payload but returns the wrapper by
    # value with no `vyx_class_alloc_abi`.
    $local = Get-IrFunctionBody $IrText "nonescaping_box_sum"
    if ($local -match '(?m)\bcall\b[^\r\n]*@(malloc|free|vyx_class_alloc_abi|vyx_aligned_alloc_abi|vyx_aligned_free_abi)\b') {
        throw "nonescaping_box_sum still performs heap or class-wrapper allocation"
    }

    $returned = Get-IrFunctionBody $IrText "returned_box"
    if ($returned -notmatch '(?m)\bcall\b[^\r\n]*@(malloc|vyx_aligned_alloc_abi)\b') {
        throw "returned_box incorrectly lost its required heap payload allocation"
    }
    if ($returned -match '(?m)\bcall\b[^\r\n]*@vyx_class_alloc_abi\b') {
        throw "returned_box allocated its Box wrapper via vyx_class_alloc_abi; the value-class ABI must keep the wrapper a by-value struct"
    }
    if ($returned -notmatch '(?m)^\s*ret\s+%mir\.struct\.Box\b') {
        throw "returned_box must return its Box wrapper by value (%mir.struct.Box)"
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

    $ir = Join-Path $runRoot "box_stack_promotion_o2.ll"
    Invoke-Checked -Name "o2_emit_ir" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "-O2", "-o", $ir) `
        -WorkingDirectory $repoRoot
    Assert-StackPromotion ([IO.File]::ReadAllText($ir))

    $exe = Join-Path $runRoot ("box_stack_promotion" + $exeSuffix)
    Invoke-Checked -Name "o2_emit_exe" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=exe", "-O2", "-o", $exe) `
        -WorkingDirectory $repoRoot
    Invoke-Checked -Name "o2_run" -FilePath $exe -WorkingDirectory $runRoot -MemoryLimitMB 512

    Write-Host "box_stack_promotion: OK"
    Write-Host "box_stack_promotion evidence=$runRoot"
} finally {
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

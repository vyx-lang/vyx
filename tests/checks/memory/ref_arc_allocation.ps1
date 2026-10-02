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
$source = Join-Path $testsRoot "cases/ref_arc_allocation.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/ref_arc_allocation_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
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
    $result = Invoke-VyxProcess -FilePath $FilePath -ArgumentList $Arguments `
        -WorkingDirectory $WorkingDirectory `
        -StdoutLog (Join-Path $runRoot ($Name + ".stdout.log")) `
        -StderrLog (Join-Path $runRoot ($Name + ".stderr.log")) `
        -DialogLog (Join-Path $runRoot ($Name + ".dialog.log")) `
        -TimeoutSec $TimeoutSec -MemoryLimitMB $(if ($windowsHost) { $MemoryLimitMB } else { 0 })
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

function Assert-ArcLowering([string]$IrText) {
    $hot = Get-IrFunctionBody $IrText "arc_hot"
    if ($hot -notmatch '(?m)\bcall\b[^\r\n]*@vyx_ref_alloc_abi\b') {
        throw "arc_hot is not using the combined Ref allocation ABI"
    }
    if ($IrText -notmatch '(?m)\bcall\b[^\r\n]*@vyx_ref_free_abi\b') {
        throw "Ref control blocks are not released through the paired runtime ABI"
    }
    if ($hot -match '(?m)\bcall\b[^\r\n]*@(vyx_atomic_load_i64|vyx_atomic_fetch_add_i64)\b') {
        throw "arc_hot still calls runtime atomic helpers instead of LLVM atomics"
    }
    if ($hot -notmatch '(?m)\batomicrmw\s+add\b' -or $hot -notmatch '(?m)\bload\s+atomic\b') {
        throw "arc_hot is missing seq_cst LLVM atomic operations"
    }
    if ($hot -match '(?m)\bcall\b[^\r\n]*@vyx_class_alloc_abi\b') {
        throw "arc_hot still heap-allocates nonescaping Ref wrappers"
    }
}

try {
    $env:Path = $runtimeDir + $pathSeparator + $originalPath
    if (-not $windowsHost) { $env:LD_LIBRARY_PATH = $runtimeDir + $pathSeparator + $originalLdLibraryPath }
    $ir = Join-Path $runRoot "ref_arc_allocation_o2.ll"
    Invoke-Checked -Name "o2_emit_ir" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "-O2", "-o", $ir) `
        -WorkingDirectory $repoRoot
    Assert-ArcLowering ([IO.File]::ReadAllText($ir))

    $exe = Join-Path $runRoot ("ref_arc_allocation" + $exeSuffix)
    Invoke-Checked -Name "o2_emit_exe" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=exe", "-O2", "-o", $exe) `
        -WorkingDirectory $repoRoot
    Invoke-Checked -Name "o2_run" -FilePath $exe -WorkingDirectory $runRoot -MemoryLimitMB 512

    $explicitRuntimeExe = Join-Path $runRoot ("ref_arc_allocation_vyx_runtime" + $exeSuffix)
    Invoke-Checked -Name "o2_emit_vyx_runtime_exe" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=exe", "-O2", "-o", $explicitRuntimeExe,
            "-L", $runtimeDir, "-l", "vyx_runtime") `
        -WorkingDirectory $repoRoot
    Invoke-Checked -Name "o2_run_vyx_runtime" -FilePath $explicitRuntimeExe `
        -WorkingDirectory $runRoot -MemoryLimitMB 512
    Write-Host "ref_arc_allocation: OK"
    Write-Host "ref_arc_allocation evidence=$runRoot"
} finally {
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

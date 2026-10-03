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
$source = Join-Path $testsRoot "cases/string_pool_lifecycle.vyx"
$autoDropSource = Join-Path $testsRoot "cases/string_auto_drop_ownership.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/string_pool_ownership_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
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

try {
    $env:Path = $runtimeDir + $pathSeparator + $originalPath
    if (-not $windowsHost) {
        $env:LD_LIBRARY_PATH = $runtimeDir + $pathSeparator + $originalLdLibraryPath
        $env:DYLD_LIBRARY_PATH = $runtimeDir + $pathSeparator + $originalDyldLibraryPath
    }

    foreach ($level in @("-O0", "-O2")) {
        $suffix = $level.Substring(1).ToLowerInvariant()
        $ir = Join-Path $runRoot ("string_pool_ownership_" + $suffix + ".ll")
        Invoke-Checked -Name ("emit_ir_" + $suffix) -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--emit=ir", $level, "-o", $ir) `
            -WorkingDirectory $repoRoot
        $irText = [IO.File]::ReadAllText($ir)
        if ($irText -notmatch '(?m)\bcall\b[^\r\n]*@vyx_string_alloc_abi\s*\(') {
            throw "$level IR does not allocate compiler-owned temporary strings through the pool ABI"
        }
        if ($irText -notmatch '(?m)\bcall\b[^\r\n]*@vyx_free_pooled_string\s*\(') {
            throw "$level IR does not release pooled temporary strings through the matching ABI"
        }

        $exe = Join-Path $runRoot ("string_pool_ownership_" + $suffix + $exeSuffix)
        Invoke-Checked -Name ("emit_exe_" + $suffix) -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--emit=exe", $level, "-o", $exe) `
            -WorkingDirectory $repoRoot
        Invoke-Checked -Name ("run_" + $suffix) -FilePath $exe `
            -WorkingDirectory $runRoot -MemoryLimitMB 512
    }

    $autoDropIr = Join-Path $runRoot "string_auto_drop_o0.ll"
    Invoke-Checked -Name "auto_drop_emit_ir_o0" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $autoDropSource, "--emit=ir", "-O0", "-o", $autoDropIr) `
        -WorkingDirectory $repoRoot
    $autoDropText = [IO.File]::ReadAllText($autoDropIr)
    $dropPattern = '(?m)\bcall\b[^\r\n]*@__vyx_F_str_5Fdrop_R_void_P_str\s*\('
    $scalarBody = Get-IrFunctionBody $autoDropText "scalar_read_loop"
    $transformBody = Get-IrFunctionBody $autoDropText "fresh_transform_loop"
    $assignmentBody = Get-IrFunctionBody $autoDropText "assignment_copy_loop"
    $aliasBody = Get-IrFunctionBody $autoDropText "direct_alias_stays_borrowed"
    $scalarDrops = [regex]::Matches($scalarBody, $dropPattern).Count
    $transformDrops = [regex]::Matches($transformBody, $dropPattern).Count
    $assignmentDrops = [regex]::Matches($assignmentBody, $dropPattern).Count
    $aliasDrops = [regex]::Matches($aliasBody, $dropPattern).Count
    if ($scalarDrops -ne 1) {
        throw "scalar_read_loop must drop its one owning interpolation local; calls=$scalarDrops"
    }
    if ($transformDrops -ne 3) {
        throw "fresh_transform_loop must drop raw/upper/lower; calls=$transformDrops"
    }
    if ($assignmentDrops -ne 3) {
        throw "assignment_copy_loop must drop each piece plus the destination on both return paths; calls=$assignmentDrops"
    }
    if ($aliasDrops -ne 1) {
        throw "direct_alias_stays_borrowed must retain only its explicit owner drop; calls=$aliasDrops"
    }
    foreach ($level in @("-O0", "-O2")) {
        $suffix = $level.Substring(1).ToLowerInvariant()
        $autoDropExe = Join-Path $runRoot ("string_auto_drop_" + $suffix + $exeSuffix)
        Invoke-Checked -Name ("auto_drop_emit_exe_" + $suffix) -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $autoDropSource, "--emit=exe", $level, "-o", $autoDropExe) `
            -WorkingDirectory $repoRoot
        Invoke-Checked -Name ("auto_drop_run_" + $suffix) -FilePath $autoDropExe `
            -WorkingDirectory $runRoot -MemoryLimitMB 512
    }

    Write-Host "string_pool_ownership O0/O2 IR+AOT and lexical auto-drop: OK"
    Write-Host "string_pool_ownership evidence=$runRoot"
} finally {
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

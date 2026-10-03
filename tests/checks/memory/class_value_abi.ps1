[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [int]$TimeoutSec = 120,
    [switch]$SkipMir2Cpp
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
$source = Join-Path $testsRoot "cases/class_value_abi.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/class_value_abi_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH
$pathSeparator = if ($windowsHost) { ";" } else { ":" }

# Every default-class value function uses automatic storage; only the explicit
# `new` function keeps an owning heap lifetime.
$ValueFunctions = @(
    "temp_literal_method",
    "local_literal_method",
    "make_value",
    "named_return",
    "cond_ctor",
    "match_ctor",
    "loop_ctor",
    "nested_mixed",
    "generic_ctor",
    "uninit_then_init"
)

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [int]$MemoryLimitMB = 0
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

function Get-IrFunctionBody {
    param(
        [Parameter(Mandatory = $true)][string]$IrText,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )

    # Vyx LLVM symbols escape source underscores as `_5F`.
    $mangledName = $FunctionName.Replace("_", "_5F")
    $escapedName = [regex]::Escape($mangledName)
    $pattern = '(?ms)^define\b[^\r\n]*@(?<symbol>[^\s(]*' + $escapedName + '[^\s(]*)\([^)]*\)[^{]*\{\r?\n(?<body>.*?)^\}'
    $matches = [regex]::Matches($IrText, $pattern)
    if ($matches.Count -ne 1) {
        throw "expected exactly one LLVM definition for $FunctionName, found $($matches.Count)"
    }
    return $matches[0].Groups["body"].Value
}

function Test-BodyAllocatesClass {
    param(
        [Parameter(Mandatory = $true)][string]$Body
    )
    return ($Body -match '(?m)\bcall\b[^\r\n]*@vyx_class_alloc_abi\s*\(') -or
           ($Body -match '(?m)\bcall\b[^\r\n]*@(malloc|vyx_aligned_alloc_abi)\s*\(')
}

function Assert-ValueClassStorage {
    param(
        [Parameter(Mandatory = $true)][string]$IrPath,
        [Parameter(Mandatory = $true)][string]$Optimization
    )

    $irText = [IO.File]::ReadAllText($IrPath)

    foreach ($fn in $ValueFunctions) {
        $body = Get-IrFunctionBody -IrText $irText -FunctionName $fn
        if (Test-BodyAllocatesClass -Body $body) {
            throw "$Optimization $fn allocates a default class value on the heap instead of automatic storage"
        }
    }

    # Positive heap control: explicit `new` keeps its owning pointer lifetime.
    $heap = Get-IrFunctionBody -IrText $irText -FunctionName "heap_new"
    if (-not (Test-BodyAllocatesClass -Body $heap)) {
        throw "$Optimization heap_new unexpectedly avoided the heap ABI for an explicit new allocation"
    }
}

function Assert-ProgramOutput {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    $output = [IO.File]::ReadAllText((Join-Path $runRoot ($Name + ".stdout.log"))).Replace("`r`n", "`n").Trim()
    if ($output -cne "class_value_abi OK") {
        throw "$Stage output mismatch: $output"
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

    foreach ($level in @("-O0", "-O2")) {
        $stem = $level.Substring(1).ToLowerInvariant()
        $ir = Join-Path $runRoot ("class_value_abi_" + $stem + ".ll")
        Invoke-Checked -Name ($stem + "_emit_ir") -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--emit=ir", $level, "-o", $ir) `
            -WorkingDirectory $repoRoot
        Assert-ValueClassStorage -IrPath $ir -Optimization $level

        $jitName = $stem + "_jit"
        Invoke-Checked -Name $jitName -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--run=jit", $level) `
            -WorkingDirectory $repoRoot
        Assert-ProgramOutput -Name $jitName -Stage ($level + " JIT")

        $exe = Join-Path $runRoot ("class_value_abi_" + $stem + $exeSuffix)
        $aotEmitName = $stem + "_aot_emit"
        Invoke-Checked -Name $aotEmitName -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--emit=exe", $level, "-o", $exe) `
            -WorkingDirectory $repoRoot
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            throw "$level AOT did not produce $exe"
        }
        $aotRunName = $stem + "_aot_run"
        Invoke-Checked -Name $aotRunName -FilePath $exe -WorkingDirectory $runRoot -MemoryLimitMB 0
        Assert-ProgramOutput -Name $aotRunName -Stage ($level + " AOT")
    }

    if (-not $SkipMir2Cpp) {
        $cppOut = Join-Path $runRoot "o2_cpp"
        Invoke-Checked -Name "o2_cpp_emit" -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut) `
            -WorkingDirectory $repoRoot
        $cmake = (Get-Command cmake -ErrorAction Stop).Source
        Invoke-Checked -Name "o2_cpp_configure" -FilePath $cmake `
            -Arguments @("--preset", "ninja-release") -WorkingDirectory $cppOut
        Invoke-Checked -Name "o2_cpp_build" -FilePath $cmake `
            -Arguments @("--build", "--preset", "ninja-release") -WorkingDirectory $cppOut
        $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_class_value_abi" + $exeSuffix)
        if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
            throw "MIR2CPP build did not produce $cppExe"
        }
        Invoke-Checked -Name "o2_cpp_run" -FilePath $cppExe `
            -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 0
        Assert-ProgramOutput -Name "o2_cpp_run" -Stage "O2 MIR2CPP"
    }

    Write-Host "class_value_abi: OK"
    Write-Host "class_value_abi evidence=$runRoot"
} finally {
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

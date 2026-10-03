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
$source = Join-Path $testsRoot "cases/llvm_lifetime_intrinsics.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/llvm_lifetime_intrinsics_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
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

function Read-Log([string]$Name) {
    return [IO.File]::ReadAllText((Join-Path $runRoot ($Name + ".stdout.log")))
}

function Get-IrFunctionBody {
    param(
        [Parameter(Mandatory = $true)][string]$IrText,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )

    $mangledName = $FunctionName.Replace("_", "_5F")
    $escapedName = [regex]::Escape($mangledName)
    $pattern = '(?ms)^define\b[^\r\n]*@(?<symbol>[^\s(]*' + $escapedName + '[^\s(]*)\([^)]*\)[^{]*\{\r?\n(?<body>.*?)^\}'
    $matches = [regex]::Matches($IrText, $pattern)
    if ($matches.Count -ne 1) {
        throw "expected exactly one LLVM definition for $FunctionName, found $($matches.Count)"
    }
    return $matches[0].Groups["body"].Value
}

function Get-LifetimeCalls([string]$Body) {
    $pattern = '(?m)\bcall\b[^\r\n]*@llvm\.lifetime\.(?<kind>start|end)[^\r\n]*\(ptr (?<pointer>%[-A-Za-z0-9._]+)\)'
    $calls = @()
    foreach ($match in [regex]::Matches($Body, $pattern)) {
        $calls += [pscustomobject]@{
            Kind = $match.Groups["kind"].Value
            Pointer = $match.Groups["pointer"].Value
        }
    }
    return $calls
}

function Assert-DirectI32Alloca([string]$Body, [string]$Pointer, [string]$Label) {
    $pattern = '(?m)^\s*' + [regex]::Escape($Pointer) + '\s*=\s*alloca\s+i32\b'
    if (-not [regex]::IsMatch($Body, $pattern)) {
        throw "$Label lifetime pointer $Pointer is not a direct i32 alloca"
    }
}

function Assert-SequentialSlotReuse([string]$IrText) {
    $body = Get-IrFunctionBody -IrText $IrText -FunctionName "sequential_reuse"
    $calls = @(Get-LifetimeCalls $body)
    $kinds = @($calls | ForEach-Object { $_.Kind }) -join ","
    if ($calls.Count -ne 6 -or $kinds -cne "start,start,end,start,end,end") {
        throw "sequential_reuse lifetime sequence mismatch: count=$($calls.Count) kinds=$kinds"
    }
    $result = $calls[0].Pointer
    $shared = $calls[1].Pointer
    if ($result -ceq $shared -or
        $calls[2].Pointer -cne $shared -or
        $calls[3].Pointer -cne $shared -or
        $calls[4].Pointer -cne $shared -or
        $calls[5].Pointer -cne $result) {
        throw "sequential_reuse did not preserve two disjoint lifetime regions on one shared slot"
    }
    Assert-DirectI32Alloca $body $result "sequential_reuse result"
    Assert-DirectI32Alloca $body $shared "sequential_reuse first/second"
    if ((@($calls | ForEach-Object { $_.Pointer } | Select-Object -Unique)).Count -ne 2) {
        throw "sequential_reuse lifetime markers reference more than result plus one shared i32 alloca"
    }
}

function Assert-OverlapSlotsStayDistinct([string]$IrText) {
    $body = Get-IrFunctionBody -IrText $IrText -FunctionName "overlap_guard"
    $calls = @(Get-LifetimeCalls $body)
    $kinds = @($calls | ForEach-Object { $_.Kind }) -join ","
    if ($calls.Count -ne 6 -or $kinds -cne "start,start,start,end,end,end") {
        throw "overlap_guard lifetime sequence mismatch: count=$($calls.Count) kinds=$kinds"
    }
    $result = $calls[0].Pointer
    $outer = $calls[1].Pointer
    $inner = $calls[2].Pointer
    if ($result -ceq $outer -or $result -ceq $inner -or $outer -ceq $inner -or
        $calls[3].Pointer -cne $inner -or
        $calls[4].Pointer -cne $outer -or
        $calls[5].Pointer -cne $result) {
        throw "overlap_guard incorrectly coalesced overlapping local lifetimes"
    }
    Assert-DirectI32Alloca $body $outer "overlap_guard outer"
    Assert-DirectI32Alloca $body $inner "overlap_guard inner"
}

function Assert-LifetimeMarkers([string]$IrPath) {
    $irText = [IO.File]::ReadAllText($IrPath)
    $direct = Get-IrFunctionBody -IrText $irText -FunctionName "direct_scalar"
    $startMatch = [regex]::Match($direct, '(?m)\bcall\b[^\r\n]*@llvm\.lifetime\.start[^\r\n]*')
    $endMatch = [regex]::Match($direct, '(?m)\bcall\b[^\r\n]*@llvm\.lifetime\.end[^\r\n]*')
    if (-not $startMatch.Success -or -not $endMatch.Success) {
        throw "direct_scalar is missing a paired LLVM lifetime marker"
    }
    $start = $startMatch.Index
    $end = $endMatch.Index
    if ($start -ge $end) {
        throw "direct_scalar lifetime.start must precede lifetime.end"
    }
    if ($direct -notmatch '(?m)\bret\s+i32\b') {
        throw "direct_scalar is missing its scalar return"
    }

    $branch = Get-IrFunctionBody -IrText $irText -FunctionName "branch_excluded"
    if ($branch -match '(?m)\bcall\b[^\r\n]*@llvm\.lifetime\.(start|end)') {
        throw "branch_excluded received a lifetime marker outside the single-block proof slice"
    }

    Assert-SequentialSlotReuse $irText
    Assert-OverlapSlotsStayDistinct $irText
}

function Assert-ProgramOutput([string]$Name, [string]$Stage) {
    $output = (Read-Log $Name).Replace("`r`n", "`n").Trim()
    if ($output -cne "llvm_lifetime_intrinsics OK") {
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

    $ir = Join-Path $runRoot "llvm_lifetime_intrinsics_o0.ll"
    Invoke-Checked -Name "o0_emit_ir" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "-O0", "-o", $ir) `
        -WorkingDirectory $repoRoot
    Assert-LifetimeMarkers $ir

    foreach ($level in @( "-O0", "-O2" )) {
        $name = $level.Substring(1).ToLowerInvariant() + "_jit"
        Invoke-Checked -Name $name -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--run=jit", $level) -WorkingDirectory $repoRoot
        Assert-ProgramOutput $name ($level + " JIT")
    }

    $aotExe = Join-Path $runRoot ("llvm_lifetime_intrinsics" + $exeSuffix)
    Invoke-Checked -Name "o2_aot_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=exe", "-O2", "-o", $aotExe) -WorkingDirectory $repoRoot
    Invoke-Checked -Name "o2_aot_run" -FilePath $aotExe -WorkingDirectory $runRoot -MemoryLimitMB 512
    Assert-ProgramOutput "o2_aot_run" "O2 AOT"

    Write-Host "llvm_lifetime_intrinsics: OK"
    Write-Host "llvm_lifetime_intrinsics evidence=$runRoot"
} finally {
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

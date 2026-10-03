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
$source = Join-Path $testsRoot "cases/mir_storage_lifetime.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_storage_lifetime_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

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

function Assert-StorageDeadMinimum([string]$Dump, [string]$FunctionName, [string]$LocalName, [int]$Minimum) {
    # P4 reserves MIR instruction kind 9 for StorageDead.  Each path-specific
    # scope exit must keep a concrete instruction rather than a synthesized drop.
    $functionId = Get-FunctionId $Dump $FunctionName
    $pattern = '(?m)^\s+instr #\d+ fn=#' + $functionId +
        '\s+block=#\d+\s+kind=9\s+.*\sname=' + [regex]::Escape($LocalName) + '$'
    $actual = [regex]::Matches($Dump, $pattern).Count
    if ($actual -lt $Minimum) {
        throw "$($FunctionName)::$($LocalName) expected at least $Minimum StorageDead instruction(s) (kind=9), found $actual"
    }
    return $actual
}

function Get-FunctionInstructions([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    $items = @()
    foreach ($line in $Dump -split "`n") {
        $match = [regex]::Match(
            $line,
            '^\s+instr #(\d+) fn=#(\d+) block=#(\d+) kind=(\d+) .* place=#(\d+) .* name=(.*)$')
        if (-not $match.Success -or [int]$match.Groups[2].Value -ne $functionId) { continue }
        $items += [pscustomobject]@{
            Id = [int]$match.Groups[1].Value
            Block = [int]$match.Groups[3].Value
            Kind = [int]$match.Groups[4].Value
            Place = [int]$match.Groups[5].Value
            Name = $match.Groups[6].Value
        }
    }
    return $items
}

function Get-GeneratedCppFunction([string]$Text, [string]$SourceName) {
    $pattern = '(?m)^\s*(?:[A-Za-z_][A-Za-z0-9_:<>]*\s+)+' +
        'vyx_fn_' + [regex]::Escape($SourceName) + '_\d+\s*\([^;{}]*\)\s*\{'
    $match = [regex]::Match($Text, $pattern)
    if (-not $match.Success) { throw "Generated C++ is missing function $SourceName" }
    $openBrace = $match.Index + $match.Length - 1
    $depth = 0
    for ($index = $openBrace; $index -lt $Text.Length; $index++) {
        if ($Text[$index] -eq '{') { $depth++ }
        elseif ($Text[$index] -eq '}') {
            $depth--
            if ($depth -eq 0) {
                return [pscustomobject]@{
                    Name = $Text.Substring($match.Index, $match.Length - 1).Trim()
                    Body = $Text.Substring($openBrace, $index - $openBrace + 1)
                }
            }
        }
    }
    throw "Generated C++ function $SourceName has an unclosed body"
}

function Get-IrFunctionBody([string]$Text, [string]$SourceName) {
    $match = [regex]::Match($Text, '(?m)^define\b[^\r\n]*@[^\r\n]*' + [regex]::Escape($SourceName) + '[^\r\n]*\{')
    if (-not $match.Success) { throw "LLVM IR is missing function containing $SourceName" }
    $openBrace = $match.Index + $match.Length - 1
    $depth = 0
    for ($index = $openBrace; $index -lt $Text.Length; $index++) {
        if ($Text[$index] -eq '{') { $depth++ }
        elseif ($Text[$index] -eq '}') {
            $depth--
            if ($depth -eq 0) { return $Text.Substring($openBrace, $index - $openBrace + 1) }
        }
    }
    throw "LLVM IR function containing $SourceName has an unclosed body"
}

function Assert-AsyncLifetimeEndAfterAwait([string]$IrText) {
    $body = Get-IrFunctionBody $IrText "async_5Fsuspend_5Fscope"
    $await = $body.IndexOf("await_5F5Fvalue", [StringComparison]::Ordinal)
    if ($await -lt 0) { throw "async_suspend_scope IR is missing the await call" }
    $lifetimeEnd = $body.IndexOf("llvm.lifetime.end", [StringComparison]::Ordinal)
    if ($lifetimeEnd -ge 0 -and $lifetimeEnd -lt $await) {
        throw "async_suspend_scope ends an LLVM lifetime before await resumes"
    }
}

function Get-GeneratedCppPrelude([string]$Body, [string]$SourceName) {
    $goto = [regex]::Match($Body, '(?m)^\s*goto bb_\d+;')
    if (-not $goto.Success) { throw "Generated C++ function $SourceName has no entry goto" }
    return $Body.Substring(0, $goto.Index)
}

function Assert-GeneratedCppPreludeLocal([string]$Text, [string]$FunctionName, [string]$LocalName) {
    $function = Get-GeneratedCppFunction $Text $FunctionName
    $prelude = Get-GeneratedCppPrelude $function.Body $FunctionName
    $pattern = '\bvyx_local_' + [regex]::Escape($LocalName) + '_\d+\b'
    if (-not [regex]::IsMatch($prelude, $pattern)) {
        throw "Generated C++ unexpectedly narrowed $FunctionName::$LocalName"
    }
}

function Assert-GeneratedCppScopedScalar([string]$Text) {
    $function = Get-GeneratedCppFunction $Text "straight_line_scoped_scalar"
    $prelude = Get-GeneratedCppPrelude $function.Body "straight_line_scoped_scalar"
    if ([regex]::IsMatch($prelude, '\bvyx_local_scoped_temp_\d+\b')) {
        throw "Generated C++ still predeclares straight_line_scoped_scalar::scoped_temp"
    }
    if (-not [regex]::IsMatch($prelude, '\bvyx_local_result_\d+\b')) {
        throw "Generated C++ unexpectedly narrowed outer result storage"
    }
    $scopePattern = '(?s)\{\s*(?:int32_t|int64_t)\s+(?<local>vyx_local_scoped_temp_\d+)\s*=\s*(?:int32_t|int64_t)\{\};\s*/\*\s*storage_live\s+scoped_temp\s*\*/.*?/\*\s*storage_dead\s+scoped_temp\s*\*/\s*\}'
    $scope = [regex]::Match($function.Body, $scopePattern)
    if (-not $scope.Success) {
        throw "Generated C++ is missing the scoped StorageLive/StorageDead region for scoped_temp"
    }
    $localPattern = '\b' + [regex]::Escape($scope.Groups['local'].Value) + '\b'
    $before = $function.Body.Substring(0, $scope.Index)
    $after = $function.Body.Substring($scope.Index + $scope.Length)
    if ([regex]::IsMatch($before, $localPattern) -or [regex]::IsMatch($after, $localPattern)) {
        throw "Generated C++ uses scoped_temp outside its narrowed lexical region"
    }
}

function Assert-DropBeforeStorageDead([string]$Dump, [string]$FunctionName, [string]$LocalName) {
    $instructions = Get-FunctionInstructions $Dump $FunctionName
    $dead = @($instructions | Where-Object { $_.Kind -eq 9 -and $_.Name -ceq $LocalName })
    if ($dead.Count -eq 0) { throw "$FunctionName::$LocalName has no StorageDead instruction" }
    foreach ($deadInstr in $dead) {
        $drop = @($instructions | Where-Object {
            $_.Kind -eq 8 -and $_.Block -eq $deadInstr.Block -and
            $_.Place -eq $deadInstr.Place -and $_.Id -lt $deadInstr.Id
        })
        if ($drop.Count -eq 0) {
            throw "$FunctionName::$LocalName StorageDead #$($deadInstr.Id) is not preceded by Drop in block #$($deadInstr.Block)"
        }
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
    $env:VYX_BOOTSTRAP_PROFILE = "0"
    $env:VYX_MIR_PASS_AUDIT = "1"

    foreach ($run in @(
        @{ Name = "o0_dump"; Args = @("--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O0") },
        @{ Name = "o2_dump_a"; Args = @("--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2") },
        @{ Name = "o2_dump_b"; Args = @("--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2") }
    )) {
        Invoke-Checked -Name $run.Name -FilePath $BootstrapCompiler -Arguments $run.Args -WorkingDirectory $repoRoot
    }

    $o0Dump = Get-MirDump (Read-Log "o0_dump")
    $o2Dump = Get-MirDump (Read-Log "o2_dump_a")
    if ((Get-MirDump (Read-Log "o2_dump_b")) -cne $o2Dump) {
        throw "two O2 StorageDead runs produced different MIR dumps"
    }

    # scalar_scope proves scalar locals receive scope-end markers. nested_owned
    # and early_return cover owned drops before the marker on normal and return
    # exits. loop_cleanup requires markers on continue, break, and backedge exits.
    foreach ($dump in @($o0Dump, $o2Dump)) {
        Assert-StorageDeadMinimum $dump "scalar_scope" "scalar" 1 | Out-Null
        Assert-StorageDeadMinimum $dump "scalar_scope" "nested_scalar" 1 | Out-Null
        Assert-StorageDeadMinimum $dump "nested_owned" "outer" 1 | Out-Null
        Assert-StorageDeadMinimum $dump "nested_owned" "inner" 1 | Out-Null
        Assert-StorageDeadMinimum $dump "early_return" "outer" 1 | Out-Null
        Assert-StorageDeadMinimum $dump "early_return" "inner" 1 | Out-Null
        Assert-StorageDeadMinimum $dump "loop_cleanup" "iteration" 3 | Out-Null
        Assert-StorageDeadMinimum $dump "for_init_scope" "i" 1 | Out-Null
        Assert-StorageDeadMinimum $dump "async_suspend_scope" "before_suspend" 1 | Out-Null
        Assert-DropBeforeStorageDead $dump "nested_owned" "outer"
        Assert-DropBeforeStorageDead $dump "nested_owned" "inner"
        Assert-DropBeforeStorageDead $dump "early_return" "outer"
        Assert-DropBeforeStorageDead $dump "early_return" "inner"
        Assert-DropBeforeStorageDead $dump "loop_cleanup" "iteration"
    }

    $asyncIr = Join-Path $runRoot "async_suspend_o0.ll"
    Invoke-Checked -Name "async_suspend_o0_ir" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "-O0", "-o", $asyncIr) -WorkingDirectory $repoRoot
    Assert-AsyncLifetimeEndAfterAwait ([IO.File]::ReadAllText($asyncIr))

    $env:VYX_MIR_PASS_AUDIT = "0"
    foreach ($level in @( "-O0", "-O2" )) {
        $name = $level.Substring(1).ToLowerInvariant() + "_jit"
        Invoke-Checked -Name $name -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--run=jit", $level) -WorkingDirectory $repoRoot
        if ((Read-Log $name).Trim() -cne "mir_storage_lifetime OK") {
            throw "$level JIT output mismatch: $((Read-Log $name).Trim())"
        }
    }

    $aotExe = Join-Path $runRoot ("mir_storage_lifetime" + $exeSuffix)
    Invoke-Checked -Name "o2_aot_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=exe", "-O2", "-o", $aotExe) -WorkingDirectory $repoRoot
    Invoke-Checked -Name "o2_aot_run" -FilePath $aotExe -WorkingDirectory $runRoot -MemoryLimitMB 512
    if ((Read-Log "o2_aot_run").Trim() -cne "mir_storage_lifetime OK") {
        throw "O2 AOT output mismatch: $((Read-Log 'o2_aot_run').Trim())"
    }

    $cppOut = Join-Path $runRoot "o2_cpp"
    Invoke-Checked -Name "o2_cpp_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut) -WorkingDirectory $repoRoot
    $cppSourceDir = Join-Path $cppOut "src"
    $cppSources = @(Get-ChildItem -LiteralPath $cppSourceDir -Filter "*.cpp" -File)
    if ($cppSources.Count -eq 0) { throw "MIR2CPP emission produced no C++ source files" }
    $generatedCpp = ($cppSources | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
    Assert-GeneratedCppScopedScalar $generatedCpp
    foreach ($case in @(
        @{ Function = "scalar_scope"; Local = "nested_scalar" },
        @{ Function = "nested_owned"; Local = "inner" },
        @{ Function = "early_return"; Local = "inner" },
        @{ Function = "loop_cleanup"; Local = "iteration" }
    )) {
        Assert-GeneratedCppPreludeLocal $generatedCpp $case.Function $case.Local
    }
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    Invoke-Checked -Name "o2_cpp_configure" -FilePath $cmake `
        -Arguments @("--preset", "ninja-release") -WorkingDirectory $cppOut
    Invoke-Checked -Name "o2_cpp_build" -FilePath $cmake `
        -Arguments @("--build", "--preset", "ninja-release") -WorkingDirectory $cppOut -MemoryLimitMB 4096
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_mir_storage_lifetime" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "MIR2CPP build did not produce $cppExe"
    }
    Invoke-Checked -Name "o2_cpp_run" -FilePath $cppExe `
        -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
    if ((Read-Log "o2_cpp_run").Trim() -cne "mir_storage_lifetime OK") {
        throw "O2 MIR2CPP output mismatch: $((Read-Log 'o2_cpp_run').Trim())"
    }

    Write-Host "mir_storage_lifetime: OK"
    Write-Host "mir_storage_lifetime evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

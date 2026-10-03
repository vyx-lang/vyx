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
$source = Join-Path $testsRoot "cases/mir_fixed_point.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/mir_fixed_point_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
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

function Get-ActiveBranchCount([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    return [regex]::Matches(
        $Dump,
        '(?m)^\s+block #\d+ fn=#' + $functionId + '\s+removed=0\s+.*\s+term=3\s+').Count
}

function Get-ControlCount([string]$Dump, [string]$FunctionName) {
    $functionId = Get-FunctionId $Dump $FunctionName
    return [regex]::Matches(
        $Dump,
        '(?m)^\s+value #\d+ fn=#' + $functionId + '\s+kind=14\s+').Count
}

function Get-AssignedValueKind([string]$Dump, [string]$InstructionName) {
    $instruction = [regex]::Match(
        $Dump,
        '(?m)^\s+instr #\d+ .* kind=2 .* value=#(\d+) .* name=' +
        [regex]::Escape($InstructionName) + '\s*$')
    if (-not $instruction.Success) {
        throw "MIR dump is missing assignment $InstructionName"
    }
    $valueId = $instruction.Groups[1].Value
    $value = [regex]::Match($Dump, '(?m)^\s+value #' + $valueId + ' .* kind=(\d+) ')
    if (-not $value.Success) {
        throw "MIR dump is missing value #$valueId for $InstructionName"
    }
    return [int]$value.Groups[1].Value
}

function Assert-FixedPointAudit {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Scope,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    $roundPattern = '^\[mir-analysis\] pipeline scope=' + [regex]::Escape($Scope) +
        ' round=(\d+) changes=(\d+) values=(\d+) instrs=(\d+) blocks=(\d+)\s*$'
    $fixedPointPattern = '^\[mir-analysis\] pipeline fixed-point scope=' +
        [regex]::Escape($Scope) + ' rounds=(\d+)\s*$'
    $rounds = @()
    $found = $false

    foreach ($line in $Text.Replace("`r`n", "`n").Split("`n")) {
        $round = [regex]::Match($line, $roundPattern)
        if ($round.Success) {
            $rounds += [pscustomobject]@{
                Number = [int]$round.Groups[1].Value
                Changes = [int]$round.Groups[2].Value
            }
            continue
        }

        $fixedPoint = [regex]::Match($line, $fixedPointPattern)
        if (-not $fixedPoint.Success) { continue }
        if ($rounds.Count -eq 0) {
            throw "$Stage reported a fixed point without round audit data"
        }

        $declaredRounds = [int]$fixedPoint.Groups[1].Value
        if ($declaredRounds -lt 1 -or $declaredRounds -gt 4) {
            throw "$Stage exceeded the fixed-point round limit: $declaredRounds"
        }
        if ($rounds[0].Number -ne 1) {
            throw "$Stage did not start its fixed-point audit at round 1"
        }
        if ($rounds[-1].Number -ne $declaredRounds -or $rounds[-1].Changes -ne 0) {
            throw "$Stage did not terminate with a zero-change final round"
        }
        if ($rounds.Count -ne $declaredRounds) {
            throw "$Stage reported $declaredRounds rounds but emitted $($rounds.Count) audit records"
        }
        for ($index = 0; $index -lt $rounds.Count; $index++) {
            if ($rounds[$index].Number -ne ($index + 1)) {
                throw "$Stage emitted non-contiguous fixed-point rounds"
            }
        }

        if ($rounds[0].Changes -gt 0) { $found = $true }
        $rounds = @()
    }

    if (-not $found) {
        throw "$Stage is missing a changed round-1 to zero-change fixed-point audit sequence"
    }
}

function Assert-ProgramOutput([string]$Text, [string]$Stage) {
    if ($Text -notmatch '(?m)^mir_fixed_point OK\s*$') {
        throw "$Stage output is missing mir_fixed_point OK"
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
        @{ Name = "o2_dump_a"; Args = @("--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2") },
        @{ Name = "o2_dump_b"; Args = @("--src=file", $source, "--dump-mir2", "--mir-verify-strict", "-O2") }
    )) {
        Invoke-Checked -Name $run.Name -FilePath $BootstrapCompiler -Arguments $run.Args -WorkingDirectory $repoRoot
    }

    $o2Log = Read-Log "o2_dump_a"
    $o2Dump = Get-MirDump $o2Log
    if ((Get-MirDump (Read-Log "o2_dump_b")) -cne $o2Dump) {
        throw "two O2 fixed-point runs produced different MIR dumps"
    }
    Assert-FixedPointAudit -Text $o2Log -Scope "unit" -Stage "O2 MIR dump"

    if ((Get-ActiveBranchCount $o2Dump "fold_copy_and_constant_branch") -ne 0) {
        throw "O2 did not remove the constant branch"
    }
    if ((Get-AssignedValueKind $o2Dump "pure_eval") -ne 1 -or
        (Get-AssignedValueKind $o2Dump "copied") -ne 1) {
        throw "O2 did not preserve the folded pure evaluation through its copy"
    }
    if ((Get-ControlCount $o2Dump "choose_select") -ne 1) {
        throw "O2 did not form the pure if-expression select"
    }
    if ((Get-AssignedValueKind $o2Dump "kept_call") -ne 10) {
        throw "O2 incorrectly removed or folded the direct-call negative case"
    }

    Invoke-Checked -Name "o2_jit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--run=jit", "-O2") -WorkingDirectory $repoRoot
    $jitLog = Read-Log "o2_jit"
    Assert-FixedPointAudit -Text $jitLog -Scope "function" -Stage "O2 JIT"
    Assert-ProgramOutput -Text $jitLog -Stage "O2 JIT"

    $cppOut = Join-Path $runRoot "o2_cpp"
    Invoke-Checked -Name "o2_cpp_emit" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=cpp", "-O2", "-o", $cppOut) -WorkingDirectory $repoRoot
    Assert-FixedPointAudit -Text (Read-Log "o2_cpp_emit") -Scope "unit" -Stage "O2 MIR2CPP"

    $env:VYX_MIR_PASS_AUDIT = "0"
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    Invoke-Checked -Name "o2_cpp_configure" -FilePath $cmake `
        -Arguments @("--preset", "ninja-release") -WorkingDirectory $cppOut
    Invoke-Checked -Name "o2_cpp_build" -FilePath $cmake `
        -Arguments @("--build", "--preset", "ninja-release") -WorkingDirectory $cppOut
    $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_mir_fixed_point" + $exeSuffix)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "MIR2CPP build did not produce $cppExe"
    }
    Invoke-Checked -Name "o2_cpp_run" -FilePath $cppExe `
        -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
    Assert-ProgramOutput -Text (Read-Log "o2_cpp_run") -Stage "O2 MIR2CPP"

    Write-Host "mir_fixed_point: OK"
    Write-Host "mir_fixed_point evidence=$runRoot"
} finally {
    $env:VYX_MIR_PASS_AUDIT = $originalAudit
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

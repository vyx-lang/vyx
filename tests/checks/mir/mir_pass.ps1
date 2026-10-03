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
$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH

$runRoot = Join-Path $repoRoot ("tests/.cache/mir_pass_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)
$source = Join-Path $testsRoot "cases/mir_pass_constant_fold.vyx"
$expected = "mir_pass_constant_fold OK"
$originalProfile = $env:VYX_BOOTSTRAP_PROFILE

function Get-Sha256Hex([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

function Invoke-Compiler {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $invokeArgs = @{
        FilePath = $BootstrapCompiler
        ArgumentList = $Arguments
        WorkingDirectory = $repoRoot
        StdoutLog = Join-Path $runRoot ($Name + ".stdout.log")
        StderrLog = Join-Path $runRoot ($Name + ".stderr.log")
        DialogLog = Join-Path $runRoot ($Name + ".dialog.log")
        TimeoutSec = $TimeoutSec
    }
    if ($windowsHost) { $invokeArgs.MemoryLimitMB = 4096 }
    $result = Invoke-VyxProcess @invokeArgs
    if ($result.ExitCode -ne 0 -or $result.TimedOut -or
        $result.MemoryExceeded -or $result.DialogCaught) {
        throw "$Name failed: exit=$($result.ExitCode) timeout=$($result.TimedOut) memory=$($result.MemoryExceeded)"
    }
    return $result
}

function Read-Log([string]$Name) {
    return [IO.File]::ReadAllText((Join-Path $runRoot $Name))
}

function Write-LargeMirUnit([string]$Path, [int]$FunctionCount) {
    if ($FunctionCount -lt 2) { throw "FunctionCount must be at least 2" }
    $sourceBuilder = [Text.StringBuilder]::new()
    for ($i = 0; $i -lt $FunctionCount - 1; $i++) {
        [void]$sourceBuilder.AppendLine("fn perf_$i(value: i32) -> i32 {")
        [void]$sourceBuilder.AppendLine("    let folded: i32 = (2i32 + 3i32) * 4i32;")
        [void]$sourceBuilder.AppendLine("    if (folded != 20i32) { return -1; }")
        [void]$sourceBuilder.AppendLine("    return perf_$($i + 1)(value + 1i32);")
        [void]$sourceBuilder.AppendLine("}")
        [void]$sourceBuilder.AppendLine()
    }
    $last = $FunctionCount - 1
    [void]$sourceBuilder.AppendLine("fn perf_$last(value: i32) -> i32 { return value; }")
    [void]$sourceBuilder.AppendLine()
    [void]$sourceBuilder.AppendLine("fn main() -> i32 {")
    [void]$sourceBuilder.AppendLine("    if (perf_0(0i32) != ${last}i32) { return 1; }")
    [void]$sourceBuilder.AppendLine("    return 0;")
    [void]$sourceBuilder.AppendLine("}")
    [IO.File]::WriteAllText($Path, $sourceBuilder.ToString(), [Text.UTF8Encoding]::new($false))
}

function Invoke-External {
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
    return $result
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
    $env:VYX_BOOTSTRAP_PROFILE = "1"
    [void](Invoke-Compiler -Name "default_dump" -Arguments @(
        "--src=file", $source, "--dump-mir2", "--mir-verify-strict"
    ))
    $defaultDump = Read-Log "default_dump.stdout.log"
    if ($defaultDump.IndexOf("[vyxc] mir-opt: -O0 OK", [StringComparison]::Ordinal) -lt 0) {
        throw "implicit optimization level was not reported as -O0"
    }
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile

    $largeUnitFunctionCount = 1024
    $largeUnitSource = Join-Path $runRoot "large_materialized_unit.vyx"
    Write-LargeMirUnit -Path $largeUnitSource -FunctionCount $largeUnitFunctionCount
    $largeUnitWatch = [Diagnostics.Stopwatch]::StartNew()
    [void](Invoke-Compiler -Name "large_unit_o2" -Arguments @(
        "--src=file", $largeUnitSource, "--verify-mir2", "-O2"
    ))
    $largeUnitWatch.Stop()
    $largeUnitElapsedMs = $largeUnitWatch.ElapsedMilliseconds
    if ($largeUnitElapsedMs -gt 90000) {
        throw "large materialized MIR pass exceeded 90 seconds: $largeUnitElapsedMs ms"
    }

    foreach ($level in @("-O0", "-O2")) {
        $stem = $level.Substring(1).ToLowerInvariant()
        [void](Invoke-Compiler -Name ($stem + "_dump") -Arguments @(
            "--src=file", $source, "--dump-mir2", "--mir-verify-strict", $level
        ))

        [void](Invoke-Compiler -Name ($stem + "_jit") -Arguments @(
            "--src=file", $source, "--run=jit", $level
        ))
        $jitOutput = Read-Log ($stem + "_jit.stdout.log")
        if ($jitOutput.Trim() -cne $expected) {
            throw "$level JIT output mismatch: $($jitOutput.Trim())"
        }

        $exe = Join-Path $runRoot ($stem + $exeSuffix)
        [void](Invoke-Compiler -Name ($stem + "_aot_compile") -Arguments @(
            "--src=file", $source, "--emit=exe", $level, "-o", $exe
        ))
        $runArgs = @{
            FilePath = $exe
            WorkingDirectory = $runRoot
            StdoutLog = Join-Path $runRoot ($stem + "_aot.stdout.log")
            StderrLog = Join-Path $runRoot ($stem + "_aot.stderr.log")
            DialogLog = Join-Path $runRoot ($stem + "_aot.dialog.log")
            TimeoutSec = 30
        }
        if ($windowsHost) { $runArgs.MemoryLimitMB = 512 }
        $run = Invoke-VyxProcess @runArgs
        $aotOutput = Read-Log ($stem + "_aot.stdout.log")
        if ($run.ExitCode -ne 0 -or $run.TimedOut -or $aotOutput.Trim() -cne $expected) {
            throw "$level AOT execution failed: exit=$($run.ExitCode) output=$($aotOutput.Trim())"
        }

        $cppOut = Join-Path $runRoot ($stem + "_cpp")
        [void](Invoke-Compiler -Name ($stem + "_cpp") -Arguments @(
            "--src=file", $source, "--emit=cpp", $level, "-o", $cppOut
        ))
        $preset = if ($level -ceq "-O0") { "ninja-debug" } else { "ninja-release" }
        $cmake = (Get-Command cmake -ErrorAction Stop).Source
        [void](Invoke-External -Name ($stem + "_cpp_configure") -FilePath $cmake `
            -Arguments @("--preset", $preset) -WorkingDirectory $cppOut)
        [void](Invoke-External -Name ($stem + "_cpp_build") -FilePath $cmake `
            -Arguments @("--build", "--preset", $preset) -WorkingDirectory $cppOut)
        $cppExe = Join-Path $cppOut ("build/" + $preset + "/vyx_mir_pass_constant_fold" + $exeSuffix)
        if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
            throw "$level MIR2CPP build did not produce $cppExe"
        }
        [void](Invoke-External -Name ($stem + "_cpp_run") -FilePath $cppExe `
            -Arguments @() -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512)
        $cppOutput = Read-Log ($stem + "_cpp_run.stdout.log")
        if ($cppOutput.Trim() -cne $expected) {
            throw "$level MIR2CPP output mismatch: $($cppOutput.Trim())"
        }
    }

    $o0Dump = Read-Log "o0_dump.stdout.log"
    $o2Dump = Read-Log "o2_dump.stdout.log"
    $binaryPattern = '(?m)^\s+value #\d+ .*kind=9 '
    $branchPattern = '(?m)^\s+block #\d+ .*term=3 '
    $o0Binaries = [regex]::Matches($o0Dump, $binaryPattern).Count
    $o2Binaries = [regex]::Matches($o2Dump, $binaryPattern).Count
    $o0Branches = [regex]::Matches($o0Dump, $branchPattern).Count
    $o2Branches = [regex]::Matches($o2Dump, $branchPattern).Count
    if ($o2Binaries -ge $o0Binaries) {
        throw "O2 did not reduce foldable MIR binary values: O0=$o0Binaries O2=$o2Binaries"
    }
    if ($o2Branches -ge $o0Branches) {
        throw "O2 did not fold a constant MIR branch: O0=$o0Branches O2=$o2Branches"
    }
    if ($o2Dump -notmatch '(?m)^\s+value #\d+ .*kind=9 type=#\d+:u16 .*op=81 ') {
        throw "O2 incorrectly folded the mixed-width u8 + u16 expression"
    }
    if ($o2Dump -notmatch '(?m)^\s+value #\d+ .*kind=9 type=#\d+:bool .*op=86 ') {
        throw "O2 incorrectly folded the mixed-width i32 == i64 expression"
    }

    $wrappedConstants = @(
        @{ Type = "i8";  Value = "-128" },
        @{ Type = "i16"; Value = "-32768" },
        @{ Type = "i32"; Value = "-2147483648" },
        @{ Type = "i64"; Value = "-9223372036854775808" },
        @{ Type = "u8";  Value = "0" },
        @{ Type = "u16"; Value = "0" },
        @{ Type = "u32"; Value = "0" },
        @{ Type = "u64"; Value = "0" }
    )
    foreach ($constant in $wrappedConstants) {
        $pattern = ('(?m)^\s+value #\d+ .*kind=1 type=#\d+:' +
                    [regex]::Escape($constant.Type) + ' .*int=' +
                    [regex]::Escape($constant.Value) + ' ')
        if ($o2Dump -notmatch $pattern) {
            throw "O2 dump is missing wrapped $($constant.Type) constant $($constant.Value)"
        }
    }

    $charAdds = [regex]::Matches(
        $o2Dump,
        '(?m)^\s+value #\d+ .*kind=9 type=#\d+:char .*op=81 ')
    if ($charAdds.Count -ne 2) {
        throw "O2 must preserve both char additions as MIR binary values: found=$($charAdds.Count)"
    }

    $charRead = [regex]::Match(
        $o2Dump,
        '(?m)^\s+value #(\d+) .*kind=6 type=#\d+:char .*name=above_byte ')
    if (-not $charRead.Success) {
        throw "O2 dump is missing the >255 char local read"
    }
    $charReadId = $charRead.Groups[1].Value
    $charCompare = [regex]::Match(
        $o2Dump,
        ('(?m)^\s+value #(\d+) .*kind=9 type=#\d+:bool .*lhs=#' +
         [regex]::Escape($charReadId) + ' .*op=86 '))
    if (-not $charCompare.Success) {
        throw "O2 incorrectly folded the >255 char comparison value"
    }
    $charCompareId = $charCompare.Groups[1].Value
    if ($o2Dump -notmatch ('(?m)^\s+block #\d+ .*term=3 term_value=#' +
                           [regex]::Escape($charCompareId) + ' ')) {
        throw "O2 incorrectly propagated or folded the >255 char branch"
    }

    $copiedRead = [regex]::Match(
        $o0Dump,
        '(?m)^\s+value #(\d+) .*kind=6 type=#\d+:bool .*name=copied_before_write ')
    if (-not $copiedRead.Success) {
        throw "O0 dump is missing the copied-before-write read-place value"
    }
    $copiedValueId = $copiedRead.Groups[1].Value
    $copiedBranch = [regex]::Match(
        $o0Dump,
        ('(?m)^\s+block #(\d+) .*term=3 term_value=#' +
         [regex]::Escape($copiedValueId) + ' target=#(\d+) false=#(\d+) '))
    if (-not $copiedBranch.Success) {
        throw "O0 dump is missing the copied-before-write branch"
    }
    $copiedBlockId = $copiedBranch.Groups[1].Value
    $copiedTrueTarget = $copiedBranch.Groups[2].Value
    $copiedExpectedTarget = $copiedTrueTarget
    $copiedGotoGuard = 0
    while ($copiedGotoGuard -lt 32) {
        $copiedGoto = [regex]::Match(
            $o0Dump,
            ('(?m)^\s+block #' + [regex]::Escape($copiedExpectedTarget) +
             ' .*term=2 .*target=#(\d+) '))
        if (-not $copiedGoto.Success) { break }
        $nextTarget = $copiedGoto.Groups[1].Value
        if ($nextTarget -eq $copiedExpectedTarget) { break }
        $copiedExpectedTarget = $nextTarget
        $copiedGotoGuard = $copiedGotoGuard + 1
    }
    if ($o2Dump -notmatch ('(?m)^\s+block #' + [regex]::Escape($copiedBlockId) +
                           ' .*term=2 .*target=#' + [regex]::Escape($copiedExpectedTarget) + ' ')) {
        throw "O2 did not preserve the earlier copied value when the source was written later"
    }

    $overwrittenRead = [regex]::Match(
        $o0Dump,
        '(?m)^\s+value #(\d+) .*kind=6 type=#\d+:i32 .*name=overwritten ')
    if (-not $overwrittenRead.Success) {
        throw "O0 dump is missing the same-block overwritten local read"
    }
    $overwrittenReadId = $overwrittenRead.Groups[1].Value
    $overwrittenCompare = [regex]::Match(
        $o0Dump,
        ('(?m)^\s+value #(\d+) .*kind=9 type=#\d+:bool .*lhs=#' +
         [regex]::Escape($overwrittenReadId) + ' .*op=87 '))
    if (-not $overwrittenCompare.Success) {
        throw "O0 dump is missing the same-block overwritten comparison"
    }
    $overwrittenCompareId = $overwrittenCompare.Groups[1].Value
    $overwrittenBranch = [regex]::Match(
        $o0Dump,
        ('(?m)^\s+block #(\d+) .*term=3 term_value=#' +
         [regex]::Escape($overwrittenCompareId) + ' target=#\d+ false=#(\d+) '))
    if (-not $overwrittenBranch.Success) {
        throw "O0 dump is missing the same-block overwritten branch"
    }
    $overwrittenBlockId = $overwrittenBranch.Groups[1].Value
    $overwrittenFalseTarget = $overwrittenBranch.Groups[2].Value
    $overwrittenExpectedTarget = $overwrittenFalseTarget
    $overwrittenGotoGuard = 0
    while ($overwrittenGotoGuard -lt 32) {
        $overwrittenGoto = [regex]::Match(
            $o0Dump,
            ('(?m)^\s+block #' + [regex]::Escape($overwrittenExpectedTarget) +
             ' .*term=2 .*target=#(\d+) '))
        if (-not $overwrittenGoto.Success) { break }
        $nextTarget = $overwrittenGoto.Groups[1].Value
        if ($nextTarget -eq $overwrittenExpectedTarget) { break }
        $overwrittenExpectedTarget = $nextTarget
        $overwrittenGotoGuard = $overwrittenGotoGuard + 1
    }
    if ($o2Dump -notmatch ('(?m)^\s+block #' + [regex]::Escape($overwrittenBlockId) +
                           ' .*term=2 .*target=#' + [regex]::Escape($overwrittenExpectedTarget) + ' ')) {
        throw "O2 did not select the last same-block assignment"
    }

    $capturedRead = [regex]::Match(
        $o2Dump,
        '(?m)^\s+value #(\d+) .*kind=6 type=#\d+:bool .*name=captured_source ')
    if (-not $capturedRead.Success) {
        throw "O2 dump is missing the reference-captured source read-place value"
    }
    $capturedValueId = $capturedRead.Groups[1].Value
    if ($o2Dump -notmatch ('(?m)^\s+block #\d+ .*term=3 term_value=#' +
                           [regex]::Escape($capturedValueId) + ' ')) {
        throw "O2 incorrectly propagated a source local captured by reference"
    }

    $escapedRead = [regex]::Match(
        $o2Dump,
        '(?m)^\s+value #(\d+) .*kind=6 type=#\d+:bool .*name=escaped ')
    if (-not $escapedRead.Success) {
        throw "O2 dump is missing the escaped bool read-place value"
    }
    $escapedValueId = $escapedRead.Groups[1].Value
    if ($o2Dump -notmatch ("(?m)^\s+block #\d+ .*term=3 term_value=#" +
                           [regex]::Escape($escapedValueId) + " ")) {
        throw "O2 incorrectly folded the address-taken escaped-local branch"
    }

    $o0Cpp = (Get-ChildItem -LiteralPath (Join-Path $runRoot "o0_cpp/src") -Filter *.cpp -File |
        Sort-Object FullName | ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join "`n"
    $o2Cpp = (Get-ChildItem -LiteralPath (Join-Path $runRoot "o2_cpp/src") -Filter *.cpp -File |
        Sort-Object FullName | ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join "`n"
    if ($o0Cpp -ceq $o2Cpp) {
        throw "MIR2CPP O0 and O2 output are identical; shared MIR optimization was not consumed"
    }

    [ordered]@{
        compiler_path = $BootstrapCompiler
        compiler_sha256 = Get-Sha256Hex $BootstrapCompiler
        o0_binary_values = $o0Binaries
        o2_binary_values = $o2Binaries
        o0_branches = $o0Branches
        o2_branches = $o2Branches
        o2_copied_branch_block = [int]$copiedBlockId
        o2_copied_branch_target = [int]$copiedTrueTarget
        o2_reference_capture_branch_value = [int]$capturedValueId
        o2_escaped_branch_value = [int]$escapedValueId
        o2_char_branch_value = [int]$charCompareId
        o2_overwritten_branch_block = [int]$overwrittenBlockId
        large_unit_function_count = $largeUnitFunctionCount
        large_unit_elapsed_ms = $largeUnitElapsedMs
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot "evidence.json") -Encoding utf8

    Write-Host "mir_pass: OK"
    Write-Host "mir_pass evidence=$runRoot"
} finally {
    $env:VYX_BOOTSTRAP_PROFILE = $originalProfile
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

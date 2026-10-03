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
$source = Join-Path $testsRoot "cases/llvm_header_string_len.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/llvm_header_string_len_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

$expectedStrlenCalls = [ordered]@{
    "header_len_direct_int_to_string" = 0
    "header_len_numeric_to_string"    = 0
    "header_len_unsigned_to_string"   = 0
    "header_len_float_to_string"      = 0
    "header_len_bool_to_string"       = 0
    "header_len_scalar_print"         = 0
    "header_len_pointer_to_string"    = 0
    "header_len_ascii_case"            = 0
    "header_len_string_append_decimal" = 0
    "header_len_substring"             = 0
    "header_len_unknown_cstr"          = 2
}

$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH
$hadUnknownCstr = Test-Path Env:VYX_HEADER_STRING_LEN_TEST
$originalUnknownCstr = $env:VYX_HEADER_STRING_LEN_TEST
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

function Read-Log([string]$Name) {
    return [IO.File]::ReadAllText((Join-Path $runRoot ($Name + ".stdout.log")))
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

function Assert-StrlenCalls {
    param(
        [Parameter(Mandatory = $true)][string]$IrPath,
        [Parameter(Mandatory = $true)][string]$Optimization
    )

    $irText = [IO.File]::ReadAllText($IrPath)
    foreach ($entry in $expectedStrlenCalls.GetEnumerator()) {
        $body = Get-IrFunctionBody -IrText $irText -FunctionName $entry.Key
        $actual = [regex]::Matches($body, '(?m)\bcall\b[^\r\n]*@strlen\s*\(').Count
        $expected = $entry.Value
        # Both raw-C-string conversions intentionally scan the same opaque
        # pointer at O0. LLVM may common-subexpression-eliminate that repeated
        # readonly strlen at O2 without changing either Vyx string value.
        if ($entry.Key -eq "header_len_unknown_cstr" -and $Optimization -eq "-O2") {
            $expected = 1
        }
        if ($actual -ne $expected) {
            throw "$Optimization $($entry.Key) expected $expected call(s) to strlen, found $actual"
        }
    }
}

function Assert-KnownLengthLowering {
    param(
        [Parameter(Mandatory = $true)][string]$IrPath,
        [Parameter(Mandatory = $true)][string]$Optimization
    )

    $irText = [IO.File]::ReadAllText($IrPath)
    foreach ($name in @(
        "header_len_direct_int_to_string",
        "header_len_numeric_to_string",
        "header_len_pointer_to_string"
    )) {
        $body = Get-IrFunctionBody -IrText $irText -FunctionName $name
        if ([regex]::Matches($body, '(?m)\bcall\b[^\r\n]*@int_to_string_len_abi\s*\(').Count -lt 1) {
            throw "$Optimization $name did not use int_to_string_len_abi"
        }
    }

    $signedBody = Get-IrFunctionBody -IrText $irText -FunctionName "header_len_numeric_to_string"
    foreach ($symbol in @(
        "vyx_int_decimal_len_abi",
        "vyx_int_to_string_into_abi"
    )) {
        if ($signedBody -notmatch ('(?m)\bcall\b[^\r\n]*@' + [regex]::Escape($symbol) + '\s*\(')) {
            throw "$Optimization header_len_numeric_to_string did not use $symbol"
        }
    }

    $unsignedBody = Get-IrFunctionBody -IrText $irText -FunctionName "header_len_unsigned_to_string"
    foreach ($symbol in @(
        "uint_to_string_len_abi",
        "vyx_uint_decimal_len_abi",
        "vyx_uint_to_string_into_abi"
    )) {
        if ($unsignedBody -notmatch ('(?m)\bcall\b[^\r\n]*@' + [regex]::Escape($symbol) + '\s*\(')) {
            throw "$Optimization header_len_unsigned_to_string did not use $symbol"
        }
    }
    # `builder.append(value.toString())` is now a shared-MIR rewrite to the
    # ordinary StringBuilder.appendU64 method.  At O0 retain a structural
    # assertion for that direct target; O2 may inline the method body.
    if ($Optimization -eq "-O0" -and
        $unsignedBody -notmatch '(?m)\bcall\b[^\r\n]*@__vyx_M_std_2Estring_2EStringBuilder_N_appendU64_R_void_P_std_2Estring_2EStringBuilder_u64\s*\(') {
        throw "$Optimization header_len_unsigned_to_string did not lower builder.append(value.toString()) to StringBuilder.appendU64"
    }
    if ($unsignedBody -match '(?m)\bcall\b[^\r\n]*@vyx_string_builder_append_u64_abi\s*\(') {
        throw "$Optimization header_len_unsigned_to_string still uses the LLVM-only StringBuilder integer append path"
    }

    # Direct String.append(i.toString()) must use the same MIR rewrite as the
    # StringBuilder case.  At O0 retain the target assertion; O2 can inline.
    $stringAppendBody = Get-IrFunctionBody -IrText $irText -FunctionName "header_len_string_append_decimal"
    if ($Optimization -eq "-O0") {
        foreach ($method in @(
            "__vyx_M_std_2Estring_2EString_N_appendI64_R_void_P_std_2Estring_2EString_i64",
            "__vyx_M_std_2Estring_2EString_N_appendU64_R_void_P_std_2Estring_2EString_u64"
        )) {
            if ($stringAppendBody -notmatch ('(?m)\bcall\b[^\r\n]*@' + [regex]::Escape($method) + '\s*\(')) {
                throw "$Optimization header_len_string_append_decimal did not lower to $method"
            }
        }
    }
    foreach ($legacy in @("int_to_string_len_abi", "uint_to_string_len_abi")) {
        if ($stringAppendBody -match ('(?m)\bcall\b[^\r\n]*@' + [regex]::Escape($legacy) + '\s*\(')) {
            throw "$Optimization header_len_string_append_decimal still materializes an owned decimal string through $legacy"
        }
    }

    $boolBody = Get-IrFunctionBody -IrText $irText -FunctionName "header_len_bool_to_string"
    if ($boolBody -notmatch '(?m)\bselect\s+i1\s+') {
        throw "$Optimization header_len_bool_to_string is missing its bool length select"
    }

    $printBody = Get-IrFunctionBody -IrText $irText -FunctionName "header_len_scalar_print"
    if ([regex]::Matches($printBody, '(?m)\bcall\b[^\r\n]*@int_to_string_len_abi\s*\(').Count -lt 1) {
        throw "$Optimization header_len_scalar_print did not use int_to_string_len_abi"
    }
    if ([regex]::Matches($printBody, '(?m)\bcall\b[^\r\n]*@uint_to_string_len_abi\s*\(').Count -lt 1) {
        throw "$Optimization header_len_scalar_print did not use uint_to_string_len_abi"
    }
    if ($printBody -match '(?m)\bcall\b[^\r\n]*@int_to_string\s*\(') {
        throw "$Optimization header_len_scalar_print still uses legacy int_to_string"
    }
    if ([regex]::Matches($printBody, '(?m)\bcall\b[^\r\n]*@bool_to_string\s*\(').Count -lt 2) {
        throw "$Optimization header_len_scalar_print did not format both bool values"
    }
    if ($Optimization -eq "-O0" -and $printBody -notmatch '(?m)\bselect\s+i1\s+') {
        throw "$Optimization header_len_scalar_print is missing its bool length select"
    }

    $floatBody = Get-IrFunctionBody -IrText $irText -FunctionName "header_len_float_to_string"
    $floatSnprintfCalls = [regex]::Matches($floatBody, '(?m)\bcall\b[^\r\n]*@snprintf\s*\(').Count
    if ($floatSnprintfCalls -lt 6) {
        throw "$Optimization header_len_float_to_string expected sizing/write snprintf pairs, found $floatSnprintfCalls call(s)"
    }
    if ($Optimization -eq "-O0" -and $floatBody -notmatch '(?m)\bicmp\s+slt\s+i32\b') {
        throw "$Optimization header_len_float_to_string is missing the negative snprintf sizing guard"
    }

    $caseBody = Get-IrFunctionBody -IrText $irText -FunctionName "header_len_ascii_case"
    foreach ($symbol in @("vyx_string_to_upper_len_abi", "vyx_string_to_lower_len_abi")) {
        if ($caseBody -notmatch ('(?m)\bcall\b[^\r\n]*@' + [regex]::Escape($symbol) + '\s*\(')) {
            throw "$Optimization header_len_ascii_case did not use $symbol"
        }
    }
}

function Get-IrDeclarationWithAttrs {
    param(
        [Parameter(Mandatory = $true)][string]$IrText,
        [Parameter(Mandatory = $true)][string]$Symbol
    )

    $pattern = '(?m)^declare\b[^\r\n]*@' + [regex]::Escape($Symbol) + '\s*\([^\r\n]*$'
    $match = [regex]::Match($IrText, $pattern)
    if (-not $match.Success) { throw "IR is missing declaration for $Symbol" }
    $declaration = $match.Value
    $functionAttrs = ""
    $group = [regex]::Match($declaration, '#(?<id>\d+)\s*$')
    if ($group.Success) {
        $groupPattern = '(?m)^attributes\s+#' + [regex]::Escape($group.Groups['id'].Value) + '\s*=\s*\{(?<body>[^\r\n]*)\}$'
        $groupMatch = [regex]::Match($IrText, $groupPattern)
        if (-not $groupMatch.Success) {
            throw "IR declaration for $Symbol refers to a missing attribute group"
        }
        $functionAttrs = $groupMatch.Groups['body'].Value
    }
    return [pscustomobject]@{ Declaration = $declaration; FunctionAttrs = $functionAttrs }
}

function Assert-IrTextContains {
    param([string]$Text, [string]$Pattern, [string]$Message)
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Assert-IntegerStringRuntimeAttrs {
    param([Parameter(Mandatory = $true)][string]$IrPath)

    $irText = [IO.File]::ReadAllText($IrPath)
    foreach ($symbol in @("vyx_int_decimal_len_abi", "vyx_uint_decimal_len_abi")) {
        $entry = Get-IrDeclarationWithAttrs -IrText $irText -Symbol $symbol
        Assert-IrTextContains $entry.FunctionAttrs '(memory\(none\)|readnone)' "$symbol is missing its readnone memory contract"
        Assert-IrTextContains $entry.FunctionAttrs '\bnounwind\b' "$symbol is missing nounwind"
    }
    foreach ($symbol in @("vyx_int_to_string_into_abi", "vyx_uint_to_string_into_abi")) {
        $entry = Get-IrDeclarationWithAttrs -IrText $irText -Symbol $symbol
        Assert-IrTextContains $entry.FunctionAttrs '(memory\(write\)|writeonly)' "$symbol is missing its writeonly memory contract"
        Assert-IrTextContains $entry.FunctionAttrs '\bnounwind\b' "$symbol is missing nounwind"
        Assert-IrTextContains $entry.Declaration 'captures\(none\)' "$symbol output pointer is missing captures(none)"
        Assert-IrTextContains $entry.Declaration '\bwriteonly\b' "$symbol output pointer is missing writeonly"
    }
    foreach ($symbol in @("int_to_string_len_abi", "uint_to_string_len_abi")) {
        $entry = Get-IrDeclarationWithAttrs -IrText $irText -Symbol $symbol
        Assert-IrTextContains $entry.FunctionAttrs '\bnounwind\b' "$symbol is missing nounwind"
        Assert-IrTextContains $entry.Declaration 'captures\(none\)' "$symbol out_len pointer is missing captures(none)"
        Assert-IrTextContains $entry.Declaration '\bwriteonly\b' "$symbol out_len pointer is missing writeonly"
        if ($entry.FunctionAttrs -match '(memory\(none\)|readnone|memory\(write\)|writeonly)') {
            throw "$symbol must not receive a function-level memory contract"
        }
    }
}

function Assert-ProgramOutput {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    $output = (Read-Log $Name).Replace("`r`n", "`n").Trim()
    $expected = "-42`n18446744073709551615`nfalse`ntrue`nllvm_header_string_len OK"
    if ($output -cne $expected) {
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
    $env:VYX_HEADER_STRING_LEN_TEST = "opaque-c-string"

    foreach ($level in @("-O0", "-O2")) {
        $stem = $level.Substring(1).ToLowerInvariant()
        $ir = Join-Path $runRoot ("llvm_header_string_len_" + $stem + ".ll")
        Invoke-Checked -Name ($stem + "_emit_ir") -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--emit=ir", $level, "-o", $ir) `
            -WorkingDirectory $repoRoot
        Assert-StrlenCalls -IrPath $ir -Optimization $level
        Assert-KnownLengthLowering -IrPath $ir -Optimization $level
        Assert-IntegerStringRuntimeAttrs -IrPath $ir

        $jitName = $stem + "_jit"
        Invoke-Checked -Name $jitName -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--run=jit", $level) `
            -WorkingDirectory $repoRoot
        Assert-ProgramOutput -Name $jitName -Stage ($level + " JIT")

        $exe = Join-Path $runRoot ("llvm_header_string_len_" + $stem + $exeSuffix)
        $aotEmitName = $stem + "_aot_emit"
        Invoke-Checked -Name $aotEmitName -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--emit=exe", $level, "-o", $exe) `
            -WorkingDirectory $repoRoot
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            throw "$level AOT did not produce $exe"
        }
        $aotRunName = $stem + "_aot_run"
        Invoke-Checked -Name $aotRunName -FilePath $exe -WorkingDirectory $runRoot -MemoryLimitMB 512
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
        $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_llvm_header_string_len" + $exeSuffix)
        if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
            throw "MIR2CPP build did not produce $cppExe"
        }
        Invoke-Checked -Name "o2_cpp_run" -FilePath $cppExe `
            -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
        Assert-ProgramOutput -Name "o2_cpp_run" -Stage "O2 MIR2CPP"
    }

    Write-Host "llvm_header_string_len: OK"
    Write-Host "llvm_header_string_len evidence=$runRoot"
} finally {
    if ($hadUnknownCstr) {
        $env:VYX_HEADER_STRING_LEN_TEST = $originalUnknownCstr
    } else {
        Remove-Item Env:VYX_HEADER_STRING_LEN_TEST -ErrorAction SilentlyContinue
    }
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

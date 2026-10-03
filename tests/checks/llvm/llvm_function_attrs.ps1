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
$source = Join-Path $testsRoot "cases/verify_codegen_attrs.vyx"
$runRoot = Join-Path $repoRoot ("tests/.cache/llvm_function_attrs_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
[void][IO.Directory]::CreateDirectory($runRoot)

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

function Get-FunctionFlags {
    param(
        [Parameter(Mandatory = $true)][string]$DumpText,
        [Parameter(Mandatory = $true)][string]$NamePattern
    )

    $matches = [regex]::Matches($DumpText,
        '(?m)^\s*fn\s+#[^\r\n]*\bname=' + $NamePattern + '(?=\s|$)[^\r\n]*')
    if ($matches.Count -eq 0) {
        throw "missing function dump row matching $NamePattern"
    }
    $flags = @()
    foreach ($match in $matches) {
        $flagMatch = [regex]::Match($match.Value, '\bflags=(?<flags>\d+)\b')
        if (-not $flagMatch.Success) {
            throw "function dump row has no flags field: $($match.Value)"
        }
        $flags += [int]$flagMatch.Groups["flags"].Value
    }
    return $flags
}

function Get-DeclarationAttributes {
    param(
        [Parameter(Mandatory = $true)][string]$IrText,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )
    $match = [regex]::Match($IrText, '(?m)^declare\b[^\r\n]*@' + [regex]::Escape($FunctionName) + '\([^\r\n]*\)\s*(?<group>#[0-9]+)')
    if (-not $match.Success) { throw "missing LLVM declaration for $FunctionName" }
    $group = [regex]::Escape($match.Groups['group'].Value)
    $attrs = [regex]::Match($IrText, '(?m)^attributes\s+' + $group + '\s*=\s*\{(?<attrs>[^}]*)\}')
    if (-not $attrs.Success) { throw "missing LLVM attribute group for declaration $FunctionName" }
    return $attrs.Groups['attrs'].Value
}

function Get-DeclarationParameters {
    param(
        [Parameter(Mandatory = $true)][string]$IrText,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )
    $match = [regex]::Match($IrText, '(?m)^declare\b[^\r\n]*@' + [regex]::Escape($FunctionName) + '\((?<params>[^\r\n]*)\)')
    if (-not $match.Success) { throw "missing LLVM declaration for $FunctionName" }
    return $match.Groups['params'].Value
}

function Assert-FunctionFlags {
    param(
        [Parameter(Mandatory = $true)][string]$DumpText,
        [Parameter(Mandatory = $true)][string]$NamePattern,
        [Parameter(Mandatory = $true)][int]$Required,
        [int]$Forbidden = 0,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    foreach ($flags in @(Get-FunctionFlags -DumpText $DumpText -NamePattern $NamePattern)) {
        if (($flags -band $Required) -ne $Required) {
            throw "$Stage $NamePattern flags=$flags missing required mask $Required"
        }
        if (($flags -band $Forbidden) -ne 0) {
            throw "$Stage $NamePattern flags=$flags contains forbidden mask $Forbidden"
        }
    }
}

function Get-IrFunctionAttributes {
    param(
        [Parameter(Mandatory = $true)][string]$IrText,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )

    $mangled = $FunctionName.Replace("_", "_5F")
    $escaped = [regex]::Escape($mangled)
    $pattern = '(?m)^define\b[^\r\n]*@[^\s(]*' + $escaped + '(?=_R_)[^\s(]*\([^\r\n]*\)[^\r\n]*?(?<group>#[0-9]+)\s*\{'
    $matches = [regex]::Matches($IrText, $pattern)
    if ($matches.Count -ne 1) {
        throw "expected one LLVM definition for $FunctionName, found $($matches.Count)"
    }
    $group = $matches[0].Groups["group"].Value
    $groupPattern = '(?m)^attributes\s+' + [regex]::Escape($group) + '\s*=\s*\{(?<attrs>[^}]*)\}'
    $groupMatch = [regex]::Match($IrText, $groupPattern)
    if (-not $groupMatch.Success) {
        throw "missing LLVM attribute group $group for $FunctionName"
    }
    return $groupMatch.Groups["attrs"].Value
}

function Assert-IrAttribute {
    param(
        [Parameter(Mandatory = $true)][string]$Attributes,
        [Parameter(Mandatory = $true)][string]$Attribute,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )

    if ($Attributes -notmatch ('\b' + [regex]::Escape($Attribute) + '\b')) {
        throw "LLVM $FunctionName is missing ${Attribute}: $Attributes"
    }
}

function Assert-ReadonlyIrAttribute {
    param(
        [Parameter(Mandatory = $true)][string]$Attributes,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )
    # LLVM 18 canonicalizes the legacy `readonly` spelling to memory(read).
    if (-not [regex]::IsMatch($Attributes, '(^|\s)(readonly|memory\(read\))(?=\s|$)')) {
        throw "LLVM $FunctionName is missing readonly/memory(read): $Attributes"
    }
}

function Assert-NoIrAttribute {
    param(
        [Parameter(Mandatory = $true)][string]$Attributes,
        [Parameter(Mandatory = $true)][string]$Attribute,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )

    if ($Attributes -match ('\b' + [regex]::Escape($Attribute) + '\b')) {
        throw "LLVM $FunctionName unexpectedly contains ${Attribute}: $Attributes"
    }
}

$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH
$originalDyldLibraryPath = $env:DYLD_LIBRARY_PATH
$pathSeparator = if ($windowsHost) { ";" } else { ":" }

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

    Invoke-Checked -Name "hir_dump" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--dump-hir2") -WorkingDirectory $repoRoot
    $hirText = Read-Log "hir_dump"

    Invoke-Checked -Name "mir_dump" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--dump-mir2", "-O0") -WorkingDirectory $repoRoot
    $mirText = Read-Log "mir_dump"

    $attrInline = 4096
    $attrNoinline = 8192
    $attrCold = 16384
    $attrHot = 32768
    $exactNames = @{
        attr_inc = [regex]::Escape("attr_inc")
        attr_dec = [regex]::Escape("attr_dec")
        attr_cold = [regex]::Escape("attr_cold")
        attr_hot = [regex]::Escape("attr_hot")
        attr_noinline_wins = [regex]::Escape("attr_noinline_wins")
        attr_hot_wins = [regex]::Escape("attr_hot_wins")
    }
    foreach ($dump in @(@{ Text = $hirText; Stage = "HIR" }, @{ Text = $mirText; Stage = "MIR" })) {
        Assert-FunctionFlags -DumpText $dump.Text -NamePattern $exactNames.attr_inc `
            -Required $attrInline -Forbidden $attrNoinline -Stage $dump.Stage
        Assert-FunctionFlags -DumpText $dump.Text -NamePattern $exactNames.attr_dec `
            -Required $attrNoinline -Forbidden $attrInline -Stage $dump.Stage
        Assert-FunctionFlags -DumpText $dump.Text -NamePattern $exactNames.attr_cold `
            -Required $attrCold -Forbidden $attrHot -Stage $dump.Stage
        Assert-FunctionFlags -DumpText $dump.Text -NamePattern $exactNames.attr_hot `
            -Required $attrHot -Forbidden $attrCold -Stage $dump.Stage
        Assert-FunctionFlags -DumpText $dump.Text -NamePattern $exactNames.attr_noinline_wins `
            -Required ($attrInline -bor $attrNoinline) -Stage $dump.Stage
        Assert-FunctionFlags -DumpText $dump.Text -NamePattern $exactNames.attr_hot_wins `
            -Required ($attrCold -bor $attrHot) -Stage $dump.Stage
    }
    Assert-FunctionFlags -DumpText $hirText -NamePattern 'attr_generic[^\s]*' `
        -Required $attrNoinline -Forbidden $attrInline -Stage "HIR generic declaration"
    Assert-FunctionFlags -DumpText $mirText -NamePattern 'mir2\$attr_5Fgeneric[^\s]*' `
        -Required $attrNoinline -Forbidden $attrInline -Stage "MIR generic instance"

    $ir = Join-Path $runRoot "llvm_function_attrs_o0.ll"
    Invoke-Checked -Name "o0_emit_ir" -FilePath $BootstrapCompiler `
        -Arguments @("--src=file", $source, "--emit=ir", "-O0", "-o", $ir) `
        -WorkingDirectory $repoRoot
    $irText = [IO.File]::ReadAllText($ir)
    $inlineAttrs = Get-IrFunctionAttributes -IrText $irText -FunctionName "attr_inc"
    Assert-IrAttribute -Attributes $inlineAttrs -Attribute "inlinehint" -FunctionName "attr_inc"
    Assert-NoIrAttribute -Attributes $inlineAttrs -Attribute "alwaysinline" -FunctionName "attr_inc"
    Assert-NoIrAttribute -Attributes $inlineAttrs -Attribute "noinline" -FunctionName "attr_inc"
    $noinlineAttrs = Get-IrFunctionAttributes -IrText $irText -FunctionName "attr_dec"
    Assert-IrAttribute -Attributes $noinlineAttrs -Attribute "noinline" -FunctionName "attr_dec"
    Assert-NoIrAttribute -Attributes $noinlineAttrs -Attribute "alwaysinline" -FunctionName "attr_dec"
    Assert-NoIrAttribute -Attributes $noinlineAttrs -Attribute "inlinehint" -FunctionName "attr_dec"
    Assert-IrAttribute -Attributes (Get-IrFunctionAttributes -IrText $irText -FunctionName "attr_cold") `
        -Attribute "cold" -FunctionName "attr_cold"
    Assert-IrAttribute -Attributes (Get-IrFunctionAttributes -IrText $irText -FunctionName "attr_hot") `
        -Attribute "hot" -FunctionName "attr_hot"
    $noinlineWinsAttrs = Get-IrFunctionAttributes -IrText $irText -FunctionName "attr_noinline_wins"
    Assert-IrAttribute -Attributes $noinlineWinsAttrs -Attribute "noinline" -FunctionName "attr_noinline_wins"
    Assert-NoIrAttribute -Attributes $noinlineWinsAttrs -Attribute "alwaysinline" -FunctionName "attr_noinline_wins"
    Assert-NoIrAttribute -Attributes $noinlineWinsAttrs -Attribute "inlinehint" -FunctionName "attr_noinline_wins"
    $hotWinsAttrs = Get-IrFunctionAttributes -IrText $irText -FunctionName "attr_hot_wins"
    Assert-IrAttribute -Attributes $hotWinsAttrs -Attribute "hot" -FunctionName "attr_hot_wins"
    Assert-NoIrAttribute -Attributes $hotWinsAttrs -Attribute "cold" -FunctionName "attr_hot_wins"
    foreach ($name in @("strlen", "strncmp", "strcmp", "memcmp")) {
        $attrs = Get-DeclarationAttributes -IrText $irText -FunctionName $name
        Assert-ReadonlyIrAttribute -Attributes $attrs -FunctionName $name
        Assert-IrAttribute -Attributes $attrs -Attribute "nounwind" -FunctionName $name
    }
    foreach ($case in @(
        @{ Name = "strlen"; ReadonlyParameters = 1 },
        @{ Name = "strncmp"; ReadonlyParameters = 2 },
        @{ Name = "strcmp"; ReadonlyParameters = 2 },
        @{ Name = "memcmp"; ReadonlyParameters = 2 }
    )) {
        $params = Get-DeclarationParameters -IrText $irText -FunctionName $case.Name
        $count = [regex]::Matches($params, 'ptr(?=[^,)]*\breadonly\b)(?=[^,)]*captures\(none\))').Count
        if ($count -ne $case.ReadonlyParameters) {
            throw "$($case.Name) expected $($case.ReadonlyParameters) readonly non-capturing pointer parameter(s), found $count"
        }
    }

    foreach ($level in @("-O0", "-O2")) {
        $stem = $level.Substring(1).ToLowerInvariant()
        Invoke-Checked -Name ($stem + "_jit") -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--run=jit", $level) -WorkingDirectory $repoRoot

        $exe = Join-Path $runRoot ("llvm_function_attrs_" + $stem + $exeSuffix)
        Invoke-Checked -Name ($stem + "_aot_emit") -FilePath $BootstrapCompiler `
            -Arguments @("--src=file", $source, "--emit=exe", $level, "-o", $exe) `
            -WorkingDirectory $repoRoot
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            throw "$level AOT did not produce $exe"
        }
        Invoke-Checked -Name ($stem + "_aot_run") -FilePath $exe `
            -WorkingDirectory $runRoot -MemoryLimitMB 512
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
        $cppExe = Join-Path $cppOut ("build/ninja-release/vyx_verify_codegen_attrs" + $exeSuffix)
        if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
            throw "MIR2CPP build did not produce $cppExe"
        }
        Invoke-Checked -Name "o2_cpp_run" -FilePath $cppExe `
            -WorkingDirectory (Split-Path -Parent $cppExe) -MemoryLimitMB 512
    }

    Write-Host "llvm_function_attrs: OK"
    Write-Host "llvm_function_attrs evidence=$runRoot"
} finally {
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    $env:DYLD_LIBRARY_PATH = $originalDyldLibraryPath
}

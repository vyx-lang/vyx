param(
    [Parameter(Mandatory = $true)]
    [string]$BootstrapCompiler
)

$ErrorActionPreference = "Stop"
if ($null -ne (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue)) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$fixtureRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $fixtureRoot "..\..\..")).Path
$compiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$runningOnWindows = $env:OS -eq "Windows_NT"
$cacheRoot = Join-Path $repoRoot "tests\.cache"
$runRoot = Join-Path $cacheRoot ("no_mangle_export_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)

function Resolve-LlvmTool([string]$Name) {
    $fileName = if ($runningOnWindows) { "$Name.exe" } else { $Name }
    if (-not [string]::IsNullOrWhiteSpace($env:LLVM_ROOT)) {
        $candidate = Join-Path $env:LLVM_ROOT ("bin\" + $fileName)
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return (Get-Command $fileName -ErrorAction Stop).Source
}

function Invoke-Captured {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )
    Push-Location $WorkingDirectory
    try {
        $lines = & $FilePath @ArgumentList 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Text = ($lines -join "`n") }
}

function Assert-Success([string]$Name, $Result) {
    if ($Result.ExitCode -ne 0) {
        throw "[no-mangle-export] $Name failed (exit=$($Result.ExitCode))`n$($Result.Text)"
    }
}

function Remove-RunRoot {
    if (-not (Test-Path -LiteralPath $runRoot -PathType Container)) { return }
    $resolved = (Resolve-Path -LiteralPath $runRoot).Path
    $cacheResolved = (Resolve-Path -LiteralPath $cacheRoot).Path
    if (-not $resolved.StartsWith($cacheResolved, [StringComparison]::OrdinalIgnoreCase)) {
        throw "[no-mangle-export] refusing cleanup outside tests cache: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

try {
    $llvmNm = Resolve-LlvmTool "llvm-nm"
    $source = Join-Path $fixtureRoot "src\main.vyx"
    $objectName = if ($runningOnWindows) { "runtime_abi_probe.obj" } else { "runtime_abi_probe.o" }
    $object = Join-Path $runRoot $objectName

    $compile = Invoke-Captured -FilePath $compiler -ArgumentList @(
        "--src=file", $source,
        "--emit=obj",
        "--export-all",
        "-O0",
        "-o", $object
    ) -WorkingDirectory $runRoot
    Assert-Success "object compile" $compile

    $symbols = Invoke-Captured -FilePath $llvmNm -ArgumentList @(
        "--defined-only", "--extern-only", $object
    ) -WorkingDirectory $runRoot
    Assert-Success "symbol inspection" $symbols

    if ($symbols.Text -notmatch "(?m)\bvyx_runtime_abi_probe$") {
        throw "[no-mangle-export] exact C ABI symbol is missing`n$($symbols.Text)"
    }
    if ($symbols.Text -match "__vyx_F_vyx_runtime_abi_probe") {
        throw "[no-mangle-export] no_mangle function retained a Vyx-mangled symbol`n$($symbols.Text)"
    }
    if ($symbols.Text -notmatch "(?m)\bargCount$") {
        throw "[no-mangle-export] no_mangle definition did not reuse the builtin ABI declaration`n$($symbols.Text)"
    }
    if ($symbols.Text -match "(?m)\bargCount\.\d+$") {
        throw "[no-mangle-export] builtin ABI collision received an LLVM suffix`n$($symbols.Text)"
    }

    Write-Host "no_mangle_export: OK"
} finally {
    Remove-RunRoot
}

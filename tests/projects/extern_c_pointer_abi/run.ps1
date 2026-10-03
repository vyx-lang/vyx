param(
    [string]$BootstrapCompiler = ""
)

$ErrorActionPreference = "Stop"
if ($null -ne (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue)) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$fixtureRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $fixtureRoot "..\..\..")).Path
$isWindowsPlatform = $env:OS -eq "Windows_NT"
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$originalPath = $env:Path
$env:Path = (Split-Path -Parent $BootstrapCompiler) + [System.IO.Path]::PathSeparator + $env:Path

$cacheRoot = Join-Path $repoRoot "tests\.cache"
New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
$runRoot = Join-Path $cacheRoot ("extern_c_pointer_abi_run_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
New-Item -ItemType Directory -Path $runRoot | Out-Null

function Remove-RunRoot {
    if (-not (Test-Path -LiteralPath $runRoot)) { return }
    $resolved = (Resolve-Path -LiteralPath $runRoot).Path
    $cacheResolved = (Resolve-Path -LiteralPath $cacheRoot).Path
    if (-not $resolved.StartsWith($cacheResolved, [StringComparison]::OrdinalIgnoreCase)) {
        throw "[extern-c-pointer-abi] refusing cleanup outside tests cache: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

function Invoke-Captured {
    param(
        [Parameter(Mandatory=$true)][string]$FilePath,
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][string[]]$ArgumentList,
        [string]$WorkingDirectory = $runRoot
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

function Assert-Success {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)]$Result
    )
    if ($Result.ExitCode -ne 0) {
        throw "[extern-c-pointer-abi] $Name failed (exit=$($Result.ExitCode))`n$($Result.Text)"
    }
}

try {
    Copy-Item -LiteralPath (Join-Path $fixtureRoot "Vyx.toml") -Destination $runRoot
    Copy-Item -LiteralPath (Join-Path $fixtureRoot "native") -Destination $runRoot -Recurse
    Copy-Item -LiteralPath (Join-Path $fixtureRoot "src") -Destination $runRoot -Recurse

    Write-Host "[extern-c-pointer-abi] LLVM project build"
    $aot = Invoke-Captured -FilePath $BootstrapCompiler -ArgumentList @(
        "build", "-j1"
    ) -WorkingDirectory $runRoot
    Assert-Success "LLVM project build" $aot
    $aotName = if ($isWindowsPlatform) { "extern_c_pointer_abi.exe" } else { "extern_c_pointer_abi" }
    $aotExe = Join-Path $runRoot ("target\" + $aotName)
    if (-not (Test-Path -LiteralPath $aotExe -PathType Leaf)) {
        throw "[extern-c-pointer-abi] LLVM executable missing: $aotExe"
    }
    $aotRun = Invoke-Captured -FilePath $aotExe -ArgumentList @()
    Assert-Success "LLVM executable" $aotRun

    Write-Host "[extern-c-pointer-abi] MIR2CPP"
    $cppOut = Join-Path $runRoot "mir2cpp"
    $generate = Invoke-Captured -FilePath $BootstrapCompiler -ArgumentList @(
        "--src=project", $runRoot, "--emit=cpp", "-o", $cppOut
    )
    Assert-Success "MIR2CPP generation" $generate

    $generatedText = (Get-ChildItem -LiteralPath $cppOut -Recurse -File |
        Where-Object { $_.Extension -eq ".cpp" -or $_.Extension -eq ".hpp" } |
        ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
    foreach ($pattern in @(
        'extern "C" void\* vyx_identity_i64\(void\*',
        'vyx_sum_i64\(to_rawptr\(',
        'vyx::ptr_from_raw<int64_t>\(vyx_identity_i64\(to_rawptr\('
    )) {
        if ($generatedText -notmatch $pattern) {
            throw "[extern-c-pointer-abi] generated C++ is missing ABI pattern: $pattern"
        }
    }

    $configure = Invoke-Captured -FilePath "cmake" -ArgumentList @(
        "--preset", "ninja-release"
    ) -WorkingDirectory $cppOut
    Assert-Success "CMake configure" $configure
    $build = Invoke-Captured -FilePath "cmake" -ArgumentList @(
        "--build", "--preset", "ninja-release", "--parallel"
    ) -WorkingDirectory $cppOut
    Assert-Success "CMake build" $build

    $cppExeName = if ($isWindowsPlatform) { "vyx_extern_c_pointer_abi.exe" } else { "vyx_extern_c_pointer_abi" }
    $cppExe = Join-Path $cppOut ("build\ninja-release\" + $cppExeName)
    if (-not (Test-Path -LiteralPath $cppExe -PathType Leaf)) {
        throw "[extern-c-pointer-abi] MIR2CPP executable missing: $cppExe"
    }
    $run = Invoke-Captured -FilePath $cppExe -ArgumentList @()
    Assert-Success "MIR2CPP executable" $run

    Write-Host "extern_c_pointer_abi: OK"
} finally {
    $env:Path = $originalPath
    Remove-RunRoot
}

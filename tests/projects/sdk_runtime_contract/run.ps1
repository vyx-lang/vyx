param(
    [Parameter(Mandatory = $true)]
    [string]$BootstrapCompiler,
    [string]$RuntimeDir = ""
)

$ErrorActionPreference = "Stop"
if ($null -ne (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue)) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$fixtureRoot = $PSScriptRoot
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $fixtureRoot "..\..\..")).Path
$compiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$compilerDir = Split-Path -Parent $compiler
$runningOnWindows = $env:OS -eq "Windows_NT"
$compilerInspectionTarget = $compiler
if (-not $runningOnWindows) {
    $packagedCompilerPayload = Join-Path $compilerDir ".vyxc-bin"
    if (Test-Path -LiteralPath $packagedCompilerPayload -PathType Leaf) {
        $compilerInspectionTarget = (Resolve-Path -LiteralPath $packagedCompilerPayload).Path
    }
}
$cacheRoot = Join-Path $repoRoot "tests\.cache"
$runRoot = Join-Path $cacheRoot ("sdk_runtime_contract_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
$originalPath = $env:Path
$originalLdLibraryPath = $env:LD_LIBRARY_PATH

function Resolve-LlvmTool([string]$Name) {
    $fileName = if ($runningOnWindows) { "$Name.exe" } else { $Name }
    $sdkRoot = Split-Path -Parent $compilerDir
    foreach ($toolDirectory in @(
        (Join-Path $sdkRoot "toolchain\bin"),
        (Join-Path $sdkRoot "clang\bin"),
        (Join-Path $compilerDir "clang\bin")
    )) {
        $candidate = Join-Path $toolDirectory $fileName
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
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
        throw "[sdk-runtime-contract] $Name failed (exit=$($Result.ExitCode))`n$($Result.Text)"
    }
}

function Remove-RunRoot {
    if (-not (Test-Path -LiteralPath $runRoot -PathType Container)) { return }
    $resolved = (Resolve-Path -LiteralPath $runRoot).Path
    $cacheResolved = (Resolve-Path -LiteralPath $cacheRoot).Path
    if (-not $resolved.StartsWith($cacheResolved, [StringComparison]::OrdinalIgnoreCase)) {
        throw "[sdk-runtime-contract] refusing cleanup outside tests cache: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
if (-not $runningOnWindows) {
    $env:LD_LIBRARY_PATH = ""
    $compilerHelp = Invoke-Captured -FilePath $compiler -ArgumentList @("--help") `
        -WorkingDirectory $runRoot
    Assert-Success "standalone compiler startup" $compilerHelp
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
}
$pathEntries = @($compilerDir)
if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $pathEntries += (Resolve-Path -LiteralPath $RuntimeDir).Path
}
$env:Path = ($pathEntries -join [System.IO.Path]::PathSeparator) `
    + [System.IO.Path]::PathSeparator + $originalPath

try {
    $llvmReadObj = Resolve-LlvmTool "llvm-readobj"
    $llvmNm = Resolve-LlvmTool "llvm-nm"
    $source = Join-Path $fixtureRoot "src\main.vyx"
    $appName = if ($runningOnWindows) { "sdk_runtime_contract.exe" } else { "sdk_runtime_contract" }
    $app = Join-Path $runRoot $appName

    $compile = Invoke-Captured -FilePath $compiler -ArgumentList @(
        "--emit=exe", "--src=file", $source, "-O2", "-o", $app
    ) -WorkingDirectory $runRoot
    Assert-Success "direct compile" $compile

    $dependencyArgs = if ($runningOnWindows) { @("--coff-imports", $app) } else { @("--needed-libs", $app) }
    $appDependencies = Invoke-Captured -FilePath $llvmReadObj `
        -ArgumentList $dependencyArgs -WorkingDirectory $runRoot
    Assert-Success "application dependency inspection" $appDependencies

    foreach ($forbidden in @(
        "vyx_compiler_backend", "vyx_run_rt", "vyx_rt", "LLVM"
    )) {
        if ($appDependencies.Text -match [regex]::Escape($forbidden)) {
            throw "[sdk-runtime-contract] ordinary application imports forbidden compiler/runtime library '$forbidden'`n$($appDependencies.Text)"
        }
    }

    $runtimeFile = if ($runningOnWindows) { "vyx_runtime.lib" } else { "libvyx_runtime.a" }
    $runtimeCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
        $runtimeCandidates += Join-Path (Resolve-Path -LiteralPath $RuntimeDir).Path $runtimeFile
    }
    $runtimeCandidates += Join-Path $compilerDir $runtimeFile
    $runtimeCandidates += Join-Path `
        (Join-Path (Split-Path -Parent $compilerDir) "lib") `
        $runtimeFile
    $runtimeArchive = $runtimeCandidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($runtimeArchive)) {
        throw "[sdk-runtime-contract] canonical static runtime not found: $runtimeFile"
    }

    $runtimeSymbols = Invoke-Captured -FilePath $llvmNm -ArgumentList @(
        "--defined-only", "--extern-only", $runtimeArchive
    ) -WorkingDirectory $runRoot
    Assert-Success "runtime symbol inspection" $runtimeSymbols
    foreach ($required in @(
        "print",
        "vyx_string_alloc_abi",
        "vyx_ref_alloc_abi",
        "vyx_atomic_compare_exchange_i64",
        "vyx_pointer_checked_address_typed",
        "vyx_dict_str_i64_try_get"
    )) {
        if ($runtimeSymbols.Text -notmatch ("(?m)\b" + [regex]::Escape($required) + "$")) {
            throw "[sdk-runtime-contract] canonical runtime is missing ABI symbol '$required'"
        }
    }

    $runtimeUndefined = Invoke-Captured -FilePath $llvmNm -ArgumentList @(
        "--undefined-only", $runtimeArchive
    ) -WorkingDirectory $runRoot
    Assert-Success "runtime dependency inspection" $runtimeUndefined
    foreach ($forbidden in @("LLVM", "vyx_compiler_backend", "__cxa_", "__gxx_", "std::")) {
        if ($runtimeUndefined.Text -match [regex]::Escape($forbidden)) {
            throw "[sdk-runtime-contract] Vyx runtime has forbidden native dependency '$forbidden'"
        }
    }

    $compilerDependencyArgs = if ($runningOnWindows) {
        @("--coff-imports", $compilerInspectionTarget)
    } else {
        @("--needed-libs", $compilerInspectionTarget)
    }
    $compilerDependencies = Invoke-Captured -FilePath $llvmReadObj `
        -ArgumentList $compilerDependencyArgs -WorkingDirectory $runRoot
    Assert-Success "compiler dependency inspection" $compilerDependencies
    if ($compilerDependencies.Text -notmatch "vyx_compiler_backend") {
        throw "[sdk-runtime-contract] compiler does not import the canonical vyx_compiler_backend"
    }
    if ($compilerDependencies.Text -match "vyx_run_rt") {
        throw "[sdk-runtime-contract] compiler imports the legacy generated-program runtime"
    }

    foreach ($legacyName in @(
        "vyx_rt.dll", "vyx_run_rt.dll",
        "libvyx_rt.so", "libvyx_run_rt.so",
        "libvyx_rt.dylib", "libvyx_run_rt.dylib"
    )) {
        if (Test-Path -LiteralPath (Join-Path $runRoot $legacyName)) {
            throw "[sdk-runtime-contract] compiler staged legacy runtime artifact: $legacyName"
        }
    }

    $run = Invoke-Captured -FilePath $app -ArgumentList @() -WorkingDirectory $runRoot
    Assert-Success "application run" $run
    if ($run.Text.Trim() -cne "sdk-runtime-contract") {
        throw "[sdk-runtime-contract] unexpected application output: $($run.Text)"
    }

    $projectRoot = Join-Path $runRoot "project"
    $projectSrc = Join-Path $projectRoot "src"
    New-Item -ItemType Directory -Force -Path $projectSrc | Out-Null
    Copy-Item -LiteralPath (Join-Path $fixtureRoot "Vyx.toml") `
        -Destination (Join-Path $projectRoot "Vyx.toml")
    Copy-Item -LiteralPath $source -Destination (Join-Path $projectSrc "main.vyx")

    $projectBuild = Invoke-Captured -FilePath $compiler -ArgumentList @(
        "build", "-j2"
    ) -WorkingDirectory $projectRoot
    Assert-Success "project build" $projectBuild
    $projectApp = Join-Path (Join-Path $projectRoot "out") $appName
    if (-not (Test-Path -LiteralPath $projectApp -PathType Leaf)) {
        throw "[sdk-runtime-contract] project build did not produce $projectApp"
    }
    $projectDependencies = Invoke-Captured -FilePath $llvmReadObj `
        -ArgumentList $(if ($runningOnWindows) {
            @("--coff-imports", $projectApp)
        } else {
            @("--needed-libs", $projectApp)
        }) `
        -WorkingDirectory $projectRoot
    Assert-Success "project dependency inspection" $projectDependencies
    foreach ($forbidden in @("vyx_compiler_backend", "vyx_run_rt", "vyx_rt", "LLVM")) {
        if ($projectDependencies.Text -match [regex]::Escape($forbidden)) {
            throw "[sdk-runtime-contract] project application imports forbidden library '$forbidden'"
        }
    }
    $projectRun = Invoke-Captured -FilePath $projectApp -ArgumentList @() -WorkingDirectory $projectRoot
    Assert-Success "project application run" $projectRun
    if ($projectRun.Text.Trim() -cne "sdk-runtime-contract") {
        throw "[sdk-runtime-contract] unexpected project application output: $($projectRun.Text)"
    }

    Write-Host "sdk_runtime_contract: OK"
} finally {
    $env:Path = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdLibraryPath
    Remove-RunRoot
}

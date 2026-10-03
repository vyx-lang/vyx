param(
    [switch]$SkipBuild,
    [switch]$SkipSamples,
    [int]$TimeoutSec = 120
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$ws = Split-Path -Parent $root
$isWindowsPlatform = $env:OS -eq "Windows_NT"
$outDir = Join-Path $root "out\selfhost_fixpoint"
$logDir = Join-Path $outDir "logs"
$samplesDir = Join-Path $ws "samples/bootstrap"
$helper = Join-Path $PSScriptRoot "VyxTestProcess.ps1"
. $helper

if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }

function Test-RuntimeLibInDir {
    param([string]$Dir, [string]$Lib)
    return ((Test-Path (Join-Path $Dir ($Lib + ".lib"))) -or
            (Test-Path (Join-Path $Dir ("lib" + $Lib + ".dll.a"))) -or
            (Test-Path (Join-Path $Dir ("lib" + $Lib + ".so"))) -or
            (Test-Path (Join-Path $Dir ("lib" + $Lib + ".dylib"))))
}

function Find-VyxCompilerBackend {
    foreach ($cand in @(
        [pscustomobject]@{ Dir = (Join-Path $ws "bootstrap_compiler\out"); Lib = "vyx_compiler_backend" },
        [pscustomobject]@{ Dir = (Join-Path $ws "build_yolo_vyxcg\vyx_rt"); Lib = "vyx_rt" },
        [pscustomobject]@{ Dir = (Join-Path $ws "build_vyxcg\vyx_rt"); Lib = "vyx_rt" },
        [pscustomobject]@{ Dir = (Join-Path $ws "build\vyx_rt"); Lib = "vyx_rt" },
        [pscustomobject]@{ Dir = (Join-Path $ws "cmake-build-debug\vyx_codegen"); Lib = "vyx_rt" }
    )) {
        if (Test-RuntimeLibInDir $cand.Dir $cand.Lib) {
            return $cand
        }
    }
    return $null
}

function Ensure-CJsonObject {
    # A single-file self-host invocation does not read the bootstrap
    # Vyx.toml, so it cannot inherit the project's cJSON native source.
    # Compile that one declared native dependency once and pass it with
    # --link-obj for A->B/B->C.  This keeps the gate equivalent to the
    # project target without pretending cJSON is a system library.
    $source = @(
        (Join-Path $ws "third_party/cjson/cJSON.c"),
        (Join-Path $root "std_packages/json/vendor/cJSON.c")
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($source)) {
        throw "selfhost fixpoint requires cJSON.c (checked third_party/cjson and std_packages/json/vendor)"
    }

    $clang = $null
    $clangName = if ($isWindowsPlatform) { "clang.exe" } else { "clang" }
    if (-not [string]::IsNullOrWhiteSpace($env:LLVM_ROOT)) {
        $candidate = Join-Path $env:LLVM_ROOT (Join-Path "bin" $clangName)
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $clang = $candidate }
    }
    if ([string]::IsNullOrWhiteSpace($clang)) {
        $candidate = Join-Path $ws (Join-Path (Join-Path "clang" "bin") $clangName)
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $clang = $candidate }
    }
    if ([string]::IsNullOrWhiteSpace($clang)) {
        $command = Get-Command clang -ErrorAction SilentlyContinue
        if ($command) { $clang = $command.Source }
    }
    if ([string]::IsNullOrWhiteSpace($clang)) { throw "clang not found for cJSON selfhost link object" }

    $extension = if ($isWindowsPlatform) { ".obj" } else { ".o" }
    $object = Join-Path $outDir ("selfhost_cjson" + $extension)
    $sourceItem = Get-Item -LiteralPath $source
    $needsBuild = -not (Test-Path -LiteralPath $object -PathType Leaf)
    if (-not $needsBuild) {
        $needsBuild = (Get-Item -LiteralPath $object).LastWriteTimeUtc -lt $sourceItem.LastWriteTimeUtc
    }
    if ($needsBuild) {
        $clangArgs = @("-c", $source, "-I", (Split-Path -Parent $source), "-O2", "-o", $object)
        Write-Host "[selfhost] compiling native cJSON dependency"
        & $clang @clangArgs
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $object -PathType Leaf)) {
            throw "cJSON native object compilation failed: $source"
        }
    }
    return $object
}

function Invoke-CheckedVyx {
    param(
        [Parameter(Mandatory=$true)][string]$Exe,
        [Parameter(Mandatory=$true)][string[]]$Args,
        [Parameter(Mandatory=$true)][string]$Name,
        [int]$Seconds = $TimeoutSec
    )

    $safe = ($Name -replace '[^A-Za-z0-9_.-]', '_')
    $run = Invoke-VyxProcess -FilePath $Exe `
        -ArgumentList $Args `
        -WorkingDirectory $ws `
        -StdoutLog (Join-Path $logDir ($safe + ".out.log")) `
        -StderrLog (Join-Path $logDir ($safe + ".err.log")) `
        -DialogLog (Join-Path $logDir ($safe + ".dialog.log")) `
        -TimeoutSec $Seconds
    if ($run.ExitCode -ne 0) {
        Write-Host "[selfhost] FAIL $Name exit=$($run.ExitCode) timeout=$($run.TimedOut) dialog=$($run.DialogCaught)"
        Write-Host "[selfhost] stdout=$($run.StdoutLog)"
        Write-Host "[selfhost] stderr=$($run.StderrLog)"
        Write-Host "[selfhost] dialog=$($run.DialogLog)"
        exit 1
    }
}

function Assert-NonEmptyFile {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path $Path)) {
        Write-Host "[selfhost] FAIL missing $Label`: $Path"
        exit 1
    }
    $item = Get-Item $Path
    if ($item.Length -le 0) {
        Write-Host "[selfhost] FAIL empty $Label`: $Path"
        exit 1
    }
}

if (-not $SkipBuild) {
    Write-Host "[selfhost] building boot_a.exe"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[selfhost] FAIL build.ps1 exit=$LASTEXITCODE"
        exit 1
    }
}

$rt = Find-VyxCompilerBackend
if (-not $rt) {
    Write-Host "[selfhost] FAIL compiler backend library not found"
    exit 2
}
$env:Path = $rt.Dir + ";" + $env:Path
$rtArgs = @("-L", $rt.Dir, "-l", $rt.Lib)
$nativeArgs = @("--link-obj", (Ensure-CJsonObject))

$bootA = Join-Path $root "boot_a.exe"
$bootB = Join-Path $root "boot_b.exe"
$bootC = Join-Path $root "boot_c.exe"
Assert-NonEmptyFile $bootA "boot_a.exe"

$entry = Join-Path $root "src\main.vyx"
Write-Host "[selfhost] stage A -> B"
Invoke-CheckedVyx -Exe $bootA -Args (@("--src=file", $entry, "-o", $bootB) + $rtArgs + $nativeArgs) -Name "stage_a_to_b" -Seconds $TimeoutSec
Assert-NonEmptyFile $bootB "boot_b.exe"

Write-Host "[selfhost] stage B -> C"
Invoke-CheckedVyx -Exe $bootB -Args (@("--src=file", $entry, "-o", $bootC) + $rtArgs + $nativeArgs) -Name "stage_b_to_c" -Seconds $TimeoutSec
Assert-NonEmptyFile $bootC "boot_c.exe"

$fpDir = Join-Path $outDir "fixpoint"
if (-not (Test-Path $fpDir)) { New-Item -ItemType Directory -Force -Path $fpDir | Out-Null }
$fpA = Join-Path $fpDir "fp_a.ll"
$fpB = Join-Path $fpDir "fp_b.ll"
$fpC = Join-Path $fpDir "fp_c.ll"

Write-Host "[selfhost] emitting fixpoint IR"
Invoke-CheckedVyx -Exe $bootA -Args @("--emit=ir", "--src=file", $entry, "-o", $fpA) -Name "emit_fp_a" -Seconds $TimeoutSec
Invoke-CheckedVyx -Exe $bootB -Args @("--emit=ir", "--src=file", $entry, "-o", $fpB) -Name "emit_fp_b" -Seconds $TimeoutSec
Invoke-CheckedVyx -Exe $bootC -Args @("--emit=ir", "--src=file", $entry, "-o", $fpC) -Name "emit_fp_c" -Seconds $TimeoutSec
Assert-NonEmptyFile $fpA "fp_a.ll"
Assert-NonEmptyFile $fpB "fp_b.ll"
Assert-NonEmptyFile $fpC "fp_c.ll"

$hashA = (Get-FileHash $fpA -Algorithm SHA256).Hash
$hashB = (Get-FileHash $fpB -Algorithm SHA256).Hash
$hashC = (Get-FileHash $fpC -Algorithm SHA256).Hash
$lenA = (Get-Item $fpA).Length
$lenB = (Get-Item $fpB).Length
$lenC = (Get-Item $fpC).Length
if ($hashA -ne $hashB -or $hashB -ne $hashC -or $lenA -ne $lenB -or $lenB -ne $lenC) {
    Write-Host "[selfhost] FAIL fixpoint mismatch"
    Write-Host "[selfhost] fp_a len=$lenA sha=$hashA"
    Write-Host "[selfhost] fp_b len=$lenB sha=$hashB"
    Write-Host "[selfhost] fp_c len=$lenC sha=$hashC"
    exit 1
}
Write-Host "[selfhost] fixpoint OK bytes=$lenA sha256=$hashA"

if (-not $SkipSamples) {
    $samples = @(Get-ChildItem -Path $samplesDir -Filter "*.vyx" -File | Sort-Object Name)
    if ($samples.Count -eq 0) {
        Write-Host "[selfhost] FAIL no samples found in $samplesDir"
        exit 2
    }
    foreach ($stage in @(
        @{ Name = "boot_a"; Exe = $bootA },
        @{ Name = "boot_b"; Exe = $bootB },
        @{ Name = "boot_c"; Exe = $bootC }
    )) {
        $stageOut = Join-Path $outDir ($stage.Name + "_samples")
        if (-not (Test-Path $stageOut)) { New-Item -ItemType Directory -Force -Path $stageOut | Out-Null }
        Write-Host "[selfhost] $($stage.Name) samples=$($samples.Count)"
        foreach ($sample in $samples) {
            $ll = Join-Path $stageOut ($sample.BaseName + ".ll")
            Invoke-CheckedVyx -Exe $stage.Exe -Args @("--emit=ir", "--src=file", $sample.FullName, "-o", $ll) -Name ($stage.Name + "_" + $sample.BaseName) -Seconds $TimeoutSec
            Assert-NonEmptyFile $ll ($stage.Name + " sample " + $sample.Name)
        }
    }
    Write-Host "[selfhost] samples OK stages=3 count=$($samples.Count)"
}

Write-Host "[selfhost] PASS"
exit 0

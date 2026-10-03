[CmdletBinding()]
param(
    [string]$BootstrapCompiler = ".\\bootstrap_compiler\\out\\boot.exe",
    [int]$TimeoutSec = 120
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
$compilerPath = (Resolve-Path -LiteralPath (Join-Path $repoRoot $BootstrapCompiler)).Path
if (-not $env:LLVM_ROOT) {
    $clangRoot = Join-Path $repoRoot "clang"
    if (Test-Path -LiteralPath $clangRoot) {
        $env:LLVM_ROOT = (Resolve-Path -LiteralPath $clangRoot).Path
    }
}
if ($env:LLVM_ROOT) {
    $env:PATH = (Join-Path $env:LLVM_ROOT "bin") + [IO.Path]::PathSeparator + $env:PATH
}
$outDir = Join-Path $env:TEMP "vyx-tutorial-aot"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$examples = @(
    "tests/cases/tutorial_beginner_core.vyx",
    "tests/cases/tutorial_beginner_surface.vyx",
    "tests/cases/tutorial_intermediate_core.vyx",
    "tests/cases/tutorial_std_core.vyx",
    "tests/cases/tutorial_advanced_core.vyx",
    "tests/cases/tutorial_advanced_generics.vyx",
    "tests/cases/tutorial_error_handling.vyx",
    "tests/cases/tutorial_async_coroutine.vyx",
    "tests/cases/tutorial_layout_ops.vyx",
    "tests/cases/tutorial_reflect.vyx"
)

function Invoke-TutorialProcess {
    param([string]$WorkingDirectory, [string[]]$Arguments, [string]$Name)

    $process = Start-Process -FilePath $compilerPath -ArgumentList $Arguments `
        -WorkingDirectory $WorkingDirectory -NoNewWindow -PassThru -Wait
    if ($process.ExitCode -ne 0) {
        throw "tutorial example failed: $Name (exit=$($process.ExitCode))"
    }
}

foreach ($relative in $examples) {
    $source = Join-Path $repoRoot $relative
    $out = Join-Path $outDir ([IO.Path]::GetFileNameWithoutExtension($relative) + ".exe")
    Invoke-TutorialProcess -WorkingDirectory $repoRoot `
        -Arguments @("--src=file", $source, "--emit=exe", "-o", $out) -Name $relative
    $runFile = Start-Process -FilePath $out -WorkingDirectory $repoRoot -NoNewWindow -PassThru -Wait
    if ($runFile.ExitCode -ne 0) {
        throw "tutorial example failed: $relative (run exit=$($runFile.ExitCode))"
    }
}

$projectDir = Join-Path $repoRoot "tests/projects/tutorial_manifest"
Invoke-TutorialProcess -WorkingDirectory $projectDir -Arguments @("build", "-j1") -Name "tutorial_manifest build"
$program = Join-Path $projectDir "target/tutorial_manifest.exe"
if (-not (Test-Path -LiteralPath $program)) {
    throw "tutorial manifest output missing: $program"
}
$run = Start-Process -FilePath $program -WorkingDirectory $projectDir -NoNewWindow -PassThru -Wait
if ($run.ExitCode -ne 0) { throw "tutorial manifest output failed: exit=$($run.ExitCode)" }

$migrateDir = Join-Path $repoRoot "tests/projects/tutorial_migrate"
$migrateOut = Join-Path $outDir "tutorial_migrate.exe"
Invoke-TutorialProcess -WorkingDirectory $migrateDir `
    -Arguments @("--src=project", $migrateDir, "--emit=exe", "-o", $migrateOut) -Name "tutorial_migrate"
$runMigrate = Start-Process -FilePath $migrateOut -WorkingDirectory $migrateDir -NoNewWindow -PassThru -Wait
if ($runMigrate.ExitCode -ne 0) { throw "tutorial migrate output failed: exit=$($runMigrate.ExitCode)" }

Write-Host "tutorial_examples: $($examples.Count) file example(s), manifest project, and migrate project passed."

[CmdletBinding()]
param(
    [string]$BootstrapCompiler = ".\bootstrap_compiler\out\boot.exe"
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

$outDir = Join-Path $env:TEMP "vyx-tutorial-probes"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$probes = @(
    "probes/gates/tutorial/nested_generic_args.vyx",
    "probes/gates/tutorial/labeled_break.vyx",
    "probes/gates/tutorial/ctor_self.vyx",
    "probes/gates/tutorial/ctor_zero.vyx",
    "probes/gates/tutorial/ctor_return_literal.vyx",
    "tests/cases/tutorial_reflect.vyx"
)

foreach ($relative in $probes) {
    $source = Join-Path $repoRoot $relative
    $out = Join-Path $outDir ([IO.Path]::GetFileNameWithoutExtension($relative) + ".exe")
    $proc = Start-Process -FilePath $compilerPath `
        -ArgumentList @("--src=file", $source, "--emit=exe", "-o", $out) `
        -WorkingDirectory $repoRoot -NoNewWindow -PassThru -Wait
    if ($proc.ExitCode -ne 0) {
        Write-Host "FAIL: $relative emit exit=$($proc.ExitCode)"
        exit $proc.ExitCode
    }
    $run = Start-Process -FilePath $out -WorkingDirectory $repoRoot -NoNewWindow -PassThru -Wait
    if ($run.ExitCode -ne 0) {
        Write-Host "FAIL: $relative run exit=$($run.ExitCode)"
        exit $run.ExitCode
    }
    Write-Host "OK: $relative"
}

Write-Host "OK: tutorial probes AOT"

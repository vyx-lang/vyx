param(
    [string]$Compiler = '',
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$source = Join-Path $repo 'tests/projects/industrial_mir_stress'
$run = Join-Path $repo ('tests/.cache/industrial_mir_cgu_' + [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'Vyx.toml') -Destination $run
Copy-Item -LiteralPath (Join-Path $source 'src') -Destination $run -Recurse

$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = (Join-Path $env:LLVM_ROOT 'bin') + [IO.Path]::PathSeparator + $env:PATH
$env:VYX_CODEGEN_UNITS = '4'
$env:VYX_CGU_PARALLEL = 'lazy'
$env:VYX_CGU_THREADS = '4'
$env:VYX_CGU_PHASES = '1'
Push-Location $run
try {
    & $Compiler build --target industrial_mir_stress "-j$Jobs" *> build.log
    if ($LASTEXITCODE -ne 0) { throw "Multi-CGU build failed; see $run/build.log" }
    $list = '.cache/crate_industrial_mir_stress.obj.cgu.list'
    if (!(Test-Path -LiteralPath $list)) { throw 'Compiler did not emit a CGU object list' }
    $objects = @(Get-Content -LiteralPath $list | Where-Object { $_.Trim() })
    if ($objects.Count -lt 4) { throw "Expected four CGU objects, got $($objects.Count)" }
    foreach ($object in $objects) {
        if (!(Test-Path -LiteralPath $object)) { throw "Missing CGU object: $object" }
    }
    & '.\target\industrial_mir_stress.exe' *> run.log
    if ($LASTEXITCODE -ne 0) { throw "Multi-CGU executable failed; see $run/run.log" }
    if (!((Get-Content -LiteralPath run.log -Raw).Contains('industrial_mir_stress OK'))) {
        throw "Executable did not report success; see $run/run.log"
    }
    Write-Output "industrial_mir_cgu OK: four CGUs linked and ran ($run)"
} finally {
    Pop-Location
}

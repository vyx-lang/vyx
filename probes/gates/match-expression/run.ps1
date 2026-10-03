param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Compiler) { $Compiler = Join-Path $root 'bootstrap_compiler\out\boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$outDir = Join-Path $root 'bootstrap_compiler\out'
$run = Join-Path $PSScriptRoot '.runs'
New-Item -ItemType Directory -Force $run | Out-Null
$exe = Join-Path $run 'match_expression.exe'
$src = Join-Path $PSScriptRoot 'match_expression.vyx'
& $Compiler --src=file $src --emit=exe -o $exe -L $outDir -l vyx_runtime *> (Join-Path $run 'compile.log')
if ($LASTEXITCODE -ne 0) { Write-Host 'match-expression FAIL: compile'; Get-Content (Join-Path $run 'compile.log'); exit 1 }
& $exe
if ($LASTEXITCODE -ne 0) { Write-Host "match-expression FAIL: exit=$LASTEXITCODE"; exit 1 }

function Assert-CompileFailure([string]$Label, [string]$Source) {
    $badExe = Join-Path $run ("negative_" + $Label + '.exe')
    $badLog = Join-Path $run ("negative_" + $Label + '.log')
    & $Compiler --src=file $Source --emit=exe -o $badExe -L $outDir -l vyx_runtime *> $badLog
    $badExit = $LASTEXITCODE
    if ($badExit -eq 0) {
        Write-Host "match-expression FAIL: $Label unexpectedly compiled"
        Get-Content $badLog
        exit 1
    }
    if (-not (Select-String -Path $badLog -Pattern 'E1000' -Quiet)) {
        Write-Host "match-expression FAIL: $Label missing E1000 diagnostic"
        Get-Content $badLog
        exit 1
    }
}

Assert-CompileFailure 'type_mismatch' (Join-Path $PSScriptRoot 'negative_type_mismatch.vyx')
Assert-CompileFailure 'guard_type' (Join-Path $PSScriptRoot 'negative_guard_type.vyx')
Assert-CompileFailure 'missing_value' (Join-Path $PSScriptRoot 'negative_missing_value.vyx')
Assert-CompileFailure 'empty' (Join-Path $PSScriptRoot 'negative_empty.vyx')
Write-Host 'match-expression OK'
exit 0

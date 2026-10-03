param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [string]$RuntimeDir = '',
    [int[]]$OptimizationLevels = @(0, 2),
    [int[]]$CodegenUnits = @(1, 4)
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
if (-not $RuntimeDir) { $RuntimeDir = Split-Path -Parent $Compiler }
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$runDir = Join-Path $PSScriptRoot '.runs'
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
. (Join-Path $repoRoot 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$identity = @{ compiler = $Compiler; sha256 = (Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash; runtime = $RuntimeDir; optimization_levels = $OptimizationLevels; codegen_units = $CodegenUnits }
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDir 'compiler.json') -Encoding utf8
Write-Host "compiler=$Compiler sha256=$($identity.sha256)"
$failures = 0
$cases = @(
    @{ Name = 'derived_trait'; Expected = "42`n" },
    @{ Name = 'derived_multilevel'; Expected = "42`n" },
    @{ Name = 'index_print'; Expected = "-6789`ny=-6789 direct=-6789`n-6789`n-6789`nbeta`nword=beta direct=beta`nbeta`n12.5`nfraction=12.5`nflag=true`n1.25`nsmall=1.25`nunsigned=18446744073709551615`n" },
    @{ Name = 'interpolation'; Expected = "has=true`nbrace=} quote=`"`nnested=inside=ok`nescaped=`${literal} char=125 comment=3`n" },
    @{ Name = 'async_stream'; Expected = "42`n" },
    @{ Name = 'async_params'; Expected = "42`n" },
    @{ Name = 'async_values'; Expected = "42`n" },
    @{ Name = 'async_payload'; Expected = "42`n" }
)
$originalUnits = $env:VYX_CODEGEN_UNITS
try {
    foreach ($opt in $OptimizationLevels) {
        foreach ($units in $CodegenUnits) {
            $env:VYX_CODEGEN_UNITS = [string]$units
            $variant = "O$opt-cgu$units"
            $variantDir = Join-Path $runDir $variant
            New-Item -ItemType Directory -Force -Path $variantDir | Out-Null
            foreach ($case in $cases) {
                $name = $case.Name
                $exe = Join-Path $variantDir "$name.exe"
                $source = Join-Path $PSScriptRoot "$name.vyx"
                $compile = Invoke-VyxProcess -FilePath $Compiler `
                    -ArgumentList @('--src=file', $source, '--emit=exe', "-O$opt", '-j', '2', '-o', $exe, '-L', $RuntimeDir, '-l', 'vyx_runtime') `
                    -WorkingDirectory $repoRoot -TimeoutSec 120 `
                    -StdoutLog (Join-Path $variantDir "$name.compile.stdout.log") `
                    -StderrLog (Join-Path $variantDir "$name.compile.stderr.log") `
                    -DialogLog (Join-Path $variantDir "$name.compile.dialog.log")
                if ($compile.ExitCode -ne 0) {
                    Write-Host "$variant $name FAIL: compile exit=$($compile.ExitCode)"
                    Get-Content -LiteralPath $compile.StdoutLog
                    Get-Content -LiteralPath $compile.StderrLog
                    $failures++
                    continue
                }
                $run = Invoke-VyxProcess -FilePath $exe -WorkingDirectory $repoRoot -TimeoutSec 15 `
                    -StdoutLog (Join-Path $variantDir "$name.stdout.log") `
                    -StderrLog (Join-Path $variantDir "$name.stderr.log") `
                    -DialogLog (Join-Path $variantDir "$name.dialog.log")
                $output = [IO.File]::ReadAllText($run.StdoutLog).Replace("`r`n", "`n")
                if ($run.ExitCode -ne 0 -or $output -cne $case.Expected) {
                    Write-Host "$variant $name FAIL: run exit=$($run.ExitCode), stdout=$(ConvertTo-Json -InputObject $output -Compress)"
                    $failures++
                    continue
                }
                Write-Host "$variant $name OK"
            }
        }
    }
} finally { $env:VYX_CODEGEN_UNITS = $originalUnits }
foreach ($case in @(
    @{ Name = 'interpolation_invalid'; Diagnostic = 'E0001: unexpected token after interpolation expression'; Location = ':2:20:' },
    @{ Name = 'interpolation_unterminated'; Diagnostic = 'E0102: unterminated string interpolation'; Location = ':2:11:' }
)) {
    $name = $case.Name
    $compile = Invoke-VyxProcess -FilePath $Compiler `
        -ArgumentList @('--src=file', (Join-Path $PSScriptRoot "$name.vyx"), '--emit=exe', '-o', (Join-Path $runDir "$name.exe"), '-L', $RuntimeDir, '-l', 'vyx_runtime') `
        -WorkingDirectory $repoRoot -TimeoutSec 30 `
        -StdoutLog (Join-Path $runDir "$name.compile.stdout.log") `
        -StderrLog (Join-Path $runDir "$name.compile.stderr.log") `
        -DialogLog (Join-Path $runDir "$name.compile.dialog.log")
    $diagnostic = [IO.File]::ReadAllText($compile.StdoutLog) + [IO.File]::ReadAllText($compile.StderrLog)
    if ($compile.TimedOut -or $compile.ExitCode -ne 1 -or -not $diagnostic.Contains($case.Diagnostic) -or -not $diagnostic.Contains($case.Location)) {
        Write-Host "$name FAIL: expected located syntax error, exit=$($compile.ExitCode)"
        Write-Host $diagnostic
        $failures++
    } else { Write-Host "$name OK: rejected at source location" }
}
if ($failures -gt 0) { exit 1 }
exit 0

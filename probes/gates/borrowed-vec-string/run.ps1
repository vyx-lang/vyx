param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [int[]]$OptimizationLevels = @(0, 2),
    [int[]]$CodegenUnits = @(1, 4)
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$project = Join-Path $repoRoot 'tests/projects/borrowed_vec_ownership'
$results = Join-Path $PSScriptRoot '.runs'
New-Item -ItemType Directory -Force -Path $results | Out-Null
. (Join-Path $repoRoot 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
@{ compiler=$Compiler; sha256=(Get-FileHash $Compiler -Algorithm SHA256).Hash } |
    ConvertTo-Json | Set-Content (Join-Path $results 'compiler.json') -Encoding utf8
$failed = 0
$previousUnits = $env:VYX_CODEGEN_UNITS
try {
    foreach ($opt in $OptimizationLevels) {
        foreach ($units in $CodegenUnits) {
            $env:VYX_CODEGEN_UNITS = [string]$units
            $variant = "O$opt-cgu$units"
            $output = Join-Path $results $variant
            New-Item -ItemType Directory -Force -Path $output | Out-Null
            $exe = Join-Path $output 'ownership.exe'
            $build = Invoke-VyxProcess -FilePath $Compiler -WorkingDirectory $project -TimeoutSec 180 `
                -ArgumentList @('--src=project', '.', '--emit=exe', "-O$opt", '-j', '2', '-o', $exe) `
                -StdoutLog (Join-Path $output 'build.stdout.log') -StderrLog (Join-Path $output 'build.stderr.log')
            if ($build.TimedOut -or $build.ExitCode -ne 0) {
                Write-Host "$variant FAIL build exit=$($build.ExitCode)"
                Get-Content $build.StdoutLog
                Get-Content $build.StderrLog
                $failed++
                continue
            }
            $run = Invoke-VyxProcess -FilePath $exe -WorkingDirectory $project -TimeoutSec 30 `
                -StdoutLog (Join-Path $output 'run.stdout.log') -StderrLog (Join-Path $output 'run.stderr.log')
            $stdout = [IO.File]::ReadAllText($run.StdoutLog).Replace("`r`n", "`n")
            if ($run.TimedOut -or $run.ExitCode -ne 0 -or $stdout -cne "BORROWED_VEC_STRING_OK`n") {
                Write-Host "$variant FAIL run exit=$($run.ExitCode) stdout=$stdout"
                $failed++
            } else { Write-Host "$variant OK" }
        }
    }
} finally { $env:VYX_CODEGEN_UNITS = $previousUnits }
foreach ($source in Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'moved_*.vyx') {
    $name = $source.BaseName
    $compile = Invoke-VyxProcess -FilePath $Compiler -WorkingDirectory $repoRoot -TimeoutSec 30 `
        -ArgumentList @('--src=file', $source.FullName, '--stop-after-sema') `
        -StdoutLog (Join-Path $results "$name.stdout.log") -StderrLog (Join-Path $results "$name.stderr.log")
    $diagnostics = [IO.File]::ReadAllText($compile.StdoutLog) + [IO.File]::ReadAllText($compile.StderrLog)
    if ($compile.TimedOut -or $compile.ExitCode -ne 1 -or $diagnostics -notmatch 'E3100:' -or
        $diagnostics -notmatch ([regex]::Escape($source.Name) + ':\d+:\d+: error:')) {
        Write-Host "$name FAIL: expected located E3100, exit=$($compile.ExitCode)"
        Write-Host $diagnostics
        $failed++
    } else { Write-Host "$name OK: E3100 at source location" }
}
if ($failed -gt 0) { exit 1 }
exit 0

param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference = 'Stop'
$Compiler = (Resolve-Path $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$savedPath = $env:PATH
$savedHoist = $env:VYX_REGION_HOIST
try {
    $env:PATH = (Split-Path $Compiler) + ';' + $env:PATH
    foreach ($hoist in @('0', '1')) {
        $env:VYX_REGION_HOIST = $hoist
        foreach ($opt in @('-O0', '-O2')) {
            $exe = Join-Path $run ($opt.Substring(1) + '-hoist' + $hoist + '.exe')
            & $Compiler --src=file (Join-Path $PSScriptRoot 'positive.vyx') --emit=exe $opt -o $exe *> ($exe + '.compile.log')
            if ($LASTEXITCODE -ne 0) { throw "Receiver borrow $opt hoist=$hoist compilation failed" }
            & $exe *> ($exe + '.run.log')
            if ($LASTEXITCODE -ne 0) { throw "Receiver borrow $opt hoist=$hoist execution failed: $LASTEXITCODE" }
        }
    }
    Write-Host "PASS: shared/mutable owning-receiver borrows and inline locals at O0/O2, hoist off/on; $run"
} finally {
    $env:PATH = $savedPath
    $env:VYX_REGION_HOIST = $savedHoist
}

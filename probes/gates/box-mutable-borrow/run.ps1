param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path $Compiler) + ';' + $env:PATH
    foreach ($opt in @('-O0', '-O2')) {
        $exe = Join-Path $run ($opt.Substring(1) + '.exe')
        & $Compiler --src=file (Join-Path $PSScriptRoot 'positive.vyx') --emit=exe $opt -o $exe *> ($exe + '.compile.log')
        if ($LASTEXITCODE -ne 0) { throw "Positive $opt compilation failed" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "Positive $opt execution failed: $LASTEXITCODE" }
    }
    foreach ($source in @('shared_negative.vyx', 'shared_vec_negative.vyx')) {
        $negativeLog = Join-Path $run ($source + '.log')
        & $Compiler --src=file (Join-Path $PSScriptRoot $source) --emit=exe -o (Join-Path $run 'negative.exe') *> $negativeLog
        if ($LASTEXITCODE -eq 0 -or (Get-Content $negativeLog -Raw) -notmatch 'E3101') {
            throw "Mutable access through a shared borrow was not rejected: $source"
        }
    }
    Write-Host "PASS: concrete/dynamic Box and Vec mutation at O0/O2; shared access rejected; $run"
} finally { $env:PATH = $savedPath }

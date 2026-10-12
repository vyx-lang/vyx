param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/policy-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$implementation = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/project/build_resources.vyx'))
$implementation = [regex]::Replace($implementation, '(?m)^module [^;]+;\r?\n', '')
$probe = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'policy.vyx'))
[IO.File]::WriteAllText((Join-Path $run 'policy.vyx'), $implementation + "`n" + $probe, [Text.UTF8Encoding]::new($false))
$compilerHash = (Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash
foreach ($optimization in @('0','2')) {
    $output = Join-Path $run ('policy-O'+$optimization+'.exe')
    & $Compiler --src=file (Join-Path $run 'policy.vyx') --emit=exe -o $output "-O$optimization" *> (Join-Path $run ('compile-O'+$optimization+'.log'))
    if ($LASTEXITCODE -ne 0) { throw "Policy compilation failed at O$optimization" }
    & $output | Tee-Object (Join-Path $run ('run-O'+$optimization+'.log'))
    if ($LASTEXITCODE -ne 0) { throw "Policy assertions failed at O$optimization" }
}
Write-Output "Compiler SHA256=$compilerHash; O0/O2 PASS; evidence=$run"

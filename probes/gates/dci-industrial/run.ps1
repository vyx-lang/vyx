param(
    [string]$Compiler = '',
    [int]$Repeat = 1,
    [int]$Scale = 1,
    [int]$Parallel = 1,
    [int]$TimeoutSec = 1800,
    [string[]]$Case = @()
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$python = (Get-Command python -ErrorAction Stop).Source
$arguments = @((Join-Path $PSScriptRoot 'run.py'), '--compiler', $Compiler,
    '--repeat', $Repeat, '--scale', $Scale, '--parallel', $Parallel,
    '--timeout-sec', $TimeoutSec)
foreach ($name in $Case) { $arguments += @('--case', $name) }
& $python @arguments
if ($LASTEXITCODE -ne 0) { throw "dci-industrial failed with exit $LASTEXITCODE" }

# Quick smoke: tests/cases/smoke/common/common_*.vyx only（委托 tests/run_all_modules.ps1）
param(
    [string]$HostCompiler = "",
    [string]$BootstrapCompiler = "",
    [switch]$HostOnly,
    [switch]$BootstrapOnly
)

$delegate = Join-Path $PSScriptRoot "..\..\..\run_all_modules.ps1"
& $delegate -HostCompiler:$HostCompiler -BootstrapCompiler:$BootstrapCompiler `
    -HostOnly:$HostOnly -BootstrapOnly:$BootstrapOnly -PathFilter "tests\cases\smoke\common\common_"

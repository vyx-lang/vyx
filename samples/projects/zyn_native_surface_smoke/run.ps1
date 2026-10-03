param(
    [string]$Compiler,
    [ValidateSet('d3d12', 'vulkan')][string]$Backend = 'd3d12',
    [ValidateRange(1, 20)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$oldCompiler = $env:ZYN_VYXC
$oldBackend = $env:CACAO_BACKEND
Push-Location $PSScriptRoot
try {
    $env:ZYN_VYXC = $Compiler
    # The SDK entry point selects compiler libraries and stages Cacao/SDL assets.
    & (Join-Path $repo 'Zyn/bin/zyn.ps1') build "-j$Jobs"
    if ($LASTEXITCODE -ne 0) { throw 'Native surface smoke build failed.' }
    $env:CACAO_BACKEND = $Backend
    $output = & './target/zyn_native_surface_smoke.exe' 2>&1
    $runExit = $LASTEXITCODE
    $output | Set-Content -LiteralPath "target/native-surface-$Backend.log" -Encoding utf8
    $output | Write-Output
    @{
        backend = $Backend
        exit_code = $runExit
        compiler_sha256 = (Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash
        cacao_sha256 = (Get-FileHash -LiteralPath 'target/Cacao.dll' -Algorithm SHA256).Hash
        zyn_sha256 = (Get-FileHash -LiteralPath 'target/Zyn.dll' -Algorithm SHA256).Hash
    } | ConvertTo-Json | Set-Content -LiteralPath "target/native-surface-$Backend.json" -Encoding utf8
    if ($runExit -ne 0) { throw "Native surface smoke failed ($Backend): $runExit" }
} finally {
    Pop-Location
    $env:ZYN_VYXC = $oldCompiler
    $env:CACAO_BACKEND = $oldBackend
}

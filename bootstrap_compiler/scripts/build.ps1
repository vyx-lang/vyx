param(
    [string]$Compiler = $env:VYX_BOOTSTRAP_VYXC,
    [string]$Target = 'boot',
    [ValidateRange(1,256)][int]$Jobs = 1
)
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Compiler) { $Compiler = (Get-Command vyxc -ErrorAction Stop).Source }
$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
if (-not $env:LLVM_ROOT) {
    $env:LLVM_ROOT = (Resolve-Path (Join-Path $project '../clang')).Path
}
Write-Host "Stage 0: $Compiler"
& $Compiler --version
if ($LASTEXITCODE -ne 0) { throw 'Cannot run Stage 0 compiler' }
Get-FileHash -LiteralPath $Compiler -Algorithm SHA256 | Format-List
Push-Location $project
try {
    & $Compiler build --target $Target "-j$Jobs"
    $code = $LASTEXITCODE
} finally { Pop-Location }
exit $code

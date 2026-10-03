$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $project '..\..\..')).Path
$boot = Join-Path $repo 'bootstrap_compiler\out\boot.exe'
$outputDir = Join-Path $repo 'bootstrap_compiler\out'
foreach ($case in @('implicit_impl', 'explicit_impl')) {
    $src = Join-Path $project "$case.vyx"
    $obj = Join-Path $outputDir "generic_impl_scope_$case.obj"
    & $boot --src=file $src --emit=obj -o $obj
    if ($LASTEXITCODE -ne 0) { throw "$case failed" }
}
$hashSource = Join-Path $repo 'bootstrap_compiler\std_packages\core\src\hash.vyx'
$hashInterface = Join-Path $outputDir 'generic_impl_scope_hash.vyi'
& $boot --src=file $hashSource --emit=vyi --vyi-shallow -o $hashInterface
if ($LASTEXITCODE -ne 0) { throw 'shallow hash interface failed' }
$interface = Get-Content -LiteralPath $hashInterface -Raw
foreach ($expected in @('impl Hashable for i32 {',
                       'impl Eq for i32 {',
                       'public fn operator_eq(self, other: i32) -> bool;',
                       'impl Eq for char {',
                       'public fn operator_eq(self, other: char) -> bool;')) {
    if (-not $interface.Contains($expected)) { throw "missing shallow interface fact: $expected" }
}
Write-Host 'generic_impl_scope: OK'

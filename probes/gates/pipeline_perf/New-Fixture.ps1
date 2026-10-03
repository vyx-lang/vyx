param(
    [ValidateRange(64, 1024)][int]$SourceCount = 64,
    [string]$OutputDir = (Join-Path $PSScriptRoot ('.runs/fixture-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
)

$ErrorActionPreference = 'Stop'
$runsRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.runs'))
$fixtureDir = [IO.Path]::GetFullPath($OutputDir)
if (-not $fixtureDir.StartsWith($runsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Generated fixtures must be beneath pipeline_perf/.runs/'
}
if (Test-Path -LiteralPath $fixtureDir) { throw "Refusing to overwrite existing fixture: $fixtureDir" }
New-Item -ItemType Directory -Path (Join-Path $fixtureDir 'src') -Force | Out-Null

$sources = [Collections.Generic.List[string]]::new()
$imports = [Collections.Generic.List[string]]::new()
$calls = [Collections.Generic.List[string]]::new()
for ($index = 0; $index -lt $SourceCount; $index++) {
    $suffix = $index.ToString('D3')
    $sources.Add('"src/unit' + $suffix + '.vyx"')
    if ($index -eq 0) {
        $imports.Add('use PipelinePerf.Unit' + $suffix + ';')
        $calls.Add('    result = compute' + $suffix + '(result);')
    }
    $steps = for ($step = 0; $step -lt 16; $step++) {
        '    value = (value + ' + ($step + $index + 1) + ') % 1000003;'
    }
    $source = @"
module PipelinePerf.Unit$suffix;

public fn compute$suffix(seed: i64) -> i64 {
    var value = seed;
$($steps -join "`n")
    return value;
}
"@
    Set-Content -LiteralPath (Join-Path $fixtureDir "src/unit$suffix.vyx") -Value $source -Encoding utf8NoBOM
}
$main = @"
module PipelinePerf;
$($imports -join "`n")

fn main() -> i32 {
    var result: i64 = 0;
$($calls -join "`n")
    if (result != 136) { return 1; }
    return 0;
}
"@
Set-Content -LiteralPath (Join-Path $fixtureDir 'src/main.vyx') -Value $main -Encoding utf8NoBOM
$manifest = @"
[package]
name = "pipeline_perf"
version = "0.1.0"
entry = "src/main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
mir_partition = true

[target.pipeline_perf]
type = "executable"
entry = "src/main.vyx"
sources = [$($sources -join ', ')]
"@
Set-Content -LiteralPath (Join-Path $fixtureDir 'Vyx.toml') -Value $manifest -Encoding utf8NoBOM
[ordered]@{ source_count = $SourceCount; units_including_main = $SourceCount + 1; generated_utc = [DateTime]::UtcNow.ToString('o') } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixtureDir 'fixture.json') -Encoding utf8NoBOM
Write-Output $fixtureDir

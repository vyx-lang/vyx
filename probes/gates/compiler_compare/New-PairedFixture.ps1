param(
    [ValidateRange(4, 4096)][int]$UnitCount = 64,
    [ValidateSet('Flat', 'Layered')][string]$Layout = 'Layered',
    [ValidateRange(2, 128)][int]$Fanout = 8,
    [ValidateRange(4, 256)][int]$Rounds = 24,
    [string]$OutputDir = (Join-Path $PSScriptRoot ('.runs/paired-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
)

$ErrorActionPreference = 'Stop'
$fixture = [IO.Path]::GetFullPath($OutputDir)
$runsRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.runs'))
if (-not $fixture.StartsWith($runsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Generated fixtures must remain beneath compiler_compare/.runs/'
}
if (Test-Path -LiteralPath $fixture) { throw "Refusing to overwrite fixture: $fixture" }
foreach ($relative in @('vyx/src', 'cpp/src', 'cpp/include')) {
    New-Item -ItemType Directory -Path (Join-Path $fixture $relative) -Force | Out-Null
}
function Write-Source([string]$Relative, [string]$Content) {
    [IO.File]::WriteAllText((Join-Path $fixture $Relative), $Content.Replace("`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
}
function Expected-Value([long]$Seed, [int]$Index) {
    [long]$value = ($Seed + $Index * 17) % 1000003
    [long]$bias = ($Seed % 97) + 17
    for ($round = 0; $round -lt $Rounds; $round++) {
        $value = ($value * 33 + $bias + $round + $Index + 1) % 1000003
    }
    return $value
}

$sources = [Collections.Generic.List[string]]::new()
$symbols = [Collections.Generic.List[string]]::new()
$headers = [Collections.Generic.List[string]]::new()
$sources.Add('shared')
$symbols.Add('compare_bias')
$headers.Add('shared')
Write-Source 'vyx/src/shared.vyx' @'
module CompilerCompare.Shared;

public fn compare_bias(seed: i64) -> i64 {
    return (seed % 97) + 17;
}
'@
Write-Source 'cpp/include/shared.hpp' @'
#pragma once
using i64 = long long;
static_assert(sizeof(i64) == 8);
i64 compare_bias(i64 seed);
'@
Write-Source 'cpp/src/shared.cpp' @'
#include "shared.hpp"
i64 compare_bias(i64 seed) {
    return (seed % 97) + 17;
}
'@

$leafNodes = [Collections.Generic.List[object]]::new()
$seeds = @(0, 17, 991)
for ($index = 0; $index -lt $UnitCount; $index++) {
    $suffix = $index.ToString('D4')
    $stem = "unit$suffix"
    $symbol = "compute$suffix"
    $sources.Add($stem)
    $headers.Add($stem)
    $symbols.Add($symbol)
    $steps = for ($round = 0; $round -lt $Rounds; $round++) {
        "    value = (value * 33 + bias + $($round + $index + 1)) % 1000003;"
    }
    Write-Source "vyx/src/$stem.vyx" @"
module CompilerCompare.Unit$suffix;
use CompilerCompare.Shared;

public fn $symbol(seed: i64) -> i64 {
    let bias = compare_bias(seed);
    var value = (seed + $($index * 17)) % 1000003;
$($steps -join "`n")
    return value;
}
"@
    Write-Source "cpp/include/$stem.hpp" @"
#pragma once
#include "shared.hpp"
i64 $symbol(i64 seed);
"@
    Write-Source "cpp/src/$stem.cpp" @"
#include "$stem.hpp"
i64 $symbol(i64 seed) {
    const i64 bias = compare_bias(seed);
    i64 value = (seed + $($index * 17)) % 1000003;
$($steps -join "`n")
    return value;
}
"@
    $checks = foreach ($seed in $seeds) {
        "    if ($symbol($seed) != $(Expected-Value $seed $index)) { return 1; }"
    }
    $leafNodes.Add([pscustomobject]@{ module = "CompilerCompare.Unit$suffix"; header = "$stem.hpp"; checks = @($checks) })
}

$nodes = @($leafNodes)
$groupCount = 0
$level = 0
if ($Layout -eq 'Layered') {
    while ($nodes.Count -gt $Fanout) {
        $parents = [Collections.Generic.List[object]]::new()
        for ($start = 0; $start -lt $nodes.Count; $start += $Fanout) {
            $children = @($nodes[$start..([Math]::Min($start + $Fanout - 1, $nodes.Count - 1))])
            $suffix = $groupCount.ToString('D4')
            $stem = "group$suffix"
            $symbol = "verify_group$suffix"
            $sources.Add($stem)
            $headers.Add($stem)
            $symbols.Add($symbol)
            $checks = @($children | ForEach-Object { $_.checks }) -join "`n"
            $imports = @($children | ForEach-Object { "use $($_.module);" }) -join "`n"
            $includes = @($children | ForEach-Object { '#include "' + $_.header + '"' }) -join "`n"
            Write-Source "vyx/src/$stem.vyx" @"
module CompilerCompare.Group$suffix;
$imports

public fn $symbol() -> i32 {
$checks
    return 0;
}
"@
            Write-Source "cpp/include/$stem.hpp" @"
#pragma once
int $symbol();
"@
            Write-Source "cpp/src/$stem.cpp" @"
#include "$stem.hpp"
$includes
int $symbol() {
$checks
    return 0;
}
"@
            $parents.Add([pscustomobject]@{module = "CompilerCompare.Group$suffix"; header = "$stem.hpp"; checks = @("    if ($symbol() != 0) { return 1; }")})
            $groupCount++
        }
        $nodes = @($parents)
        $level++
    }
}
$imports = @($nodes | ForEach-Object { "use $($_.module);" }) -join "`n"
$includes = @($nodes | ForEach-Object { '#include "' + $_.header + '"' }) -join "`n"
$checks = @($nodes | ForEach-Object { $_.checks }) -join "`n"
Write-Source 'vyx/src/main.vyx' @"
module CompilerCompare;
$imports

fn main() -> i32 {
$checks
    return 0;
}
"@
Write-Source 'cpp/src/main.cpp' @"
$includes
int main() {
$checks
    return 0;
}
"@
$sourceList = @($sources | ForEach-Object { '"src/' + $_ + '.vyx"' }) -join ', '
Write-Source 'vyx/Vyx.toml' @"
[package]
name = "compiler_compare"
version = "0.1.0"
entry = "src/main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
mir_partition = true

[target.compiler_compare]
type = "executable"
entry = "src/main.vyx"
sources = [$sourceList]
"@
$cppSources = @(@('src/main.cpp') + @($sources | ForEach-Object { 'src/' + $_ + '.cpp' })) -join "`n    "
Write-Source 'cpp/CMakeLists.txt' @"
cmake_minimum_required(VERSION 3.20)
project(compiler_compare LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_executable(compiler_compare
    $cppSources
)
target_include_directories(compiler_compare PRIVATE include)
"@
$sourceHashes = @(Get-ChildItem -LiteralPath $fixture -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=[IO.Path]::GetRelativePath($fixture,$_.FullName); sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash}
})
[ordered]@{
    schema_version = 1
    created_utc = [DateTime]::UtcNow.ToString('o')
    leaf_units = $UnitCount
    layout = $Layout
    fanout = $Fanout
    aggregation_units = $groupCount
    aggregation_levels = $level
    translation_units = $sources.Count + 1
    rounds = $Rounds
    seeds = $seeds
    expected_symbol_names = @($symbols)
    algorithm = 'i64 positive modular recurrence; each leaf is called and checked for all three seeds; main reaches every check.'
    arithmetic_bound = 'All seeds and recurrence inputs are nonnegative; intermediate values stay below 34,000,000 for the accepted fixture parameters, so signed overflow is absent.'
    source_sha256 = $sourceHashes
    applied_changes = @()
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $fixture 'fixture.json') -Encoding utf8NoBOM
Write-Output $fixture

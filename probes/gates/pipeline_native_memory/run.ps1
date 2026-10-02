param([string]$Clang = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Clang) { $Clang = Join-Path $repo 'clang/bin/clang++.exe' }
if ($env:OS -ne 'Windows_NT') { throw 'This runner tests Windows OS memory counters.' }
$runDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$source = [IO.File]::ReadAllText((Join-Path $repo 'vyx_codegen/src/vyx_bootstrap_rt.cpp'))
function Get-ProductionExcerpt([string]$Start, [string]$End) {
    $first = $source.IndexOf($Start, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Production excerpt start missing: $Start" }
    $last = $source.IndexOf($End, $first + $Start.Length, [StringComparison]::Ordinal)
    if ($last -lt 0) { throw "Production excerpt end missing: $End" }
    return $source.Substring($first, $last - $first)
}
# Compile the actual implementations without linking unrelated LLVM/JIT code.
# No OS observation or process-map implementation is replaced by a test double.
$prefix = @'
#define NOMINMAX
#define VYX_RT_ABI
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#ifdef _WIN32
'@
$query = Get-ProductionExcerpt 'static bool vyx_rt_query_windows_process_memory(' 'static bool vyx_rt_query_process_memory('
$process = Get-ProductionExcerpt 'struct VyxBootstrapProcess {' 'static int64_t& vyx_bootstrap_next_process_id()'
$observe = Get-ProductionExcerpt '// Caller holds vyx_bootstrap_process_mutex()' 'extern "C" VYX_RT_ABI int32_t vyx_bootstrap_process_exit_code('
$close = Get-ProductionExcerpt 'extern "C" VYX_RT_ABI void vyx_bootstrap_process_close(' 'static std::string vyx_repl_trim('
$probe = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'probe.cpp'))
$testSource = Join-Path $runDir 'native_memory.cpp'
[IO.File]::WriteAllText($testSource, ($prefix + "`n" + $query + $process + $observe + $close + $probe))
$executable = Join-Path $runDir 'native_memory.exe'
& $Clang -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS $testSource -o $executable
if ($LASTEXITCODE -ne 0) { throw "Probe compile failed: $LASTEXITCODE" }
& $executable $runDir
if ($LASTEXITCODE -ne 0) { throw "Probe failed: $LASTEXITCODE" }
Write-Host "artifacts: $runDir"

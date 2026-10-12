param([string]$Compiler = '')

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$clang = Join-Path $repo 'clang/bin/clang++.exe'
$runtime = Join-Path $repo 'bootstrap_compiler/out'
$run = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force -Path $run | Out-Null

& $clang -c (Join-Path $PSScriptRoot 'native.cpp') -O2 -std=c++17 -fms-runtime-lib=static `
    -mno-incremental-linker-compatible -o (Join-Path $run 'native.obj')
if ($LASTEXITCODE -ne 0) { throw 'native allocation tracker did not compile' }

foreach ($level in @('O0', 'O2')) {
    $ir = Join-Path $run "$level.ll"
    $hooked = Join-Path $run "$level.tracked.ll"
    $exe = Join-Path $run "$level.exe"
    & $Compiler --src=file (Join-Path $PSScriptRoot 'main.vyx') --emit=ir "-$level" -o $ir
    if ($LASTEXITCODE -ne 0) { throw "$level IR emission failed" }
    # Intercept only allocation calls in the generated program, including
    # current runtime entry points and legacy libc names. Match complete LLVM
    # symbols so unrelated helpers keep their normal allocators. The runtime
    # and native producer are linked unchanged.
    $text = [IO.File]::ReadAllText($ir)
    $text = [regex]::Replace($text, '@vyx_class_alloc_abi(?=[(\s])', '@tracked_class_malloc')
    $text = [regex]::Replace($text, '@(vyx_runtime_malloc|malloc)(?=[(\s])', '@tracked_malloc')
    $text = [regex]::Replace($text, '@(vyx_runtime_calloc|calloc)(?=[(\s])', '@tracked_calloc')
    $text = [regex]::Replace($text, '@(vyx_runtime_free|free)(?=[(\s])', '@tracked_free')
    [IO.File]::WriteAllText($hooked, $text)
    & $clang $hooked (Join-Path $run 'native.obj') "-L$runtime" -lvyx_runtime `
        -ltbb12 -lsynchronization -lws2_32 -fms-runtime-lib=static -fuse-ld=lld -o $exe
    if ($LASTEXITCODE -ne 0) { throw "$level tracked program did not link" }
    $env:PATH = "$runtime;$env:PATH"
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$level DCI storage regression failed: $LASTEXITCODE" }
    Write-Output "${level}: allocations = destructors = releases"
}

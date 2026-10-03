param(
    [Parameter(Mandatory = $true)][string]$CacaoRoot,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateSet('windows-x64', 'android-arm64-v8a', 'android-x86_64')]
    [Parameter(Mandatory = $true)][string]$Platform
)

$ErrorActionPreference = 'Stop'
$sdkRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = (Resolve-Path -LiteralPath $CacaoRoot).Path
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$destinationRoot = Join-Path $sdkRoot 'vendor/Cacao'
$cachePath = Join-Path $buildRoot 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) { throw "Missing CMake cache: $cachePath" }
$cache = @{}
Get-Content -LiteralPath $cachePath | ForEach-Object {
    if ($_ -match '^([^#/][^:]*):[^=]+=(.*)$') { $cache[$matches[1]] = $matches[2] }
}
if ($cache['CMAKE_BUILD_TYPE'] -ne 'Release') { throw 'Only a Release Cacao build can be staged.' }
if ([IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY']) -ne $sourceRoot) {
    throw 'The build directory belongs to a different Cacao source tree.'
}
$copies = [System.Collections.Generic.List[object]]::new()
function Add-SdkFile([string]$Source, [string]$RelativeDestination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing SDK input: $Source" }
    $copies.Add(@{ source = $Source; destination = $RelativeDestination })
}
Add-SdkFile (Join-Path $sourceRoot 'include/CacaoC.h') 'include/CacaoC.h'
if ($Platform -eq 'windows-x64') {
    Add-SdkFile (Join-Path $buildRoot 'Cacao.dll') 'bin/x64/Cacao.dll'
    Add-SdkFile (Join-Path $buildRoot 'Cacao.lib') 'lib/x64/Cacao.lib'
    Add-SdkFile (Join-Path $buildRoot 'slang-compiler.dll') 'bin/x64/slang-compiler.dll'
    # Dawn is a load-time dependency whenever this Cacao build enables WebGPU.
    if ($cache['CACAO_BACKEND_WEBGPU'] -eq 'ON') {
        Add-SdkFile (Join-Path $buildRoot 'dawn.dll') 'bin/x64/dawn.dll'
    }
} else {
    $abi = $Platform.Substring('android-'.Length)
    if ($cache['ANDROID_ABI'] -ne $abi) { throw "CMake ABI does not match requested ABI: $abi" }
    Add-SdkFile (Join-Path $buildRoot 'libCacao.so') "lib/android/$abi/libCacao.so"
    Add-SdkFile (Join-Path $buildRoot 'Release/lib/libslang-compiler.so') "lib/android/$abi/libslang-compiler.so"
    # Do not copy a host library or invent a missing NDK runtime. These builds
    # use c++_static. A build using c++_shared must supply that runtime explicitly.
    if ($cache['ANDROID_STL'] -eq 'c++_shared') {
        $triple = if ($abi -eq 'arm64-v8a') { 'aarch64-linux-android' } else { 'x86_64-linux-android' }
        $ndk = $cache['CMAKE_ANDROID_NDK']
        if (-not $ndk) { throw 'A c++_shared build must identify CMAKE_ANDROID_NDK.' }
        Add-SdkFile (Join-Path $ndk "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/$triple/libc++_shared.so") "lib/android/$abi/libc++_shared.so"
    }
}
# Validate all inputs before replacing any SDK file. No directory is deleted.
$records = foreach ($copy in $copies) {
    $destination = Join-Path $destinationRoot $copy.destination
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $copy.source -Destination $destination -Force
    $sourceHash = (Get-FileHash -LiteralPath $copy.source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) { throw "SDK copy verification failed: $destination" }
    [ordered]@{ path = $copy.destination; bytes = (Get-Item -LiteralPath $destination).Length; sha256 = $destinationHash }
}
$revision = (& git -C $sourceRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not read Cacao source revision.' }
$manifest = [ordered]@{
    platform = $Platform
    source_revision = $revision
    source_worktree_changes = @(& git -C $sourceRoot status --short)
    configuration = 'Release'
    android_platform = $cache['ANDROID_PLATFORM']
    backends = @('VULKAN', 'D3D12', 'D3D11', 'WEBGPU', 'OPENGL', 'OPENGLES') | Where-Object { $cache["CACAO_BACKEND_$_"] -eq 'ON' }
    files = @($records)
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destinationRoot "sdk-$Platform.json") -Encoding utf8
Write-Output "Cacao SDK staged: $Platform ($($records.Count) files, SHA256 verified)"

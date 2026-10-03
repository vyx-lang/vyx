param(
    [ValidateSet('arm64-v8a','x86_64')][string]$Abi = 'arm64-v8a',
    [ValidateRange(29, 35)][int]$MinSdk = 29,
    [string]$NdkRoot = 'E:/Android/sdk/ndk/30.0.15729638',
    [string]$CMake = 'D:/Jetbrains/CLion/bin/cmake/win/x64/bin/cmake.exe',
    [string]$Ninja = 'D:/LLVM/bin/ninja.exe',
    [string]$Adb = 'E:/Android/sdk/platform-tools/adb.exe',
    [string]$Device = ''
)
$ErrorActionPreference = 'Stop'
$zyn = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$build = Join-Path $PSScriptRoot ".work/verify/$Abi"
$stage = Join-Path $build 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$expectedLibraries = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($dependency in @('SDL3','FreeType','HarfBuzz','ICU')) {
    $vendor = Join-Path $zyn "vendor/$dependency/lib/android/$Abi"
    if (-not (Test-Path -LiteralPath (Join-Path $vendor 'manifest.json'))) { throw "Missing completed dependency manifest: $vendor" }
    $identity = Get-Content -LiteralPath (Join-Path $vendor 'manifest.json') -Raw | ConvertFrom-Json
    $dependencyMinSdk = if ($dependency -eq 'ICU') { $identity.min_android_api } else { $identity.min_sdk }
    if ($identity.abi -ne $Abi -or $dependencyMinSdk -ne $MinSdk) { throw "Dependency ABI/API mismatch: $vendor" }
    foreach ($lib in Get-ChildItem -LiteralPath $vendor -Filter '*.so' -File) {
        [void]$expectedLibraries.Add($lib.Name)
        $dest = Join-Path $stage $lib.Name
        if ((Test-Path -LiteralPath $dest) -and $lib.Name -eq 'libc++_shared.so' -and ((Get-FileHash $dest).Hash -ne (Get-FileHash $lib.FullName).Hash)) { throw 'Dependencies disagree on libc++_shared.so' }
        Copy-Item -LiteralPath $lib.FullName -Destination $dest -Force
    }
}
foreach ($stale in Get-ChildItem -LiteralPath $stage -Filter '*.so' -File) {
    if ($stale.Name -ne 'libzyn_text_bridge_check.so' -and !$expectedLibraries.Contains($stale.Name)) {
        Remove-Item -LiteralPath $stale.FullName
    }
}
& $CMake -S (Join-Path $PSScriptRoot 'verify') -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DCMAKE_TOOLCHAIN_FILE=$NdkRoot/build/cmake/android.toolchain.cmake" "-DANDROID_ABI=$Abi" "-DANDROID_PLATFORM=android-$MinSdk" -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release "-DZYN_ROOT=$zyn" *> (Join-Path $build 'configure.log')
if ($LASTEXITCODE -ne 0) { throw "Verification configure failed: $build/configure.log" }
& $CMake --build $build --parallel 2 *> (Join-Path $build 'build.log')
if ($LASTEXITCODE -ne 0) { throw "Verification link failed: $build/build.log" }
Copy-Item -LiteralPath (Join-Path $build 'zyn_android_deps_verify'), (Join-Path $build 'libzyn_text_bridge_check.so') -Destination $stage -Force
$report = @{ abi=$Abi; min_sdk=$MinSdk; bridge_link='passed --no-undefined'; runtime='not run'; artifacts=@(Get-ChildItem $stage -File | ForEach-Object { @{name=$_.Name;sha256=(Get-FileHash $_.FullName).Hash;bytes=$_.Length} }) }
if ($Device) {
    $supported = & $Adb -s $Device shell getprop ro.product.cpu.abilist
    if ($LASTEXITCODE -ne 0 -or $Abi -notin (($supported -join '').Trim().Split(','))) { throw "Device does not report $Abi support" }
    $deviceApi = & $Adb -s $Device shell getprop ro.build.version.sdk
    if ($LASTEXITCODE -ne 0 -or [int](($deviceApi -join '').Trim()) -lt $MinSdk) { throw 'Device API is lower than the dependency build minimum' }
    $remote = "/data/local/tmp/zyn-deps-$Abi"
    & $Adb -s $Device shell mkdir -p $remote
    if ($LASTEXITCODE -ne 0) { throw 'adb mkdir failed' }
    foreach ($file in Get-ChildItem $stage -File) {
        & $Adb -s $Device push $file.FullName "$remote/$($file.Name)" *> (Join-Path $build "push-$($file.Name).log")
        if ($LASTEXITCODE -ne 0) { throw "adb push failed: $($file.Name)" }
    }
    & $Adb -s $Device shell chmod 755 "$remote/zyn_android_deps_verify"
    if ($LASTEXITCODE -ne 0) { throw 'adb chmod failed' }
    & $Adb -s $Device shell "LD_LIBRARY_PATH=$remote $remote/zyn_android_deps_verify" *> (Join-Path $build 'runtime.log')
    $report.runtime_exit_code = $LASTEXITCODE
    $report.runtime = if ($LASTEXITCODE -eq 0) { 'passed' } else { 'failed' }
    $report.device = $Device
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $build 'result.json') -Encoding utf8
if ($report.runtime -eq 'failed') { throw "Runtime verification failed; see $build/runtime.log" }
Write-Host "Android dependency verification ${Abi}: bridge link passed; runtime $($report.runtime). $build/result.json"

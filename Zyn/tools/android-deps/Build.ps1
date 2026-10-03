param(
    [string]$NdkRoot = 'E:/Android/sdk/ndk/30.0.15729638',
    [string]$CMake = 'D:/Jetbrains/CLion/bin/cmake/win/x64/bin/cmake.exe',
    [string]$Ninja = 'D:/LLVM/bin/ninja.exe',
    [ValidateSet('arm64-v8a', 'x86_64')][string[]]$Abi = @('arm64-v8a', 'x86_64'),
    [ValidateSet('SDL3', 'FreeType', 'HarfBuzz')][string[]]$Libraries = @('SDL3', 'FreeType', 'HarfBuzz'),
    [ValidateRange(29, 35)][int]$MinSdk = 29,
    [ValidateRange(1, 20)][int]$Jobs = 2
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or $PSVersionTable.PSVersion.Major -lt 7) { throw 'Requires Windows and PowerShell 7.' }
$NdkRoot = (Resolve-Path -LiteralPath $NdkRoot).Path
$CMake = (Resolve-Path -LiteralPath $CMake).Path
$Ninja = (Resolve-Path -LiteralPath $Ninja).Path
$zyn = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$work = Join-Path $PSScriptRoot '.work'
$locks = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'sources.json') -Raw | ConvertFrom-Json -AsHashtable
$run = Join-Path $work ('logs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force -Path $run, (Join-Path $work 'downloads'), (Join-Path $work 'sources') | Out-Null
$toolchain = Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake'
$ndkBin = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$readelf = Join-Path $ndkBin 'llvm-readelf.exe'
$script:commandIndex = 0

function Run-Tool([string]$File, [string[]]$Arguments, [string]$Name) {
    $script:commandIndex++
    $stem = Join-Path $run ('{0:D3}-{1}' -f $script:commandIndex, $Name)
    @{ executable = $File; argv = $Arguments; working_directory = $zyn } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath ($stem + '.command.json') -Encoding utf8
    $psi = [Diagnostics.ProcessStartInfo]::new($File)
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $zyn
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($arg in $Arguments) { $psi.ArgumentList.Add($arg) }
    $p = [Diagnostics.Process]::new()
    $p.StartInfo = $psi
    $clock = [Diagnostics.Stopwatch]::StartNew()
    if (-not $p.Start()) { throw "Could not start $File" }
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath ($stem + '.stdout.log') -Encoding utf8
    $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath ($stem + '.stderr.log') -Encoding utf8
    @{ exit_code = $p.ExitCode; wall_seconds = $clock.Elapsed.TotalSeconds } | ConvertTo-Json | Set-Content -LiteralPath ($stem + '.result.json')
    Write-Host "$Name exit=$($p.ExitCode) seconds=$([math]::Round($clock.Elapsed.TotalSeconds,2))"
    if ($p.ExitCode -ne 0) { throw "Failed $Name; see $stem.stderr.log and stdout.log" }
}

foreach ($name in @('SDL3', 'FreeType', 'HarfBuzz')) {
    if ($name -notin $Libraries) { continue }
    $source = $locks[$name]
    $archive = Join-Path $work ('downloads/' + $source.archive)
    if (-not (Test-Path -LiteralPath $archive)) {
        Run-Tool (Get-Command curl.exe).Source @('-fL', '--retry', '3', '--connect-timeout', '20', '-o', $archive, $source.url) "download-$name"
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $source.sha256) { throw "Source hash mismatch: $archive" }
    $src = Join-Path $work ('sources/' + $source.directory)
    if (-not (Test-Path -LiteralPath $src)) {
        Run-Tool (Get-Command tar.exe).Source @('-xf', $archive, '-C', (Join-Path $work 'sources')) "extract-$name"
    }
    foreach ($targetAbi in $Abi) {
        $build = Join-Path $work "build/$name/$targetAbi-api$MinSdk"
        $install = Join-Path $work "install/$name/$targetAbi-api$MinSdk"
        $vendor = Join-Path $zyn "vendor/$name/lib/android/$targetAbi"
        $argsCmake = @('-S', $src, '-B', $build, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DANDROID_ABI=$targetAbi", "-DANDROID_PLATFORM=android-$MinSdk", '-DANDROID_STL=c++_shared', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF', "-DCMAKE_INSTALL_PREFIX=$install", '-DBUILD_SHARED_LIBS=ON')
        switch ($name) {
            SDL3 { $argsCmake += @('-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF', '-DSDL_ANDROID_JAR=OFF') }
            FreeType { $argsCmake += @('-DFT_REQUIRE_ZLIB=ON', '-DFT_DISABLE_BZIP2=ON', '-DFT_DISABLE_PNG=ON', '-DFT_DISABLE_HARFBUZZ=ON', '-DFT_DISABLE_BROTLI=ON') }
            HarfBuzz {
                $ft = Join-Path $work "install/FreeType/$targetAbi-api$MinSdk"
                $ftLib = Join-Path $ft 'lib/libfreetype.so'
                if (-not (Test-Path -LiteralPath $ftLib)) { throw "Build FreeType first: $ftLib" }
                $argsCmake += @('-DHB_HAVE_FREETYPE=ON', "-DFREETYPE_LIBRARY=$ftLib", "-DFREETYPE_INCLUDE_DIR_freetype2=$ft/include/freetype2", "-DFREETYPE_INCLUDE_DIR_ft2build=$ft/include/freetype2", '-DHB_HAVE_ICU=OFF', '-DHB_BUILD_UTILS=OFF', '-DHB_BUILD_SUBSET=OFF', '-DHB_BUILD_RASTER=OFF', '-DHB_BUILD_VECTOR=OFF', '-DHB_BUILD_GPU=OFF')
            }
        }
        Run-Tool $CMake $argsCmake "configure-$name-$targetAbi"
        Run-Tool $CMake @('--build', $build, '--parallel', "$Jobs") "build-$name-$targetAbi"
        Run-Tool $CMake @('--install', $build) "install-$name-$targetAbi"
        New-Item -ItemType Directory -Force -Path $vendor, (Join-Path $vendor 'licenses') | Out-Null
        $outputs = @(Get-ChildItem -LiteralPath (Join-Path $install 'lib') -Filter '*.so' -File)
        if ($outputs.Count -eq 0) { throw "No Android shared library installed for $name/$targetAbi" }
        foreach ($lib in $outputs) {
            Copy-Item -LiteralPath $lib.FullName -Destination (Join-Path $vendor $lib.Name) -Force
            Run-Tool $readelf @('--file-header', '--dynamic', '--notes', (Join-Path $vendor $lib.Name)) "elf-$name-$targetAbi-$($lib.BaseName)"
        }
        foreach ($license in $source.licenses) {
            $licenseDestination = Join-Path $vendor ('licenses/' + $license)
            New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($licenseDestination)) | Out-Null
            Copy-Item -LiteralPath (Join-Path $src $license) -Destination $licenseDestination -Force
        }
        $triple = if ($targetAbi -eq 'arm64-v8a') { 'aarch64-linux-android' } else { 'x86_64-linux-android' }
        $cppRuntime = Join-Path $NdkRoot "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/$triple/libc++_shared.so"
        Copy-Item -LiteralPath $cppRuntime -Destination (Join-Path $vendor 'libc++_shared.so') -Force
        Copy-Item -LiteralPath (Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/sysroot/NOTICE') -Destination (Join-Path $vendor 'licenses/NDK-sysroot-NOTICE') -Force
        $manifest = [ordered]@{
            schema_version = 1; dependency = $name; version = $source.version; source_url = $source.url; archive_sha256 = $source.sha256
            abi = $targetAbi; min_sdk = $MinSdk; configuration = 'Release'; ndk_properties = (Get-Content -LiteralPath (Join-Path $NdkRoot 'source.properties') -Raw)
            build_argv = $argsCmake; logs = $run; generated_utc = [DateTime]::UtcNow.ToString('o')
            tools = @($CMake, $Ninja, (Join-Path $ndkBin 'clang++.exe')) | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash } }
            artifacts = @(Get-ChildItem -LiteralPath $vendor -Filter '*.so' -File | ForEach-Object { @{ name = $_.Name; bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } })
        }
        $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $vendor 'manifest.json') -Encoding utf8
    }
    if ($name -eq 'SDL3') {
        $java = Join-Path $zyn 'vendor/SDL3/android/java/org/libsdl/app'
        New-Item -ItemType Directory -Force -Path $java | Out-Null
        Copy-Item -Path (Join-Path $src 'android-project/app/src/main/java/org/libsdl/app/*.java') -Destination $java -Force
        Copy-Item -LiteralPath (Join-Path $src 'LICENSE.txt') -Destination (Join-Path $zyn 'vendor/SDL3/android/LICENSE.txt') -Force
    }
}
Write-Host "Android dependency builds complete. Logs: $run"

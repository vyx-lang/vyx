param(
    [string]$Ndk = 'E:/Android/sdk/ndk/30.0.15729638',
    [string]$CMake = 'cmake',
    [string]$Ninja = 'ninja',
    [ValidateSet('arm64-v8a', 'x86_64')][string[]]$Abi = @('arm64-v8a', 'x86_64'),
    [ValidateRange(29, 36)][int]$MinApi = 29,
    [ValidateRange(1, 64)][int]$Jobs = 2,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or $PSVersionTable.PSVersion.Major -lt 7) { throw 'Use Windows PowerShell 7.' }
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../.work/icu'))
$ndkPath = (Resolve-Path -LiteralPath $Ndk).Path
$cmakePath = (Get-Command $CMake -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$ninjaPath = (Get-Command $Ninja -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$toolchain = Join-Path $ndkPath 'build/cmake/android.toolchain.cmake'
$llvmBin = Join-Path $ndkPath 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$readelf = Join-Path $llvmBin 'llvm-readelf.exe'
$objcopy = Join-Path $llvmBin 'llvm-objcopy.exe'
foreach ($path in @($toolchain, $readelf, $objcopy)) { if (-not (Test-Path -LiteralPath $path)) { throw "Missing NDK tool: $path" } }
New-Item -ItemType Directory -Path $work -Force | Out-Null

function Invoke-Logged([string]$File, [string[]]$Arguments, [string]$Directory, [string]$Log) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $File
    $info.WorkingDirectory = $Directory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    $stdout = [IO.FileStream]::new("$Log.stdout.log", [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $stderr = [IO.FileStream]::new("$Log.stderr.log", [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $outTask = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
        $errTask = $process.StandardError.BaseStream.CopyToAsync($stderr)
        $process.WaitForExit()
        $null = $outTask.GetAwaiter().GetResult()
        $null = $errTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Command failed with exit $($process.ExitCode): $File. Logs: $Log.*.log" }
    } finally { $stdout.Dispose(); $stderr.Dispose(); $process.Dispose() }
}

$downloads = @(
    @{ name='icu4c-78.3-sources.zip'; sha256='20b295e2c23c541aec17f35c546e4d3136b7ab7d58e3a5fc4e479958a69b1a2c'; destination='source'; marker='source/icu/source/common/sources.txt' },
    @{ name='icu4c-78.3-data-bin-l.zip'; sha256='982619632b78887f1895b063e96e8c3cc7f99283337c8abbd05aa71635de613c'; destination='data'; marker='data/icudt78l.dat' }
)
foreach ($download in $downloads) {
    $archive = Join-Path $work $download.name
    $url = 'https://github.com/unicode-org/icu/releases/download/release-78.3/' + $download.name
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host "Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $download.sha256) { throw "Release archive hash mismatch: $archive" }
    if (-not (Test-Path -LiteralPath (Join-Path $work $download.marker))) {
        Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $work $download.destination)
    }
}
$source = Join-Path $work 'source/icu/source'
$data = Join-Path $work 'data/icudt78l.dat'
$dataHash = (Get-FileHash -LiteralPath $data -Algorithm SHA256).Hash
if ($dataHash -ne 'd5cf2a40dccbe471781ec7af85693bff542ff12f0b670c9630c4e72d60714b8b') { throw 'Extracted ICU data hash mismatch.' }
$dataBytes = [IO.File]::ReadAllBytes($data)
$headerSize = [BitConverter]::ToUInt16($dataBytes, 0)
$dataItems = [BitConverter]::ToUInt32($dataBytes, $headerSize)
if ($dataBytes[2] -ne 0xda -or $dataBytes[3] -ne 0x27 -or $dataBytes[8] -ne 0 -or $dataBytes[9] -ne 0 -or $dataBytes[10] -ne 2 -or $dataItems -ne 4305) {
    throw 'ICU package header is not the expected full little-endian ASCII/UChar2 data.'
}
$dataSize = $dataBytes.Length
$dataBytes = $null
$sourcesIdentity = @($downloads | ForEach-Object { [ordered]@{url=('https://github.com/unicode-org/icu/releases/download/release-78.3/'+$_.name); sha256=$_.sha256} })

foreach ($targetAbi in $Abi) {
    $build = Join-Path $work "build/$targetAbi-api$MinApi"
    $stage = Join-Path $repository "Zyn/vendor/ICU/lib/android/$targetAbi"
    New-Item -ItemType Directory -Path $build -Force | Out-Null
    $configureArgs = @('-S', $PSScriptRoot, '-B', $build, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninjaPath", "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DANDROID_ABI=$targetAbi",
        "-DANDROID_PLATFORM=android-$MinApi", '-DANDROID_STL=c++_shared', '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG',
        "-DICU_SOURCE_DIR=$source", "-DICU_DATA_FILE=$data", '-DICU_BUILD_SMOKE=ON')
    Write-Host "ICU 78.3 configure: $targetAbi, API $MinApi"
    Invoke-Logged $cmakePath $configureArgs $repository (Join-Path $build 'configure')
    if ($ConfigureOnly) { continue }
    Write-Host "ICU 78.3 build: $targetAbi, jobs=$Jobs. Log: $build/build.stdout.log"
    $started = [DateTime]::UtcNow
    Invoke-Logged $cmakePath @('--build', $build, '--parallel', "$Jobs") $repository (Join-Path $build 'build')
    $seconds = ([DateTime]::UtcNow - $started).TotalSeconds
    $outputs = @()
    foreach ($name in @('libzynicudata.so', 'libzynicuuc.so', 'libzynicui18n.so')) {
        $library = Join-Path $build "lib/$name"
        Invoke-Logged $readelf @('--file-header', '--dynamic', '--dyn-syms', '--wide', $library) $repository (Join-Path $build "$name.elf")
        $elf = Get-Content -LiteralPath (Join-Path $build "$name.elf.stdout.log") -Raw
        $machine = if ($targetAbi -eq 'arm64-v8a') { 'AArch64' } else { 'Advanced Micro Devices X86-64' }
        if (-not $elf.Contains($machine) -or -not ($elf -match 'Class:\s+ELF64')) { throw "Wrong ELF architecture: $library" }
        $soname = [regex]::Match($elf, 'SONAME[^\r\n]*\[([^\]]+)\]').Groups[1].Value
        if ($soname -ne $name) { throw "Unexpected SONAME '$soname' for $library" }
        $needed = @([regex]::Matches($elf, 'NEEDED[^\r\n]*\[([^\]]+)\]') | ForEach-Object { $_.Groups[1].Value })
        if (@($needed | Where-Object { $_ -match '^lib(zyn)?icu.*\.so\.' -or $_ -in @('libicudata.so','libicuuc.so','libicui18n.so') }).Count -ne 0) {
            throw "Unexpected ICU DT_NEEDED in $library"
        }
        if ($elf -match '\bTEXTREL\b') { throw "Text relocation in $library" }
        if ($name -eq 'libzynicudata.so' -and -not ($elf -match "\b$dataSize\s+OBJECT\s+GLOBAL\s+DEFAULT\s+\d+\s+icudt78_dat\b")) {
            throw 'The exported ICU data object is missing or has the wrong size.'
        }
        if ($name -eq 'libzynicuuc.so' -and (-not ($elf -match '\bu_init_78\b') -or 'libzynicudata.so' -notin $needed)) {
            throw 'ICU common must export versioned u_init_78 and depend on the real data library.'
        }
        if ($name -eq 'libzynicui18n.so' -and (-not ($elf -match '\bucol_open_78\b') -or 'libzynicuuc.so' -notin $needed)) {
            throw 'ICU i18n must export ucol_open_78 and depend on ICU common.'
        }
        $outputs += [ordered]@{ name=$name; bytes=(Get-Item -LiteralPath $library).Length; sha256=(Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash; soname=$soname; needed=$needed; machine=$machine }
    }
    $embeddedData = Join-Path $build 'embedded-icudata.bin'
    Invoke-Logged $objcopy @('--dump-section', ".rodata=$embeddedData", (Join-Path $build 'lib/libzynicudata.so')) $repository (Join-Path $build 'extract-data')
    if ((Get-FileHash -LiteralPath $embeddedData -Algorithm SHA256).Hash -ne $dataHash) {
        throw 'The linked ICU .rodata is not identical to the official full .dat package.'
    }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    foreach ($old in @('libicudata.so', 'libicuuc.so', 'libicui18n.so')) {
        $oldPath = Join-Path $stage $old
        if (Test-Path -LiteralPath $oldPath) { Remove-Item -LiteralPath $oldPath }
    }
    foreach ($output in $outputs) { Copy-Item -LiteralPath (Join-Path $build "lib/$($output.name)") -Destination (Join-Path $stage $output.name) }
    Copy-Item -LiteralPath (Join-Path $work 'source/icu/LICENSE') -Destination (Join-Path $stage 'LICENSE')
    [ordered]@{
        schema_version=1; package='ICU'; version='78.3'; abi=$targetAbi; min_android_api=$MinApi;
        built_utc=[DateTime]::UtcNow.ToString('o'); build_seconds=$seconds; jobs=$Jobs;
        ndk=$ndkPath; ndk_properties=(Get-Content -LiteralPath (Join-Path $ndkPath 'source.properties') -Raw).Trim();
        compiler_sha256=(Get-FileHash -LiteralPath (Join-Path $llvmBin 'clang++.exe') -Algorithm SHA256).Hash;
        configure_argv=$configureArgs; source_archives=$sourcesIdentity;
        data=[ordered]@{name='icudt78l.dat'; bytes=$dataSize; entries=$dataItems; sha256=$dataHash; embedded_bytes_identical=$true};
        common_source_count=@(Get-Content -LiteralPath (Join-Path $source 'common/sources.txt')).Count;
        i18n_source_count=@(Get-Content -LiteralPath (Join-Path $source 'i18n/sources.txt')).Count;
        common_sources_sha256=(Get-FileHash -LiteralPath (Join-Path $source 'common/sources.txt') -Algorithm SHA256).Hash;
        i18n_sources_sha256=(Get-FileHash -LiteralPath (Join-Path $source 'i18n/sources.txt') -Algorithm SHA256).Hash;
        license='Unicode-3.0 plus bundled third-party notices; see LICENSE';
        license_sha256=(Get-FileHash -LiteralPath (Join-Path $stage 'LICENSE') -Algorithm SHA256).Hash;
        libraries=$outputs;
        runtime_dependency='Package the matching NDK libc++_shared.so once per ABI along with these three ICU libraries.';
        device_smoke=[ordered]@{path=(Join-Path $build 'icu_data_smoke'); executed=$false}
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding utf8NoBOM
    Write-Host ("ICU {0}: staged three verified libraries in {1} ({2:N1}s)" -f $targetAbi, $stage, $seconds)
}

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $project '..\..\..')).Path
$llvm = if ($env:LLVM_ROOT) { $env:LLVM_ROOT } else { Join-Path $repo 'clang' }
$clang = Join-Path $llvm 'bin\clang.exe'
$llvmLib = Join-Path $llvm 'bin\llvm-lib.exe'
$boot = Join-Path $repo 'bootstrap_compiler\out\boot.exe'

$oldCurl = $env:VYX_CURL_LIB
$oldOpenSsl = $env:OPENSSL_ROOT
$oldVcpkg = $env:VCPKG_ROOT
$oldDebug = $env:VYX_DEBUG_LINK
$oldPath = $env:Path
$oldPkgDir = $env:VYX_TEST_PKGCONFIG_LIBDIR
Push-Location $project
try {
    $envRoot = Join-Path $project 'fixture\env'
    $curlDir = Join-Path $envRoot 'curl'
    $openSslDir = Join-Path $envRoot 'openssl\lib'
    $vcpkgRoot = Join-Path $project 'fixture\vcpkg'
    $vcpkgDir = Join-Path $vcpkgRoot 'installed\x64-windows\lib'
    $pkgDir = Join-Path $project 'fixture\pkgconfig\lib'
    $pkgBin = Join-Path $project 'fixture\pkgconfig\bin'
    New-Item -ItemType Directory -Force '.cache', $curlDir, $openSslDir, $vcpkgDir, $pkgDir, $pkgBin | Out-Null

    foreach ($name in @('curl', 'ssl', 'crypto')) {
        $obj = Join-Path $project ".cache\$name.obj"
        & $clang --target=x86_64-pc-windows-msvc -c "native/$name.c" -o $obj
        if ($LASTEXITCODE -ne 0) { throw "clang failed for $name" }
        $file = if ($name -eq 'curl') { Join-Path $curlDir 'libcurl.lib' }
                else { Join-Path $openSslDir "lib$name.lib" }
        & $llvmLib "/OUT:$file" $obj
        if ($LASTEXITCODE -ne 0) { throw "llvm-lib failed for $name" }
        Copy-Item -LiteralPath $file -Destination (Join-Path $vcpkgDir "lib$name.lib") -Force
        Copy-Item -LiteralPath $file -Destination (Join-Path $pkgDir "lib$name.lib") -Force
    }
    & $clang --target=x86_64-pc-windows-msvc 'native/pkgconfig.c' -o (Join-Path $pkgBin 'pkg-config.exe')
    if ($LASTEXITCODE -ne 0) { throw 'clang failed for fake pkg-config' }

    $env:VYX_DEBUG_LINK = '1'
    foreach ($mode in @('env', 'vcpkg', 'pkgconfig')) {
        $env:Path = $oldPath
        $env:VYX_TEST_PKGCONFIG_LIBDIR = $null
        if ($mode -eq 'env') {
            $env:VYX_CURL_LIB = Join-Path $curlDir 'libcurl.lib'
            $env:OPENSSL_ROOT = Join-Path $envRoot 'openssl'
            $env:VCPKG_ROOT = $null
        } elseif ($mode -eq 'vcpkg') {
            $env:VYX_CURL_LIB = $null
            $env:OPENSSL_ROOT = $null
            $env:VCPKG_ROOT = $vcpkgRoot
        } else {
            $env:VYX_CURL_LIB = $null
            $env:OPENSSL_ROOT = $null
            $env:VCPKG_ROOT = $null
            $env:VYX_TEST_PKGCONFIG_LIBDIR = $pkgDir
            $env:Path = "$pkgBin;$oldPath"
        }
        foreach ($output in @('.cache/crate_system_lib_discovery.obj',
                              'target/system_lib_discovery.exe')) {
            if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output }
        }
        $buildOutput = & $boot build --target system_lib_discovery -j2 2>&1
        if ($LASTEXITCODE -ne 0) {
            $buildOutput | Select-Object -Last 50 | Write-Host
            throw "$mode discovery build failed"
        }
        $linkLine = @($buildOutput | Where-Object { $_ -match '\[boot-cmd\].*clang\+\+.*libcurl' })
        if ($linkLine.Count -eq 0 -or
            @($linkLine | Where-Object { $_ -match 'libssl' -and $_ -match 'libcrypto' }).Count -eq 0) {
            $buildOutput | Select-Object -Last 50 | Write-Host
            throw "$mode discovery did not resolve all three libraries"
        }
        $runOutput = & (Join-Path $project 'target\system_lib_discovery.exe')
        if ($LASTEXITCODE -ne 0 -or (($runOutput -join "`n").Trim() -ne 'system lib discovery OK')) {
            throw "$mode executable failed: $runOutput"
        }
    }
    Write-Host 'system_lib_discovery: OK (env, vcpkg, pkg-config)'
} finally {
    $env:VYX_CURL_LIB = $oldCurl
    $env:OPENSSL_ROOT = $oldOpenSsl
    $env:VCPKG_ROOT = $oldVcpkg
    $env:VYX_DEBUG_LINK = $oldDebug
    $env:Path = $oldPath
    $env:VYX_TEST_PKGCONFIG_LIBDIR = $oldPkgDir
    Pop-Location
}

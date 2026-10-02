$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $project '..\..\..')).Path
$llvm = if ($env:LLVM_ROOT) { $env:LLVM_ROOT } else { Join-Path $repo 'clang' }
$clang = Join-Path $llvm 'bin\clang.exe'
$llvmLib = Join-Path $llvm 'bin\llvm-lib.exe'
$boot = Join-Path $repo 'bootstrap_compiler\out\boot.exe'

Push-Location $project
try {
    New-Item -ItemType Directory -Force '.cache', 'lib' | Out-Null
    foreach ($name in @('curl', 'ssl', 'crypto')) {
        & $clang --target=x86_64-pc-windows-msvc -c ("native/$name.c") -o (".cache/$name.obj")
        if ($LASTEXITCODE -ne 0) { throw "clang failed for $name" }
        & $llvmLib ("/OUT:lib/lib$name.lib") (".cache/$name.obj")
        if ($LASTEXITCODE -ne 0) { throw "llvm-lib failed for $name" }
    }

    # Exercise compiler argument parsing and native linking on every run.
    foreach ($output in @('.cache/crate_manifest_windows_lib_names.obj',
                          'target/manifest_windows_lib_names.exe')) {
        if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output }
    }

    $oldDebugLink = $env:VYX_DEBUG_LINK
    try {
        $env:VYX_DEBUG_LINK = '1'
        $buildOutput = & $boot build --target manifest_windows_lib_names -j2 2>&1
        $buildExit = $LASTEXITCODE
    } finally {
        $env:VYX_DEBUG_LINK = $oldDebugLink
    }
    if ($buildExit -ne 0) {
        $buildOutput | Select-Object -Last 60 | Write-Host
        throw "boot build failed ($buildExit)"
    }
    $linkLine = @($buildOutput | Where-Object { $_ -match '\[boot-cmd\].*clang\+\+.*-l libcurl' })
    if ($linkLine.Count -eq 0 -or
        @($linkLine | Where-Object { $_ -match '-l libssl\b' -and $_ -match '-l libcrypto\b' }).Count -eq 0) {
        $buildOutput | Select-Object -Last 60 | Write-Host
        throw 'link command did not preserve the three Windows library names'
    }
    $runOutput = & (Join-Path $project 'target\manifest_windows_lib_names.exe')
    if ($LASTEXITCODE -ne 0 -or (($runOutput -join "`n").Trim() -ne 'windows library names OK')) {
        throw "linked executable failed: $runOutput"
    }
    Write-Host 'windows_lib_names: OK'
} finally {
    Pop-Location
}

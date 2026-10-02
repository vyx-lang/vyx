param(
    [string]$Compiler = (Join-Path $PSScriptRoot '../../../bootstrap_compiler/out/boot.exe'),
    [int]$Threads = 2
)

$ErrorActionPreference = 'Stop'
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$runName = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$runDir = Join-Path $PSScriptRoot ('.runs/' + $runName)

# Every invocation starts from source-only copies so no stale DLL, interface,
# or compiler-cache entry can make a compiler comparison pass accidentally.
foreach ($project in @('dep', 'factory', 'app')) {
    $source = Join-Path $PSScriptRoot $project
    $destination = Join-Path $runDir $project
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source 'Vyx.toml') -Destination $destination
    Copy-Item -LiteralPath (Join-Path $source 'src') -Destination $destination -Recurse
}

$oldSplit = $env:VYX_BUILD_SPLIT_MODULE_UNITS
$oldSingle = $env:VYX_BUILD_SINGLE_FILE_UNITS
try {
    # Exercise root pruning even for this small fixture. File-sized units do
    # not reliably expose the original missing-interface-method failure.
    $env:VYX_BUILD_SPLIT_MODULE_UNITS = '0'
    $env:VYX_BUILD_SINGLE_FILE_UNITS = '0'
    Push-Location (Join-Path $runDir 'app')
    try {
        & $compilerPath build --target BoxAbiConsumer "-j$Threads" 2>&1 |
            Tee-Object -FilePath (Join-Path $runDir 'build.log')
        if ($LASTEXITCODE -ne 0) { throw "Fixture build failed: $LASTEXITCODE" }

        $chunks = @(Get-ChildItem -LiteralPath (Join-Path $runDir 'dep/.cache') -Filter '*rootchunk*.obj')
        if ($chunks.Count -lt 2) {
            throw "Fixture requires at least two root codegen units; found $($chunks.Count)"
        }

        & './target/BoxAbiConsumer.exe' 2>&1 |
            Tee-Object -FilePath (Join-Path $runDir 'run.log')
        $probeExit = $LASTEXITCODE
        "compiler=$compilerPath`nroot_chunks=$($chunks.Count)`nexit=$probeExit" |
            Set-Content -LiteralPath (Join-Path $runDir 'result.txt')
        if ($probeExit -ne 0) { throw "Cross-DLL Box probe failed: $probeExit; artifacts: $runDir" }

        $dceOutput = Join-Path $runDir 'unused_default.exe'
        & $compilerPath --src=file (Join-Path $PSScriptRoot 'unused_default.vyx') --emit=exe --emit-root-names main -o $dceOutput 2>&1 |
            Tee-Object -FilePath (Join-Path $runDir 'unused_default.build.log')
        if ($LASTEXITCODE -ne 0) { throw "Unused default-method body became reachable: $LASTEXITCODE" }
        & $dceOutput
        if ($LASTEXITCODE -ne 0) { throw "Unused default-method probe failed: $LASTEXITCODE" }

        Write-Output "PASS: cross-DLL Box dispatch, clone and lifetime ($($chunks.Count) root chunks)"
        Write-Output "Artifacts: $runDir"
    } finally {
        Pop-Location
    }
} finally {
    $env:VYX_BUILD_SPLIT_MODULE_UNITS = $oldSplit
    $env:VYX_BUILD_SINGLE_FILE_UNITS = $oldSingle
}

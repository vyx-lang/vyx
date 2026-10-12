param([string]$BootstrapCompiler = "", [string]$RuntimeDir = "")
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    throw "Pass -BootstrapCompiler with the current source build."
}
$gateArgs = @((Join-Path $repoRoot "probes/gates/network-protocols/run.py"),
              "--compiler", (Resolve-Path $BootstrapCompiler).Path)
if ($env:VYX_NETWORK_NATIVE_BIN) {
    foreach ($directory in $env:VYX_NETWORK_NATIVE_BIN.Split([IO.Path]::PathSeparator)) {
        if ($directory) { $gateArgs += @("--native-bin", $directory) }
    }
}
if ($env:VYX_NETWORK_PYTHON_PACKAGES) {
    $gateArgs += @("--python-packages", $env:VYX_NETWORK_PYTHON_PACKAGES)
}
& python @gateArgs
exit $LASTEXITCODE

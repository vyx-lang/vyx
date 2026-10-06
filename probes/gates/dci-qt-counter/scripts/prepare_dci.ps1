param([int]$Jobs = 4, [switch]$Force)
$ErrorActionPreference = "Stop"
$Project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Root = [IO.Path]::GetFullPath((Join-Path $Project "..\..\.."))
$Python = if ($env:VYX_DCI_PYTHON) { $env:VYX_DCI_PYTHON } else { "python" }
$Options = @()
if ($Force) { $Options += '--force' }
Push-Location $Project
try {
    & $Python (Join-Path $Root "tools/dci/dci.py") cpp-import Vyx.toml --target dci_qt_counter --import qt_widgets -j $Jobs @Options
    if ($LASTEXITCODE -ne 0) { throw "Qt DCI Adapter preparation failed" }
} finally { Pop-Location }

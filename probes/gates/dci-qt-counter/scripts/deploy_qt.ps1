param(
    [string]$QtRoot = "",
    [string]$Program = ""
)
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($QtRoot)) { $QtRoot = $env:QTDIR }
if ([string]::IsNullOrWhiteSpace($QtRoot)) { $QtRoot = "E:/Qt/6.7.3/msvc2019_64" }
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$QtBin = Join-Path $QtRoot "bin"
$Deploy = Join-Path $QtBin "windeployqt.exe"
$QtPaths = Join-Path $QtBin "qtpaths.exe"
foreach ($Tool in @($Deploy, $QtPaths)) {
    if (-not (Test-Path -LiteralPath $Tool -PathType Leaf)) {
        throw "Qt deployment tool missing: $Tool; set QTDIR to the matching Qt installation"
    }
}
if ([string]::IsNullOrWhiteSpace($Program)) {
    $Program = Join-Path $PSScriptRoot "../target/dci_qt_counter.exe"
}
$Program = (Resolve-Path -LiteralPath $Program).Path

$SavedPath = $env:PATH
$SavedVc = $env:VCINSTALLDIR
try {
    $env:PATH = "$QtBin;$SavedPath"
    if ([string]::IsNullOrWhiteSpace($env:VCINSTALLDIR)) {
        $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
        if (Test-Path -LiteralPath $VsWhere) {
            $VsRoot = & $VsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -ne 0) { throw "Visual Studio runtime discovery failed" }
            if ($VsRoot) { $env:VCINSTALLDIR = Join-Path $VsRoot "VC" }
        }
    }
    # postbuild also runs after a cached link, so deleted deployment files are restored.
    & $Deploy --release --no-translations --compiler-runtime --include-plugins qoffscreen --qtpaths $QtPaths $Program
    if ($LASTEXITCODE -ne 0) { throw "Qt deployment failed: exit=$LASTEXITCODE" }
    $Output = Split-Path -Parent $Program
    foreach ($Relative in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'platforms/qwindows.dll', 'platforms/qoffscreen.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Output $Relative) -PathType Leaf)) {
            throw "Qt deployment did not produce $Relative beside $Program"
        }
    }
} finally {
    $env:PATH = $SavedPath
    $env:VCINSTALLDIR = $SavedVc
}

param(
    [string]$Compiler = "",
    [string]$QtRoot = "",
    [int]$Jobs = 4,
    [string]$ResultDir = "",
    [switch]$Clean,
    [switch]$Interactive
)
$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$Probe = $PSScriptRoot
$LLVM = Join-Path $Root "clang"
$env:LLVM_ROOT = $LLVM
$env:PATH = "$LLVM\bin;$Root\bootstrap_compiler\out;$env:PATH"

if ([string]::IsNullOrWhiteSpace($Compiler)) {
    $Compiler = Join-Path $Root "bootstrap_compiler\out\boot.exe"
}
if (-not (Test-Path -LiteralPath $Compiler)) {
    throw "SDK compiler missing: $Compiler; build the current tree first"
}
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path

$Qt = $QtRoot
if ([string]::IsNullOrWhiteSpace($Qt)) { $Qt = $env:QTDIR }
if ([string]::IsNullOrWhiteSpace($Qt)) {
    $Qt = "E:\Qt\6.7.3\msvc2019_64"
}
$Qt = (Resolve-Path $Qt).Path
$QtInc = Join-Path $Qt "include"
$QtCoreRoot = Join-Path $QtInc "QtCore"
$QtGuiRoot = Join-Path $QtInc "QtGui"
$QtWidgetsRoot = Join-Path $QtInc "QtWidgets"
$QApplicationH = Join-Path $QtWidgetsRoot "qapplication.h"
$QWidgetH = Join-Path $QtWidgetsRoot "qwidget.h"
$QLCDNumberH = Join-Path $QtWidgetsRoot "qlcdnumber.h"
$QPushButtonH = Join-Path $QtWidgetsRoot "qpushbutton.h"
$QAbstractButtonH = Join-Path $QtWidgetsRoot "qabstractbutton.h"
$QBoxLayoutH = Join-Path $QtWidgetsRoot "qboxlayout.h"
$QLayoutH = Join-Path $QtWidgetsRoot "qlayout.h"
$QFrameH = Join-Path $QtWidgetsRoot "qframe.h"
$QCoreEventH = Join-Path $QtCoreRoot "qcoreevent.h"
$QObjectH = Join-Path $QtCoreRoot "qobject.h"
$QCoreAppH = Join-Path $QtCoreRoot "qcoreapplication.h"
$QStringH = Join-Path $QtCoreRoot "qstring.h"
if (-not (Test-Path $QApplicationH)) {
    throw "Qt Widgets headers missing under $QtWidgetsRoot; set QTDIR"
}

$Clangxx = Join-Path $LLVM "bin\clang++.exe"
if (-not (Test-Path $Clangxx)) {
    throw "clang++ missing: $Clangxx"
}

$Python = if (-not [string]::IsNullOrWhiteSpace($env:VYX_DCI_PYTHON)) { $env:VYX_DCI_PYTHON } else { "python" }
$Dci = Join-Path $Root "tools\dci\dci.py"
$Contracts = Join-Path $Probe "contracts"
if ([string]::IsNullOrWhiteSpace($ResultDir)) {
    $ResultDir = Join-Path $Root ".runs\dci-qt-counter"
}
$OutDir = [IO.Path]::GetFullPath($ResultDir)
New-Item -ItemType Directory -Force -Path $Contracts, $OutDir | Out-Null

if (-not [string]::IsNullOrWhiteSpace($env:DCI_ADAPTER_JOBS)) {
    $Jobs = [int]$env:DCI_ADAPTER_JOBS
}
if ($Jobs -lt 1) { throw "Jobs must be positive" }

$Overlay = Join-Path $Probe "native\qt_widgets_dci.hpp"
$Dcib = Join-Path $Contracts "qt_widgets.dcib"
$DebugJson = Join-Path $OutDir "qt_widgets.dci.json"
$TestNative = Join-Path $Probe "checks\native"
$TestContracts = Join-Path $Probe "checks\contracts"
$ExceptionHeader = Join-Path $TestNative "exception_test.hpp"
$TestDcib = Join-Path $TestContracts "qt_widgets_unwind.dcib"
$QtLifetimeFacts = Join-Path $Root "tools/dci/profiles/qt_widgets.hpp"
$QtHeaders = @($Overlay, $QtLifetimeFacts, $QApplicationH, $QWidgetH, $QLCDNumberH, $QPushButtonH, $QAbstractButtonH, $QObjectH, $QCoreAppH, $QStringH, $QBoxLayoutH, $QLayoutH, $QFrameH, $QCoreEventH)
$AssertPy = Join-Path $Probe "assert_contract.py"

function New-QtContract {
    param([string[]]$Headers, [string]$Contract, [string]$Debug, [switch]$Unwind)
    $ExtraRoots = @()
    $Assertions = @()
    if ($Unwind) {
        $ExtraRoots = @('--project-root', $TestNative)
        $Assertions = @('--unwind')
    }
    & $Python $Dci adapter --language cpp @Headers `
    -o $Contract `
    --debug-json $Debug `
    --boundary shared_abi `
    --triplet windows_x64 `
    --std c++17 `
    --toolchain clang `
    --clang $Clangxx `
    -j $Jobs `
    --project-root (Split-Path -Parent $QtLifetimeFacts) `
    --project-root (Join-Path $Probe "native") `
    --project-root $QtWidgetsRoot `
    --project-root $QtGuiRoot `
    --project-root $QtCoreRoot `
    -I $QtInc `
    -I $QtCoreRoot `
    -I $QtGuiRoot `
    -I $QtWidgetsRoot `
    -I (Join-Path $Qt "mkspecs\win32-msvc") `
    --clang-arg=-fms-compatibility `
    --clang-arg=-DQT_CORE_LIB `
    --clang-arg=-DQT_GUI_LIB `
    --clang-arg=-DQT_WIDGETS_LIB `
    --clang-arg=-DUNICODE `
    --clang-arg=-D_UNICODE `
    --clang-arg=-DWIN32 `
    --clang-arg=-D_WIN64 @ExtraRoots
    if ($LASTEXITCODE -ne 0) { throw "adapter failed: $Contract" }

    & $Python $AssertPy (Join-Path $Root "tools\dci") $Contract @Assertions
    if ($LASTEXITCODE -ne 0) { throw "qt widgets contract assertions failed: $Contract" }
    & $Python $Dci validate --strict $Contract
    if ($LASTEXITCODE -ne 0) { throw "qt widgets shared ABI contract validation failed: $Contract" }
}

# Remove obsolete translator output only after verifying the new shared ABI.
foreach ($LegacyStub in @((Join-Path $Probe "generated/translate_unwind.cpp"),
                          (Join-Path $OutDir "translate_unwind.cpp"),
                          (Join-Path $Contracts "qt_widgets_translate_unwind.cpp"))) {
    if (Test-Path -LiteralPath $LegacyStub -PathType Leaf) { Remove-Item -LiteralPath $LegacyStub }
}

$env:DCI_QT_CXX = $Clangxx
$env:DCI_QT_INCLUDE = $QtInc
$env:DCI_QT_INCLUDE_QTCORE = $QtCoreRoot
$env:DCI_QT_INCLUDE_QTGUI = $QtGuiRoot
$env:DCI_QT_INCLUDE_QTWIDGETS = $QtWidgetsRoot
$env:DCI_QT_MKSPECS = Join-Path $Qt "mkspecs\win32-msvc"
$env:DCI_QT_LIBDIR = Join-Path $Qt "lib"
$env:QTDIR = $Qt
$env:QT_PLUGIN_PATH = Join-Path $Qt "plugins"
$env:PATH = "$(Join-Path $Qt 'bin');$env:PATH"

Write-Host "== project build with DCI artifact preparation =="
Push-Location $Probe
try {
    if ($Clean) {
        foreach ($leaf in @('.cache', 'target')) {
            $artifact = [IO.Path]::GetFullPath((Join-Path $Probe $leaf))
            if ([IO.Path]::GetDirectoryName($artifact) -ne [IO.Path]::GetFullPath($Probe)) {
                throw "Generated path escaped the Qt gate: $artifact"
            }
            if (Test-Path -LiteralPath $artifact) { Remove-Item -LiteralPath $artifact -Recurse -Force }
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Probe 'src/qt_widgets.vyx'))) {
        throw "Visible Qt definitions missing: use dci convert to create src/qt_widgets.vyx"
    }
    & $Compiler build --target dci_qt_counter -j $Jobs
    if ($LASTEXITCODE -ne 0) { throw "SDK compiler build dci_qt_counter failed" }
} finally { Pop-Location }

$Exe = Join-Path $Probe "target\dci_qt_counter.exe"
if (-not (Test-Path $Exe)) { $Exe = Join-Path $Probe "out\dci_qt_counter.exe" }
if (-not (Test-Path $Exe)) { throw "dci_qt_counter.exe not produced" }

& $Python $AssertPy (Join-Path $Root "tools/dci") $Dcib
if ($LASTEXITCODE -ne 0) { throw "Qt import signature assertions failed" }
& $Python $Dci validate --strict $Dcib
if ($LASTEXITCODE -ne 0) { throw "Qt import contract validation failed" }
& $Python $Dci decode $Dcib $DebugJson
if ($LASTEXITCODE -ne 0) { throw "Qt import inspection failed" }

Write-Host "== AOT contract and event-loop checks =="
& $Python (Join-Path $Probe "check.py") --compiler $Compiler --program $Exe --contract $Dcib --qt $Qt --result (Join-Path $OutDir "result.json")
if ($LASTEXITCODE -ne 0) { throw "Qt counter gate failed" }
& $Python (Join-Path $Probe "check_bridge.py") --qt $Qt --result (Join-Path $OutDir "bridge-result.json")
if ($LASTEXITCODE -ne 0) { throw "Qt member bridge lifetime gate failed" }

Write-Host "== adapter: separate shared ABI exception test contract =="
New-Item -ItemType Directory -Force -Path $TestContracts | Out-Null
New-QtContract -Headers ($QtHeaders + @($ExceptionHeader)) -Contract $TestDcib -Debug (Join-Path $OutDir "qt_widgets_unwind.dci.json") -Unwind
& $Python (Join-Path $Probe "check_unwind.py") --compiler $Compiler --qt $Qt --result (Join-Path $OutDir "unwind-result.json")
if ($LASTEXITCODE -ne 0) { throw "Qt shared ABI propagation/cleanup gate failed" }
if ($Interactive) {
    Write-Host "== interactive counter: close the window to finish =="
    & $Exe
    if ($LASTEXITCODE -ne 0) { throw "dci_qt_counter exit=$LASTEXITCODE" }
}
Write-Output "dci-qt-counter OK: exit=0"

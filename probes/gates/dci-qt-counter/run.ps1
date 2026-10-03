$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$Probe = $PSScriptRoot
$LLVM = Join-Path $Root "clang"
$env:LLVM_ROOT = $LLVM
$env:PATH = "$LLVM\bin;$Root\bootstrap_compiler\out;$env:PATH"

$Boot = Join-Path $Root "bootstrap_compiler\out\boot.exe"
if (-not (Test-Path $Boot)) {
    throw "boot.exe missing; rebuild bootstrap_compiler/out/boot.exe"
}

$Qt = $env:QTDIR
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
$Generated = Join-Path $Probe "generated"
$OutDir = Join-Path $Probe "run-output"
New-Item -ItemType Directory -Force -Path $Contracts, $Generated, $OutDir | Out-Null

$Jobs = if (-not [string]::IsNullOrWhiteSpace($env:DCI_ADAPTER_JOBS)) {
    [int]$env:DCI_ADAPTER_JOBS
} else {
    [Math]::Max(1, [Math]::Min(32, [Environment]::ProcessorCount))
}

$Overlay = Join-Path $Probe "native\qt_widgets_dci.hpp"
$Dcib = Join-Path $Contracts "qt_widgets.dcib"
$DebugJson = Join-Path $OutDir "qt_widgets.dci.json"
$Stub = Join-Path $Generated "translate_unwind.cpp"

Write-Host "== adapter: Qt Widgets Counter surface =="
& $Python $Dci adapter --language cpp $Overlay $QApplicationH $QWidgetH $QLCDNumberH $QPushButtonH $QAbstractButtonH $QObjectH $QCoreAppH $QStringH `
    -o $Dcib `
    --debug-json $DebugJson `
    --stub-out $Stub `
    --triplet windows_x64 `
    --std c++17 `
    --toolchain clang `
    --clang $Clangxx `
    -j $Jobs `
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
    --clang-arg=-D_WIN64
if ($LASTEXITCODE -ne 0) { throw "adapter failed" }

Write-Host "== contract symbols =="
$AssertPy = Join-Path $OutDir "assert_qt_widgets.py"
& $Python $AssertPy (Join-Path $Root "tools\dci") $Dcib
if ($LASTEXITCODE -ne 0) { throw "qt widgets contract assertions failed" }

$env:DCI_QT_CXX = $Clangxx
$env:DCI_QT_INCLUDE = $QtInc
$env:DCI_QT_INCLUDE_QTCORE = $QtCoreRoot
$env:DCI_QT_INCLUDE_QTGUI = $QtGuiRoot
$env:DCI_QT_INCLUDE_QTWIDGETS = $QtWidgetsRoot
$env:DCI_QT_MKSPECS = Join-Path $Qt "mkspecs\win32-msvc"
$env:DCI_QT_LIBDIR = Join-Path $Qt "lib"
$env:QT_PLUGIN_PATH = Join-Path $Qt "plugins"
$env:PATH = "$(Join-Path $Qt 'bin');$env:PATH"

Write-Host "== vyx build =="
Set-Location $Probe
if (Test-Path (Join-Path $Probe ".cache")) { Remove-Item -Recurse -Force (Join-Path $Probe ".cache") }
if (Test-Path (Join-Path $Probe "target")) { Remove-Item -Recurse -Force (Join-Path $Probe "target") }
& $Boot build --target dci_qt_counter -j 4
if ($LASTEXITCODE -ne 0) { throw "boot build dci_qt_counter failed" }

$Exe = Join-Path $Probe "target\dci_qt_counter.exe"
if (-not (Test-Path $Exe)) { $Exe = Join-Path $Probe "out\dci_qt_counter.exe" }
if (-not (Test-Path $Exe)) { throw "dci_qt_counter.exe not produced" }

Write-Host "== run $Exe =="
& $Exe
if ($LASTEXITCODE -ne 0) { throw "dci_qt_counter exit=$LASTEXITCODE" }
Write-Output "dci-qt-counter OK: exit=0"

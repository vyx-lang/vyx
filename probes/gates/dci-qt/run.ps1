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
$QtCoreRoot = Join-Path $Qt "include\QtCore"
$QtUmbrella = Join-Path $QtCoreRoot "QtCore"
if (-not (Test-Path $QtUmbrella)) {
    throw "QtCore umbrella not found at $QtUmbrella; set QTDIR to a Qt 6 msvc_64 prefix"
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

$Overlay = Join-Path $Probe "native\qt_dci.hpp"
$Dcib = Join-Path $Contracts "qt.dcib"
$DebugJson = Join-Path $OutDir "qt.dci.json"
$Stub = Join-Path $Generated "translate_unwind.cpp"
$InspectLog = Join-Path $OutDir "inspect.txt"
$Coverage = Join-Path $OutDir "coverage.txt"

Write-Host "== adapter: public QtCore module ($QtUmbrella) =="
& $Python $Dci adapter --language cpp $Overlay $QtUmbrella `
    -o $Dcib `
    --debug-json $DebugJson `
    --stub-out $Stub `
    --triplet windows_x64 `
    --std c++17 `
    --toolchain clang `
    --clang $Clangxx `
    -j $Jobs `
    --project-root $QtCoreRoot `
    -I (Join-Path $Qt "include") `
    -I $QtCoreRoot `
    -I (Join-Path $Qt "mkspecs\win32-msvc") `
    --clang-arg=-fms-compatibility `
    --clang-arg=-DQT_CORE_LIB `
    --clang-arg=-DUNICODE `
    --clang-arg=-D_UNICODE `
    --clang-arg=-DWIN32 `
    --clang-arg=-D_WIN64
if ($LASTEXITCODE -ne 0) { throw "adapter failed" }

Write-Host "== dci inspect =="
& $Python $Dci inspect $Dcib | Tee-Object -FilePath $InspectLog
if ($LASTEXITCODE -ne 0) { throw "inspect failed" }

Write-Host "== dci validate --strict =="
& $Python $Dci validate --strict $Dcib
if ($LASTEXITCODE -ne 0) { throw "validate --strict failed" }

Write-Host "== QtCore coverage (real types, not helpers) =="
$AssertPy = Join-Path $OutDir "assert_qt.py"
@'
import collections
import json
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
import dcib

doc = dcib.decode(Path(sys.argv[2]).read_bytes())
ex = doc["exports"]
layouts = ex.get("layouts") or []
syms = ex.get("symbols") or []
rej = ex.get("rejected_symbols") or []
vt = ex.get("vtables") or []

def layout_named(*names):
    return [L for L in layouts if L.get("type_name") in names]

owners = collections.Counter((s.get("owner") or "<free>") for s in syms)
rej_reasons = collections.Counter(
    (r.get("reason") or "").split(":")[0][:80] for r in rej
)
wanted_types = ["QPoint", "QSize", "QRect", "QString", "QByteArray", "QObject", "QVariant", "QUrl"]
print("layouts=%d symbols=%d rejected=%d vtables=%d" % (
    len(layouts), len(syms), len(rej), len(vt)))
for name in wanted_types:
    hits = layout_named(name)
    methods = [s.get("member_name") for s in syms if (s.get("owner") or "") == name]
    rejected = [r.get("member_name") or r.get("name") for r in rej if (r.get("owner") or "") == name]
    print("%s layout=%s methods=%d rejected_members=%d" % (
        name,
        [{"size": L.get("size"), "align": L.get("alignment"), "fields": len(L.get("fields") or [])} for L in hits],
        len(methods),
        len(rejected),
    ))
    if methods[:8]:
        print("  methods_sample=%s" % methods[:8])
qversion = [s for s in syms if (s.get("member_name") or s.get("name")) == "qVersion"]
print("qVersion_exported=%d" % len(qversion))
print("top_owners=%s" % owners.most_common(12))
print("reject_reason_head=%s" % rej_reasons.most_common(8))
qp = layout_named("QPoint")
if not qp:
    print("FAILED: QPoint layout missing from QtCore contract", file=sys.stderr)
    sys.exit(1)
if int(qp[0].get("size") or 0) != 8:
    print("FAILED: QPoint size %s" % qp[0], file=sys.stderr)
    sys.exit(1)
qp_methods = [s.get("member_name") for s in syms if (s.get("owner") or "") == "QPoint"]
if "manhattanLength" not in qp_methods and "x" not in qp_methods:
    print("FAILED: QPoint methods missing; got %s" % qp_methods[:20], file=sys.stderr)
    sys.exit(1)
print("assert OK")
'@ | Set-Content -Encoding utf8 $AssertPy
& $Python $AssertPy (Join-Path $Root "tools\dci") $Dcib | Tee-Object -FilePath $Coverage
if ($LASTEXITCODE -ne 0) { throw "qt contract assertions failed" }

$env:DCI_QT_CXX = $Clangxx
$env:DCI_QT_INCLUDE = Join-Path $Qt "include"
$env:DCI_QT_INCLUDE_QTCORE = $QtCoreRoot
$env:DCI_QT_MKSPECS = Join-Path $Qt "mkspecs\win32-msvc"
$env:DCI_QT_LIBDIR = Join-Path $Qt "lib"
$env:PATH = "$(Join-Path $Qt 'bin');$env:PATH"

Write-Host "== vyx build =="
Set-Location $Probe
if (Test-Path (Join-Path $Probe ".cache")) { Remove-Item -Recurse -Force (Join-Path $Probe ".cache") }
if (Test-Path (Join-Path $Probe "target")) { Remove-Item -Recurse -Force (Join-Path $Probe "target") }
& $Boot build --target dci_qt -j 4
if ($LASTEXITCODE -ne 0) { throw "boot build dci_qt failed" }

$Exe = Join-Path $Probe "target\dci_qt.exe"
if (-not (Test-Path $Exe)) { $Exe = Join-Path $Probe "out\dci_qt.exe" }
if (-not (Test-Path $Exe)) { throw "dci_qt.exe not produced" }

Write-Host "== run $Exe =="
& $Exe
if ($LASTEXITCODE -ne 0) { throw "dci_qt exit=$LASTEXITCODE" }
Write-Output "dci-qt OK: exit=0"

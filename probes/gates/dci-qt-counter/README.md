# Qt Widgets DCI counter

This Windows AOT gate imports Qt Widgets directly from the C++ headers. A Vyx
`Counter : QWidget` owns its counter state, and a separate test object overrides
Qt virtual event handlers. No C facade replaces
the Qt objects or event loop.

## Scope and application usability

This executable is a compiler integration regression. Passing this gate establishes
the exercised ABI, virtual dispatch, event-loop and cleanup paths; it does not
establish complete Qt application support.

The application maintains visible Vyx definitions in `src/qt_widgets.vyx`.
Its `extern "dci"` block exposes native classes, their public base hierarchy,
constructors, destructors and const/virtual signatures. The Consumer validates
these definitions against the measured contract; `CounterCheck : QObject`
implements `override fn timerEvent(...)` using this surface.

`[dci.import.qt_widgets]` names the original public header, module, explicit
`export_types`, selected `ownership_headers` and optional `qt_connections`
for the independent DCI toolchain. The target's `dci_imports` runs preparation
before source discovery, publishing the project contract to
`contracts/qt_widgets.dcib`. `definitions = "src/qt_widgets.vyx"` selects the
maintained Vyx file; preparation never writes it or adds another generated module.
Native materialization and dependency stamps live in `.cache/dci/qt_widgets/`.
Deleting that cache is supported by an ordinary build. `scripts/prepare_dci.ps1`
also remains available as an independent preparation command.
No per-method selection JSON or application-written C wrapper is required.

C++ constant default values are measured by the producer compiler and emitted
on the original Vyx signatures. The Consumer supplies them at the call site:
calls `QApplication(&mut argc, argv)`, `layout.addWidget(widget)` and
`self.startTimer(50)` directly. It does not turn Qt constants into application
helper functions. No shortened-signature default wrapper is generated.
`layout.addWidget` resolves the native inherited `QLayout::addWidget(QWidget*)`;
the generic QFlags overload is diagnosed as outside the current Converter surface.
Ordinary measured methods remain direct DCI calls, and `cpp_include` references
`qt_widgets_dci.hpp`, containing original Qt includes.

The Qt profile recognizes `Q_SIGNALS` / `Q_SIGNAL` in the producer AST and
validates the explicitly selected `QAbstractButton::clicked(bool)` operation.
It provides the optional `on_clicked` connection method. For example,
`plus.on_clicked(context, counter_increment, state)` connects the native
`clicked(bool)` signal through a checked `cfn(rawptr,bool)` callback. This is
an adapter-generated connection API; the native signal emission method keeps
its original meaning. Qt disconnects when sender or context dies. This
application uses its window as both context and state owner, and uses `unsafe`
for the borrowed native state address. `unsafe` does not change an ABI.

`src/counter.vyx` owns each window's scalar state and a borrowed LCD pointer.
`src/counter_test.vyx` supplies the separate timer-driven regression.
`src/main.vyx` keeps real argument strings and a native-address argv array
alive through application destruction. Children leave scope before the parent.
The target explicitly selects the SDK's authored library lifetime facts in
`tools/dci/profiles/qt_widgets.hpp`. The application's public header contains
only original Qt includes. Pointer retention is not inferred from C++ spelling;
other APIs without an authoritative fact remain rejected.

The import cache hashes the transitive compiler-reported header dependencies,
producer compilers, DCI tools, build configuration and user-declared export scope.
Existing compatible contracts are reused, including standalone Adapter output.
Rebuilding missing cache outputs does not re-extract the producer AST or rewrite
the contract. Generated contracts record producer input hashes outside the cache;
known native input changes trigger measurement. Plain offline contracts without
this provenance retain provider-managed version freshness. Invalid supplied
contracts fail rather than being overwritten. An ordinary source
edit reuses unchanged producer facts and cannot expand the export scope. Rejected ABI,
overload and lifecycle cases are recorded in `report.json` and cannot provide
an executable binding.

This is a selected Qt Widgets surface, not complete Qt bindings. The source
generator currently emits global C++ class names, and callback payloads are
closed scalar values with void return. Capturing Vyx closures, arbitrary C++
member-pointer values, queued callback payload ownership, and consumer-owned
aggregate/string fields in native subclasses require additional protocols and
are rejected. The generic `shared_abi` exception implementation does not supply
those higher-level bindings by itself.

The contract uses `shared_abi` with `dci.eh.msvc-cxx.v1`: native C++ exceptions
propagate through Vyx frames and run their object cleanup. The adapter does not
generate `translate_unwind.cpp` or convert exceptions to `dci.Failure` here.
Generated C++ subclass callbacks remain necessary for Qt virtual dispatch.
The compiler generates those callbacks as part of the project build.
The Vyx bindings explicitly declare complete-object destructors. Buttons live
in a scope inside their parent window, and the widget function returns before
the application's destructor runs, so Qt parenting and Vyx ownership agree.

Build the current SDK compiler first (see [AGENTS.md](../../../AGENTS.md)), then
run from the repository root:

```powershell
./probes/gates/dci-qt-counter/run.ps1 -Compiler ./bootstrap_compiler/out/boot.exe -QtRoot E:/Qt/6.7.3/msvc2019_64 -Jobs 4 -Clean
```

`QTDIR` supplies the Qt root when `-QtRoot` is omitted. The gate needs Qt headers,
import libraries, DLLs and the offscreen platform plugin, plus the repository's
LLVM 22 development tools for the C++ adapter. `VYX_DCI_PYTHON` can select Python.
`DCI_ADAPTER_JOBS` overrides the adapter/build job count. `-Interactive` also
opens the counter after automatic checks; close its window to finish.

## Build and launch from the IDE

Use the normal project build command in this directory. It uses an existing
compatible contract and rebuilds missing cache artifacts automatically:

```powershell
vyxc --run=aot --src=project . --target dci_qt_counter
vyxc --emit=ir --src=project .
```

To explicitly regenerate or review the Vyx surface from the measured contract:

```powershell
python ../../../tools/dci/dci.py convert contracts/qt_widgets.dcib --module qt.widgets --header qt_widgets_dci.hpp -o src/qt_widgets.next.vyx --report contracts/conversion-report.json
```

Review this separate output before updating `src/qt_widgets.vyx`. The standalone
Converter protects existing files; `--force` is an explicit overwrite operation.
The producer materialization retains native C++ linkage and its real mangled
symbols are measured by the Adapter. The current reverse-override Stub backend
still uses internal C-linkage factory/state/callback entries; this gate does not
establish an entirely C-ABI-free implementation.

The target's `postbuild` hook calls `scripts/deploy_qt.ps1`. Qt's `windeployqt`
copies the release DLLs and platform plugins into `target/`, including after a
cached link. `QTDIR` selects the matching Qt installation; the default is
`E:/Qt/6.7.3/msvc2019_64`. This deployment does not require a Qt directory in the
IDE's `PATH` or `QT_PLUGIN_PATH`. The Microsoft C++ runtime must be installed;
when Visual Studio is available, deployment also includes its redistributable
installer.

Windows exit `0xC0000135` means a required DLL could not be loaded. This counter
imports `Qt6Widgets.dll` and `Qt6Core.dll`; Widgets also requires `Qt6Gui.dll`.
The Windows window system plugin is `target/platforms/qwindows.dll`, and the
headless check uses `target/platforms/qoffscreen.dll`.

The script regenerates the binary contract, asserts the exact signatures,
builds and deploys the application, then runs the Qt event loop with both the
offscreen and Windows plugins without the Qt installation in the runtime PATH.
It checks increment/decrement, holding a button across multiple timer events,
LCD values, Qt-to-Vyx virtual dispatch and normal shutdown after object cleanup.
Negative compilations reject wrong pointer depth, `i8*` in place of C++ `char*`,
and mismatched callback payload/return types. A pointer-storage check distinguishes
Vyx pointer handles from native addresses. Native O0/O2 checks exercise 1000
context destructions and sender-first destruction. A separate O0/O2 exception
check constructs actual QString, QLCDNumber and
QPushButton objects, preserves the original exception at a native C++ catcher,
checks reverse Qt-to-Vyx virtual exception propagation and initializer failure,
observes QObject destruction, and measures balanced Vyx-owned storage.
The check calls a public virtual method directly rather than throwing from the
Qt event dispatcher. Results and compiler hashes go to `.runs/dci-qt-counter/`; pass
`-ResultDir` to choose another location. This gate is separate from the
`tests/projects/dci_*` project scan. Linux Qt has not been validated here.

## Application and exception test inputs

The application imports `contracts/qt_widgets.dcib`, generated from the Qt
headers and the SDK Qt lifetime profile. Its contract contains
no exception test functions, and its target does not link the test C++ source.

`checks/native/exception_test.hpp` and `.cpp` are handwritten regression
instrumentation, used only by `check_unwind.py` and `checks/unwind.vyx`. The gate
generates their separate `checks/contracts/qt_widgets_unwind.dcib` contract:

| Test function | What it verifies |
|---|---|
| `qt_counter_watch` | Records actual QObject destruction and its order |
| `qt_counter_throw` | Throws a known C++ exception and checks native stack cleanup |
| `qt_counter_call_visible` | Calls a Qt virtual method to test exception propagation through a Vyx override |

These names and the test header have no special meaning to DCI. Other C++
projects import their own public headers with `--boundary shared_abi` and a
compatible supported unwind ABI. The compiler emits invokes, frame cleanup and
required native bridges from their contracts; applications do not implement
these three functions. Ownership facts and destructor declarations still have
to describe the actual library's lifetime rules.

## Type identity

The C++ adapter preserves `char`, `signed char` and `unsigned char` as distinct
identities. The counter imports C++ text/argv parameters as `*char` / `**char`
and explicitly casts UTF-8 byte addresses at the boundary. It does not load or
index those pointers as Vyx Unicode scalar values. The contract's nested
`pointee` objects define pointer depth; the repeated terminal `name` is not a
complete pointer type. Do not change the producer mapping to `i8` to bypass an
incorrect consumer declaration.

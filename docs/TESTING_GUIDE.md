# Vyx bootstrap and verification guide

[简体中文](TESTING_GUIDE_ZH.md) · [Documentation index](README.md)

This document describes the current execution path. The archived C++ host and
a pre-existing `bootstrap_compiler/out/` binary are not evidence for the current
source tree. Select a compatible Stage 0 explicitly and record its identity.
The current regression baseline is native AOT; JIT parity and performance are
deferred work.

## Stage 0 compiler

Download a compatible SDK from [Releases latest](https://github.com/vyx-lang/vyx/releases/latest)
and set `VYX_BOOTSTRAP_VYXC` to its `vyxc`, or explicitly select the compatible
repository seed. The Git-ignored `dist/` directory is for local packaging.

```powershell
$env:VYX_BOOTSTRAP_VYXC = (Get-Command vyxc -ErrorAction Stop).Source
$env:LLVM_ROOT = (Resolve-Path .\clang).Path

if (-not (Test-Path $env:VYX_BOOTSTRAP_VYXC)) {
    throw 'Stage 0 compiler not found.'
}
```

The repository's test runner already recognises `VYX_BOOTSTRAP_VYXC`; passing
`-BootstrapCompiler` is equivalent and takes precedence. A locally found
`out/vyxc.exe` becomes a valid candidate only after Stage 0 has rebuilt it from
this checkout.

Preflight, targeting the repository Windows SDK layout:

```powershell
foreach ($tool in @('clang.exe', 'clang++.exe', 'llvm-ar.exe')) {
  $path = Join-Path (Join-Path (Resolve-Path .\clang).Path 'bin') $tool
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "repository LLVM SDK is missing $tool`: $path"
  }
}
```

Do not substitute a pre-existing `out/vyxc.exe` for the Stage 0 compiler. On a
host that cannot run the Windows compiler, pass an explicitly selected
compatible release compiler with `-SeedCompiler` and record its hash; never
silently change the toolchain.

## Build this checkout

Run from the repository root:

```powershell
$compilerTarget = [regex]::Match((Get-Content .\bootstrap_compiler\Vyx.toml -Raw), '(?m)^name\s*=\s*"([^"]+)"').Groups[1].Value
Push-Location .\bootstrap_compiler
& $env:VYX_BOOTSTRAP_VYXC build --target $compilerTarget -j10
if ($LASTEXITCODE -ne 0) { throw "bootstrap build failed: $LASTEXITCODE" }
Copy-Item -LiteralPath ".\out\$compilerTarget.exe" -Destination .\out\vyxc.exe -Force
Pop-Location

$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path
$runtime = (Resolve-Path .\bootstrap_compiler\out).Path
```

The project manifest builds the private `vyx_compiler_backend`, the static
`vyx_runtime`, the SDK compiler entry point `vyxc`, and self-hosted tools. The
compiler target is read from `[package].name` in the manifest; the copy step
updates the SDK entry point even when automatic alias deployment is skipped.
LLVM is a compiler dependency only; generated applications link `vyx_runtime` and do not
load LLVM or a Vyx runtime DLL. This Windows setup uses the repository LLVM SDK
at `clang/`; another installation needs an explicit `LLVM_ROOT`. Pair the tested
SDK compiler with its own backend and runtime. Do not substitute the frozen C++ host.

Package the canonical SDK after rebuilding:

```powershell
.\bootstrap_compiler\scripts\package_sdk.ps1 -Archive
```

On Linux, add `-BundleLlvm` to include the compact compiler-only LLVM 22
toolchain. This does not change the application contract: application outputs
still contain only their code and the statically linked Vyx runtime.

## Backend policy

MIR2LLVM is the supported bootstrap backend. MIR2CPP and `--emit=cpp` are
experimental and unstable, so MIR2CPP output is not a release or fixed-point
acceptance gate. Both output paths accept `--triplet=<triple>`; target selection
does not change the backend's stability status.
Use AOT for the gates in this guide; a JIT result is not a substitute.

MIR2CPP-only cases in the project sweep therefore self-skip (exit code 77,
reported as skipped); set `VYX_ACCEPT_MIR2CPP=1` to force them when validating
that backend by hand.

## CLI and diagnostics

```powershell
# Identify the compiler under test (1.0.0-alpha.1 EA, LLVM, this binary).
& $compiler --version
& $compiler version
```

Use one explicit source mode and one output mode for a reproducible invocation:

```powershell
# Native executable (the default output is also `exe`).
& $compiler --src=file .\path\to\main.vyx --emit=exe -o .\out\main.exe

# Structured compiler inspection, without producing a program.
& $compiler --src=file .\path\to\main.vyx --dump-hir2
& $compiler --src=file .\path\to\main.vyx --dump-mir2

# Execute a successfully compiled input through the selected runtime mode.
& $compiler --src=file .\path\to\main.vyx --run=aot

# Package commands (cwd is a Vyx package / fixture, not the compiler tree).
& $compiler test                          # @[test] under tests/
& $compiler bench                         # @[bench] under benches/; prints ms
& $compiler fmt --check .\src\main.vyx    # token reprint; exits 1 if dirty
& $compiler doc .\src\main.vyx            # markdown from ///
```

The supported `--emit` values are `ir`, `obj`, `exe`, `dll`, `lib`, `vyi`,
`cpp`, and `dci-stubs`. `cpp` is experimental under the backend policy above;
`ir`, `hir2`, and `mir2` inspection are distinct debug operations (`--dump-hir2`,
`--dump-mir2`) rather than emit kinds. Invalid or incomplete CLI options report
`E0003` and return a non-zero status before source compilation.

Source diagnostics use a stable code and source location, for example
`path/to/file.vyx:3:14: error: E1000: ...`. Set `VYX_DIAG_JSON=1` when a tool
needs machine-readable records; keep the compiler's exit status as the failure
signal. `--verify-hir2` and `--verify-mir2` check compiler IR invariants only;
they supplement, and never replace, a compile/run contract for user programs.

## Cross compilation (Windows host)

`--triplet` selects the LLVM/backend target. Android is the supported
cross-link path from Windows when an NDK is visible. macOS triples can emit
LLVM IR and Mach-O objects from this LLVM SDK; linking a Darwin executable
still needs a macOS sysroot and is not a current verification gate.

```powershell
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
$env:ANDROID_NDK_HOME = $env:ANDROID_NDK_ROOT  # or NDK_HOME, or the newest NDK under the Android SDK
$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path

# Stage the Android runtime once per triplet (output: out/runtimes/android/arm64/ndk/).
Push-Location .\bootstrap_compiler
& $compiler build --target vyx_runtime --triplet aarch64-linux-android23
Pop-Location

# Then build a project with the same triplet.
& $compiler build --triplet aarch64-linux-android23
```

NDK `clang++` would otherwise always link `libc++_shared`. Android Vyx links
with `-nostdlib++` by default so a Vyx-only binary depends on Bionic
(`libc` / `libm` / `libdl`), not libc++. Opt in when the app already ships
libc++ or when the target has C++/DCI stubs:

- `libs_android = ["c++_shared"]` or `["c++_static"]` in `Vyx.toml`
- C++ sources or DCI C++ stubs (auto `c++_shared`)
- `VYX_ANDROID_LIBCXX=shared` or `static`

`VYX_ANDROID_LIBCXX=none` (or `0` / `off`) suppresses the auto-add even for
C++/DCI. Bionic has no `libpthread`; Android links do not add `-lpthread`.

`@[platform("posix")]` matches Linux, Android, and macOS. OS-facing std
(`os`, `fs`, `path`, `sync`, `threading`, `vio`) is `windows` vs `posix`.
`io_uring` and x86_64 `syscallN` stay Linux-only. Bionic has no
`getcontext`/`swapcontext`; Android fiber APIs are present as no-op stubs.

Do not place `--src=file` inputs under repository-root `out/` on Windows.

## Run the tests that match your change

`tests/run_all_modules.ps1` executes loose language modules and manifest
projects. It intentionally does not cover the separate contract-check catalogue.
Its summary reports passed, skipped, and failed runs separately. A project
script uses exit code `77` only when the fixture has an explicit platform
requirement, such as an x86_64 MSVC ABI descriptor; a skip is never counted as
a pass.

```powershell
# Full module/project sweep against the compiler just built above.
powershell -ExecutionPolicy Bypass -File .\tests\run_all_modules.ps1 `
  -BootstrapOnly -BootstrapCompiler $compiler -RuntimeDir $runtime -RuntimeLib vyx_runtime `
  -LogPath .\tests\.cache\bootstrap-current.log

# A focused language regression.
powershell -ExecutionPolicy Bypass -File .\tests\run_all_modules.ps1 `
  -BootstrapOnly -BootstrapCompiler $compiler -RuntimeDir $runtime -RuntimeLib vyx_runtime `
  -PathFilter 'cases\language\references\reference_receiver_ergonomics.vyx'

# Discover and run explicit checks.
powershell -ExecutionPolicy Bypass -File .\tests\checks\run_checks.ps1 -ListOnly
powershell -ExecutionPolicy Bypass -File .\tests\checks\run_checks.ps1 `
  -Check reference_receiver_ergonomics -BootstrapCompiler $compiler
```

Record the Stage 0 compiler hash/version, source commit, exact command, and produced log with
every all-suite claim. The branch may be mid-development; a command is a
reproducible measurement, not a predeclared pass badge.

## AOT compiler regression gates

### Gate A: exact hello MIR

The hello source must include `print("hello")`. On Windows, keep the temporary
source in the compiler project and run from that directory:

```powershell
$expected = 'mir2.unit functions=1 blocks=1 locals=1 places=1 values=6 instrs=2 cases=0 types=5'
$helloGateCreated = $false
Push-Location ./bootstrap_compiler
try {
    if (Test-Path ./hello-gate-doc.vyx) { throw 'hello-gate-doc.vyx already exists' }
    Copy-Item ../probes/gates/hello_gate.vyx ./hello-gate-doc.vyx
    $helloGateCreated = $true
    $summary = & $compiler --src=file ./hello-gate-doc.vyx --dump-mir2 2>$null |
        Select-String '^mir2.unit'
    if ($LASTEXITCODE -ne 0 -or $summary.Line -ne $expected) { throw 'hello MIR gate failed' }
    $summary.Line
} finally {
    if ($helloGateCreated) { Remove-Item ./hello-gate-doc.vyx -ErrorAction SilentlyContinue }
    Pop-Location
}
```

Compare the complete line, not just the presence of `mir2.unit`. In particular,
`values=6 instrs=2` must not drift without an explained change. Gate A does not
replace executable tests or the self-host fixed point below.

### Language, interface, and scheduling gates

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/match-expression/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/generic-direct-return.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/extended.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/long-chains.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/compiler-industrial/run.ps1 -Compiler $compiler -Scale 8 -Jobs 4
```

The gates cover [match values and diagnostics](../probes/gates/match-expression/README.md),
[cross-module globals/enums and generic returns](../probes/gates/cross-module/README.md),
[template artifact integrity and duplicate consumer links](../probes/gates/generic_interfaces/README.md),
and [cold/warm/incremental scheduling](../probes/gates/compiler-industrial/README.md).
They are separate entry points and are not automatically included by either
`tests/run_all_modules.ps1` or `tests/checks/run_checks.ps1`.

Scale 8/jobs 4 is the recorded Windows smoke matrix. Larger scale and job counts,
repeated measurements, memory limits, and real projects are needed for performance
claims. Scheduler fanout does not prove shared frontend analysis or concurrent
LLVM lowering.

### Gate C: DCI end to end

DCI, class ABI, and lowerer changes must compile and execute the project fixtures
with the tested SDK compiler, then run the relevant `probes/gates/dci-*` contracts:

```powershell
foreach ($name in @('dci_cpp_trait', 'dci_rust_trait', 'dci_multilang', 'dci_zig_abi')) {
    powershell -NoProfile -ExecutionPolicy Bypass -File "./tests/projects/$name/run.ps1" `
        -BootstrapCompiler $compiler -RuntimeDir $runtime
    if ($LASTEXITCODE -eq 77) { Write-Host "$name skipped on this platform" }
    elseif ($LASTEXITCODE -ne 0) { throw "$name failed" }
}
```

These four scripts currently require Windows and their native toolchains;
exit 77 is not a pass. Add the [DCI storage](../probes/gates/dci-storage/README.md),
[exception](../probes/gates/dci-exceptions/README.md),
[raw C++ vector](../probes/gates/dci-vector/README.md),
[Rust ecosystem](../probes/gates/dci-rust-ecosystem/README.md), and
[C++ ecosystem](../probes/gates/dci-cpp-ecosystem/README.md) gates as applicable.
With a Qt SDK, also run the [Qt Widgets counter](../probes/gates/dci-qt-counter/README.md).
It is separate from the `tests/projects/dci_*` scan and checks nested pointer
contracts, a real event loop, Qt-to-Vyx virtual dispatch, button/LCD state and
clean shutdown.
The old `dci_spdlog/run.sh` and `probes/gates/gate-c/` were retired on 2026-09-28;
they are not current acceptance commands. `--verify-mir2` and a Vyx FFI-only
program cannot replace DCI end-to-end execution.

## Project fixed point

The cold project S1/S2/S3 gate accepts the Stage 0 compiler directly. It copies
the source tree into isolated stages, builds each stage with the previous one,
then compares S2/S3 outputs and deterministic cache data:

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\bootstrap_compiler\scripts\test_project_selfhost_fixpoint.ps1 `
  -SeedCompiler $env:VYX_BOOTSTRAP_VYXC -LlvmRoot .\clang -Target $compilerTarget -TimeoutSec 1800
```

The same gate runs under PowerShell on Linux. Pair each compiler with the
runtime from the same stage; the script prepends that directory to
`LD_LIBRARY_PATH` so an older machine-wide runtime cannot override `$ORIGIN`:

```bash
export LLVM_ROOT=/usr/lib/llvm-22
export PATH="$LLVM_ROOT/bin:$PATH"
export VYX_BOOTSTRAP_VYXC=$(command -v vyxc)
compiler_target=$(awk -F '"' '/^name[ \t]*=/{print $2; exit}' bootstrap_compiler/Vyx.toml)
pwsh -NoProfile \
  -File bootstrap_compiler/scripts/test_project_selfhost_fixpoint.ps1 \
  -SeedCompiler "$VYX_BOOTSTRAP_VYXC" \
  -LlvmRoot "$LLVM_ROOT" \
  -Target "$compiler_target" \
  -TimeoutSec 1800
```

`-Target $compilerTarget` selects the compiler gate without also building LSP/DAP targets.
The script defaults to 3 isolated generations and 10 jobs; record those settings
and inspect the generated compiler fingerprints and comparisons. Do not mix
Release SDK and freshly built SDK compiler artifacts between stages.

The final 2026-10-01 artifact follow-up (`61d31e51`) also passed two in-place
self-host builds at `build --target $compilerTarget -j4 -O0`: S2 and S3 were byte-identical,
SHA-256 `13BBC891263F818FD25E9D10AC4CA92668EA863115B5A2DD98B876E4D66F06EF`.
Gate A retained the exact line above. This is dated Windows evidence; it is not
a claim that the isolated cold script, Linux, other optimization levels, or JIT
were run with that same binary. Retained results live under
`bootstrap_compiler/.runs/artifact-fixpoint/` and are not checked-in artifacts.

The older single-file A/B/C scripts and `scripts/build.ps1` are retained for
historical diagnostics. They assume an older host layout and are not the standard
way to establish the current compiler.

## Distribution packages

Release staging (`bootstrap_compiler/scripts/package_sdk.ps1 -Archive`) writes
`dist/vyx-sdk-windows-x86_64-llvm22.zip` and
`dist/vyx-sdk-linux-x86_64-llvm22.tar.gz`. Validate an extracted archive,
not only its staging directory:

```powershell
Expand-Archive .\dist\vyx-sdk-windows-x86_64-llvm22.zip .\vyx
.\vyx\vyx-sdk-windows-x86_64-llvm22\bin\vyxc.exe --help
```

```bash
tar -xzf dist/vyx-sdk-linux-x86_64-llvm22.tar.gz
./vyx-sdk-linux-x86_64-llvm22/bin/vyxc --help
```

Windows and Linux SDK packages bundle the LLVM backend, runtime, standard library,
and DCI tools. Normal SDK use does not require a separate LLVM installation.
Rebuilding the LLVM bridge from source requires LLVM 22 development tools and
libraries and the host build environment described above.

## IR and benchmark tooling

`tests/snapshots/ir/` is a host-era snapshot corpus. Its checked-in baselines
are useful for archaeology and migration work, but are not a current
acceptance gate. Do not report `ir_diff.ps1 -Strict` as validation
of this branch until the snapshot corpus has been regenerated and reviewed for
the current compiler. The script can probe either CLI when an explicit
`-Compiler` path is supplied, which is useful for that migration work.

The benchmark runner can compare a fresh bootstrap compiler with the C++26
baseline. The archived host compiler is not a current benchmark authority:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\benchmarks\run.ps1 `
  -BootstrapVyxc $compiler -RuntimeDir $runtime -RuntimeLib vyx_runtime -SkipHost `
  -Iterations 3 -Warmups 1 -RuntimeWarmups 1 -RuntimeRuns 3 `
  -RequireSemanticMatch
```

`tests/.cache/`, `bootstrap_compiler/out/`, and `benchmarks/out/` are generated
artifacts. Keep logs needed for a result, but do not commit binaries, caches, or
benchmark CSV output.

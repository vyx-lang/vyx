# DCI shared exception propagation — AOT

Run after building the tested SDK compiler from current source:

```powershell
./probes/gates/dci-exceptions/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-exceptions/run-linux.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

The Linux gate uses the Windows SDK compiler from this build to emit Linux IR,
repository LLVM 22 to emit ELF objects, and Ubuntu WSL g++ to link/run the native
C++ harness. Its small
runtime fixture provides argument initialization and native class arena storage.
Both scripts generate canonical contracts from tracked fixture facts and use
the freshly built SDK compiler to compile the tested Vyx programs. The Release
SDK compiler serves only as the Stage 0 build seed. JIT is deferred.

## Actual execution, 2026-10-01

| Target / ABI | O0 | O2 |
|---|---|---|
| x86_64 Windows / `dci.eh.msvc-cxx.v1` | Pass | Pass |
| x86_64 Linux / `dci.eh.itanium-cxx.v1` | Pass | Pass |

Each execution runs modes 0 through 10 and verifies exception identity at a native C++ catcher, native
C++ stack cleanup, exact Vyx destructor order and balanced DCI malloc/free.
Only allocator calls in emitted Vyx IR are intercepted, including LLVM's
malloc-to-calloc optimization. The native producer and runtime keep their own
allocation domains. Normal return uses existing Vyx declaration order; unwind
uses reverse construction order.

Coverage: nested Vyx frames/scopes, early return, constructor failure (free
without destructor), reassignment, loop continue/break, native Vyx static
factory, aggregate literal and constructor. The separate inline fixture checks
that a DCI record lives directly in a stack alloca, receives one destroy during
unwind, and performs no DCI allocation/release.
An exception from ordinary scope-exit destruction releases that object's
storage, skips a second destructor call, and destroys the remaining live objects.

Both targets also execute a throwing destructor in a subprocess during unwind.
The fixture must print `SECOND_EXCEPTION_THROWN` and abort (Windows fast-fail
`0xC0000409`, Linux SIGABRT/134). Catching the second exception instead returns
77 and fails the gate.

Before the fix, both native harnesses caught the exception but reported
`mode=1 allocated=3 released=0 destructors=0` (exit 11). After the fix each
completed object is destroyed once and each DCI allocation is released once.
Construction failure counts one fewer destructor than allocations, as required.

## Historical compiler gates at the exception repair

The hashes below identify that repair's build stages, not the newest repository
SDK compiler. Rebuild and rerun Gate A/B for subsequent compiler changes.

- Hello MIR: `functions=1 blocks=1 locals=1 places=1 values=6 instrs=2 cases=0 types=5`.
- Hello IR SHA-256 unchanged:
  `7C98411BB094C939533A0942A785063B010D20D677585CE4FBDB7BCFA11F7BF0`.
- Final S1/S2/S3 SDK compiler SHA-256, identical:
  `CEF474A5C7EE6BB7DFF6329389E96E8C936F597855F61BAA4D5523B02282104E`.
- Required projects pass: `dci_cpp_trait`, `dci_rust_trait`, `dci_multilang`, `dci_zig_abi`.
- Additional gates pass: `dci-storage` (2056 allocations/destructors/releases
  at both optimization levels), `dci-failure`, `dci-inalloca`, `dci-landingpad`, `dci-active`.
- `DciConsumerGate` covers unknown ABI/version/target and nonparticipating caller
  rejection. Consumer lifecycle regressions now inspect emitted operations
  instead of requiring the removed `[dci-lifecycle]` diagnostic count logs;
  those former log assertions are not current known failures.
- Qt adapter validation and constructor binding failures at that repair are
  recorded in `../dci-storage/README.md`. The current
  [Qt counter gate](../dci-qt-counter/README.md) validates original C++ exception
  propagation, virtual callbacks and object cleanup with `shared_abi`.

Before semantic changes, SPLIT2 extracted call/invoke emission into
`llvm_unwind.vyx`. S2/S3 matched
`976546F60F9586ED2597F5777D8E641EAED3079FE9FAFAE2668E9275633F35C0`.
Shared O0 IR matched before/after extraction:
`704B9C21DB97B4AE67CCA9CD5E89EC5640BE76A5F0783F461A1089F75B3D2FF1`;
complex ABI O0 IR matched
`11AE9A668777D554A7AA35BE7DC6278CE2882DDAD8218FF1881F37368390119E`.
The lowerer retains only wiring to the extracted exception/lifetime emitters.

The backend emits a per-frame cleanup stack of entry allocas and private LLVM
helpers. MIR Drop targets determine operations; no name matching or process
registry is used. Raw DCI allocation registers a free-only action, successful
construction activates destruction, and ordinary Drop/release removes that
action. Reused loop sites must have released the previous object. Cleanup
operations require a complete-object `void(pointer)` ABI; unsupported cleanup
signatures fail closed. This does not synthesize Vyx catch/throw syntax, convert
Rust/Zig failure models into C++ EH, or enable previously rejected DCI moves.
The real [vector](../dci-vector/README.md) gate also propagates original
`std::out_of_range` while cleaning both active vector objects. C++ ICU and
Rust Cargo have separate [ecosystem](../dci-industrial/README.md) acceptance.
The tests establish these target profiles and cases, not arbitrary unwind or
industrial readiness.

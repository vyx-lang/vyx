# Compiler source recheck

Run after building the compiler once. This uses the **new compiler**, its matching
backend/runtime, the current `std_packages`, and the actual object plans and
interfaces produced by that build:

```sh
python probes/gates/compiler-self-source/run.py --compiler bootstrap_compiler/out/boot --build-root bootstrap_compiler --output .cache/compiler-self-source
```

On Windows, use `out/boot.exe`. Output must be a new directory. All declared Vyx
sources in the selected manifest target must be accounted for by completed object
plans in `.cache/.vyx_cache`, with the published object present; an absent module
is a failure. Optional `.vyx-cost` memory statistics and pending task files are
not evidence of a completed build. Cached builds use the same committed records.
Before checking bodies, the new compiler re-emits the compiler modules' shallow
interfaces into a separate directory using the recorded interface commands.
The checks use those fresh interfaces and the current standard-library packages.
Stage 0 interfaces can leak private declarations that the new compiler no longer
publishes; reusing them would miss undefined names in a subsequent build.
Each module is checked through the streaming
MIR path, one process at a time. At least one lowered body is required for every
module containing function definitions. Declaration-only host-service modules
are identified from their inputs and recorded separately. Commands,
source/compiler hashes, diagnostics and counts go to
`verification.json` and per-module logs.

The gate first checks that named and top-level export roots both reject a return
of local String storage with `E3102`. Previously, MIR verify could report success
without lowering any exported function body when no process entry was selected.

This is a source/semantic recheck, not a second native compiler generation or a
fixed-point test. AOT linkage, execution, DCI and platform packaging remain
separate release checks. The native release scripts run it before building tools
or packaging an SDK, so Android cannot consume a Linux SDK whose compiler rejects
its own modules.

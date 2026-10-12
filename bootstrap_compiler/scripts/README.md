# Bootstrap scripts

[Documentation](../../docs/README.md) · [Compiler bootstrap](../README.md) · [Verification](../../docs/TESTING_GUIDE.md)

This directory contains current bootstrap tooling and retained diagnostic scripts. It is deliberately
separate from `tests/checks/`: scripts here either build the self-host compiler,
exercise a bootstrap stage, or provide a reusable process/IR helper.

`test_project_selfhost_fixpoint.ps1` defaults to `../seed/vyxc.exe` and the
repository LLVM SDK at `../../clang`; pass `-SeedCompiler` and `-LlvmRoot`
explicitly to select another compatible toolchain. It does not itself read
`VYX_BOOTSTRAP_VYXC`, so pass that variable's value as the argument when using the
verification guide. The bash `selfhost_3gen_ir.sh` uses argv 1 or `VYX_SEED` for
the seed and `LLVM_ROOT` for LLVM. Record the selected compiler/backend/runtime
hashes and do not mix stages.
The archived July seed is not the current bootstrap input.
Use `-Jobs 20` to cap each project generation's parallel build. The fixed-point
comparison excludes `.vyx-provenance` and `.vyx-cost` metadata: they contain
stage paths, compiler mtimes and measured memory, and remain available in the
output directories. Compiler binaries, interfaces and deterministic cache
objects are still compared. `-HelloGate` copies the canonical standalone hello
into an isolated directory to keep unrelated probes out of its import registry.
The [Linux compiler object exporter](emit_linux_boot_crate.ps1) requires the
SDK compiler outputs freshly built from this checkout and matching object records.

## Build and process infrastructure

| Script | Role |
| --- | --- |
| `build.ps1`, `build.sh` | Build the manifest target with an explicit Stage 0 (`-Compiler` / `VYX_BOOTSTRAP_VYXC`, otherwise `vyxc` on PATH); record its version and hash. |
| `VyxTestProcess.ps1` | Shared process timeout/crash-dialog helper. |
| `build_probes.ps1` | Build the lexer/parser/sema/emit probe executables. |
| `export_partitioned_ir.ps1` | Export partitioned MIR/LLVM IR for a project. |
| [Linux compiler object exporter](emit_linux_boot_crate.ps1) | Cross-emit the SDK compiler crate as a checked ELF object from the current build's object records. |

After building the current SDK compiler target, run the exporter to produce
a checked ELF object under `out/linux/`. It derives the source unit from that build's
object records and rejects missing `bootstrap.*` dependencies before codegen.
For a multi-file logical module, the exporter reads every `group_src` member
from the object's recorded source stamp and requires the complete module group.
Both ordinary and dependency-partitioned record formats are accepted.
The HIR, MIR and LLVM implementation files therefore remain in the exported crate.
`-DryRun` writes and validates the source list without emitting an object.
A failed emit removes the canonical object from the link path; the previous
object is retained with a `.stale.bin` suffix for inspection. The exporter reports
the generated paths.

## Termux packaging

`package_termux_sdk.py` updates a prepared Termux `.deb` using a freshly built
Android compiler and matching backend/runtime. The base package supplies the
private LLVM payload and SDK assets, not compiler source or a bootstrap input.
Run on Linux/WSL with Python 3.11+, `dpkg-deb` and `patchelf`:

```sh
python3 bootstrap_compiler/scripts/package_termux_sdk.py \
  --base-package dist/vyx-sdk-termux-aarch64.deb \
  --compiler-out /path/to/current/bootstrap_compiler/out \
  --source-root /path/to/current/source/snapshot \
  --destination dist/vyx-sdk-termux-aarch64.deb \
  --work-dir .cache/termux-package \
  --version '1.0.5~android21.20261009'
```

The compiler output must include `boot` and its matching
`runtimes/android/arm64/ndk` build artifacts. That cross-build staging directory
does not become an installed NDK requirement. The installed package keeps one
backend/runtime copy under `lib/vyx-sdk/lib`.

Pass `--source-root` when rebuilding the complete SDK. It replaces the base
package's standard library, DCI tools and license files with the selected
source snapshot and verifies their hashes after package extraction. Without
this option, those assets remain from the base package. Newly built libraries
are measured again; saved private namespace metadata applies only to retained
dependency libraries.
Retained private libraries with matching metadata are left byte-for-byte
unchanged; the full dependency and version checks still run on every ELF.

The launcher does not add SDK libraries to `LD_LIBRARY_PATH`. Compiler and
LLVM tools use private SONAMEs under `toolchain/lib/llvm-private`, resolved by
ELF `RUNPATH`. Ordinary runtime libraries retain their application linker
names. The large LLVM image has one physical copy and an ordinary linker alias.
System clang, linkers and Python inherit the caller's environment.
`--prefix` must match the base payload and launcher installation prefix.
Record both original and patched ELF hashes: packaging changes loader metadata.
Unresolved or ambiguous library dependencies abort packaging before publication.

Only LLVM tools are retained from the toolchain payload. Incidental utilities
such as `zstd`, `clear` and `xml2-config` remain supplied by Termux packages.
`termux_elf.py` checks dynamic metadata, direct dependency SONAMEs and required
symbol versions. Android's
[VersionTracker::init_verneed](https://android.googlesource.com/platform/bionic/+/refs/heads/main/linker/linker.cpp)
finds a version provider by its SONAME. Updating only `DT_NEEDED` and `vn_file`
to an absolute path violates that association. Private dependencies therefore
use the same name in `DT_NEEDED`, `DT_SONAME` and `verneed`; version names,
hashes and indices are preserved. Missing private version definitions reject
the package. Android system-library version definitions require a device test.

The [ELF environment gate](../../probes/gates/termux-sdk/README.md) reproduces
the system-clang/private-LLVM collision. Package verification checks the
AArch64 library closure, SONAME/version associations and roundtrip hashes;
it does not replace a Termux
device build.

## Bootstrap gates

| Script | Role |
| --- | --- |
| `test_selfhost_fixpoint.ps1` | Retained single-file A/B/C compiler and IR diagnostic gate; the project gate below is the current bootstrap entry. |
| `test_project_selfhost_fixpoint.ps1` | Cold S1..SN project self-host gate (default 3 gens; `-Generations 5` for five, max 12). `-Target $compilerTarget` matches AGENTS.md 门 B (avoids lsp/dap). S1 may differ from seed; S2 through SN must match. Compiler binaries are the stability evidence. clang COFF `TimeDateStamp` on runtime C objects (`vyx_runtime.lib`, `*_c_c.obj`) is excluded from the byte compare. `-HelloGate` rechecks `probes/gates/hello_gate.vyx` `values=6 instrs=2` with the last compiler. `-InstallSeed seed_94` copies the last generation's compiler + backend into `bootstrap_compiler/seed_94/`. `-CompareOnly -WorkRoot <retained-run>` rechecks without rebuilding. |
| `selfhost_3gen_ir.sh` | Linux bash S1/S2/S3 self-host plus same-input `--emit=ir` identity. Stage 0 defaults to the checked-in seed. |
| `test_mir2cpp_multigen.ps1` | Experimental multi-generation MIR2CPP diagnostic; not a native release/fixed-point gate. |
| `test_full_pipeline.ps1`, `test_pipeline.ps1` | Probe pipeline parity checks. |
| `test_parse_sema_conformance.ps1` | Parser/sema probe conformance. |
| `test_diag_smoke.ps1` | Bootstrap diagnostic smoke matrix. |

## Focused/manual regression gates

Current acceptance uses native AOT. `probes/gates/match-expression`,
`cross-module`, `generic_interfaces` (including `extended.ps1` and
`long-chains.ps1`), and `compiler-industrial` have separate entry points. See
the [AOT Gate A/B/C guide](../../docs/TESTING_GUIDE.md#aot-compiler-regression-gates)
for exact commands and platform/scale boundaries. Neither the module sweep nor
the contract dispatcher automatically includes all of these probes. JIT parity
is deferred.

These are intentionally retained because they cover contracts that are not part
of the recursive module runner: cancellation, receiver modes, reflection
partitioning, and process/job behavior.

`test_process_cancel.ps1`, `test_receiver_modes_smoke.ps1`,
`test_reflection_partition_smoke.ps1`, and
`test_vyx_test_process_memory_job.ps1` are manual gates, not temporary scratch
files.  Their generated projects belong under `out/` or the system temporary
directory and must never be checked in.

The canonical list of root-language and backend checks is
`tests/checks/run_checks.ps1`; use that dispatcher before adding another
one-off script here.  A script may be removed only after its gate is either
registered there or replaced by a documented build-system check.

Repository output cleanup is handled by `scripts/clean_outputs.ps1` (dry run
by default).  Use `-IncludeBenchmarks`/`-IncludeExamples` to opt into those
trees, and pass `-Purge -Confirm:$false` only when the listed paths are no
longer needed.

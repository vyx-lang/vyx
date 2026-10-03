# Public generic interface regressions

[Compiler architecture](../../../docs/COMPILER.md) · [Verification](../../../docs/TESTING_GUIDE.md)

These gates use the SDK compiler freshly built from current source for AOT
and its matching backend/runtime.
They compile consumers from emitted `.vyi` artifacts. User producer source is
not supplied as an import fallback.

## Run

From the repository root on Windows with the repository LLVM 22 and native
linking environment:

```powershell
$compiler = (Resolve-Path ./bootstrap_compiler/out/vyxc.exe).Path
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/extended.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/long-chains.ps1 -Compiler $compiler
```

| Gate | Contract |
|---|---|
| `run.ps1` | Direct-source control and interface-only `Cell<T>` both execute; a private helper cannot be called from the consumer (`E3000`); altered surface/payload/version/module, truncation, and duplicate end markers fail import |
| `extended.ps1` | Nested free generics and private alias/constant/helper dependencies; same short function name in two defining modules; `Dict<string, i64>`; two independent consumers instantiate the same generic and link together |
| `long-chains.ps1` | 620 generated local declarations plus a closure capturing two values survive interface serialization and consumer execution; a nongeneric extern/record surface does not receive a template payload |

The `Dict` case uses the actual `std.collections.vyi` emitted during the compiler
build and explicitly supplies `bootstrap_compiler/std/dict.vyx` via
`--interface-unit-sources`. Std interfaces currently omit the template payload.
This case therefore verifies the supported std source-unit path, not source-free
distribution of all std generic implementations.

The duplicate-consumer case links real COFF objects with repository clang/lld.
It catches duplicate strong definitions while preserving canonical instance
names for cross-CGU calls. COMDAT emission also exists for ELF/Wasm in the
backend; this Windows gate is not platform evidence for those formats.

## Artifact contract and limits

For user modules containing generics, `.vyi` consists of the public declaration
surface followed by three comment lines: `VYX_TEMPLATE_ARTIFACT_BEGIN`, hex
`VYX_TEMPLATE_ARTIFACT_PAYLOAD`, and `VYX_TEMPLATE_ARTIFACT_END`. Version 1 uses
`format=ast-graph`. It carries generic bodies and private dependencies;
semantic pointers are rebuilt on import, and lookup retains defining-module
scope and visibility.

The decoder checks marker order and duplicates, header fields, surface and
payload SHA-256, decoded node count, module identity, field bounds, and trailing
data. Once artifact markers are present, invalid data is rejected rather than
silently parsed as bodyless declarations. Ordinary interfaces without markers
remain valid for modules that do not use this payload.

The header records format version, producer label, module, `source`, options
label, surface/payload digests, and node count. In v1, `source` is the output
interface path; the producer and options labels are emitted as
`vyxc/1.0.0-alpha.1/aot` and `aot;template-artifact=1`. They are not a compiler
binary hash or a complete target/optimization fingerprint. Record the actual
compiler/toolchain identity separately when publishing evidence.

Current source limits are 64 MiB of graph bytes, 200,000 nodes, 16 MiB per field,
and structural depth 512. Statement/declaration `next` chains are processed
iteratively; a flat list longer than 512 is not structural nesting. Runtime,
`bootstrap`, `bootstrap.*`, and `std.*` modules omit this artifact. Marker-only
parser metadata is not sufficient to classify a declaration as generic.

Changes to the public surface or payload change the artifact's hashes. This
protects artifact consistency; it does not by itself implement shared frontend
facts, complete dependency closure, or a whole-build cache identity model.

## Recorded result — 2026-10-01

The final Windows AOT follow-up (`61d31e51`, after `ef86b928`) passed all three
gates. Extended cases: `free-generic`, `same-short-name`, `std-dict-string-i64`,
and `duplicate-consumer-link`. The long-list case recorded 2,532 graph nodes,
consumer exit 0, and no payload for the nongeneric surface. Compiler SHA-256:

```text
13BBC891263F818FD25E9D10AC4CA92668EA863115B5A2DD98B876E4D66F06EF
```

`result.json` and compiler/link/run logs are retained in generated `.runs/`
directories. These results cover the listed contracts, not every generic form,
cross-platform link mode, or JIT execution.

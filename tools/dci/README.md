# DCI tools

[简体中文](README.zh-CN.md) · [Documentation](../../docs/README.md) · [DCI reference](../../docs/DCI_SPEC.md) · [Fact Semantic Ownership System](../../docs/MOSP.md)

This directory contains producer Adapters, contract validation and encoding,
Active Adapter sessions, and Stub backends. The unified command is
`python tools/dci/dci.py`; `vyxc dci` discovers the same tools beside the SDK
or through `VYX_DCI_TOOLS`.

DCI follows the Fact Semantic Ownership System model: the producer owns source-language semantics; the
Consumer uses verified facts for a particular operation and target. Contracts
carry closed ABI facts. Active / Online Adapters can request new facts and
implementation artifacts during a build.

## Unified SDK architecture

SDK 1.1 organizes built-in and third-party implementations through the same
parameter model and registration API. The contract format remains independently versioned at 1.0.

```text
CLI / vyxc dci
    → declarative parameters → AdapterRequest
    → AdapterPlugin → producer facts
    → validation / encoding / Stub / artifact delivery
```

| Component | Role |
|---|---|
| `sdk_parameters.py` | Shared parameter concepts and canonical spellings |
| `OptionSpec` / `param()` | Declare shape, aliases, supported languages, and help |
| `AdapterRequest` | Shared input to `build_argv(request)`, `run(request)`, and `doctor(request)` |
| `AdapterPlugin` | Register a producer, parameters, and input rules |
| `CommandPlugin` | Register CLI commands |
| `StubBackendPlugin` | Register Stub compilation strategies |

Declarations merge by `dest`: one concept becomes one parameter. `--compiler`
is canonical; `--cxx`, `--rustc`, and `--zig` are language-specific aliases.
Option ownership also generates cross-language misuse diagnostics. New languages
contribute declarations rather than requiring new core CLI branches.

Discovery loads built-ins, local `plugins/*.py`, and installed-package entry points:
`vyx_dci.adapters`, `vyx_dci.commands`, and `vyx_dci.backends`.

```sh
python tools/dci/dci.py plugins --json
```

This reports SDK/contract versions, registered implementations, and the merged
parameter table with spellings, shapes, and language ownership.
See [EXTENDING.md (Chinese)](EXTENDING.md) for installation and extension examples.

## Requirements and entry points

Use Python 3.10 or later and the producer toolchain for the selected language.
C++ extraction uses Clang as a structured helper and the selected Clang, GCC,
or MSVC compiler as the ABI authority. Rust uses the selected rustc; Zig uses Zig.

Run commands below from the repository root:

```sh
python tools/dci/dci.py --help
python tools/dci/dci.py doctor --language cpp --toolchain clang
python tools/dci/dci.py doctor --language rust
python tools/dci/dci.py plugins
```

Equivalent entry points are `vyxc dci ...`, the Windows `tools\dci\dci.cmd`,
and the installed `vyx-dci` console command. Installation and plugin APIs are
documented in [the SDK extension guide (Chinese)](EXTENDING.md).

## Generate and inspect a contract

The following example uses the repository's C++ open-generics provider:

```sh
python tools/dci/dci.py adapter --language cpp tests/projects/dci_opengeneric/native/lib.hpp --toolchain clang -o contracts/native.dcib --debug-json
python tools/dci/dci.py validate --strict contracts/native.dcib
python tools/dci/dci.py inspect contracts/native.dcib
python tools/dci/dci.py decode contracts/native.dcib contracts/native.dci.json
python tools/dci/dci.py encode contracts/native.dci.json contracts/native.dcib
```

A contract describes an implementation; generating it does not replace compiling
and linking that implementation. Open-generic calls additionally require the
Active Adapter configuration described below.

`.dcib` is the canonical binary input to Vyx builds. JSON is for inspection,
validation, and explicit conversion. Integers passing through the current cJSON
model must remain in the exact range `±2^53`.

### One parameter set

`dci adapter` has one parameter per concept. The canonical spelling and the
per-language alias select the same value within that language. Use canonical
names for shared concepts; language-specific aliases and options require their
owning language:

| Canonical | Per-language aliases | Meaning |
|---|---|---|
| `--compiler` | `--cxx` (cpp) · `--rustc` (rust) · `--zig` (zig) | producer compiler to run |
| `--compiler-arg` | `--cxx-arg`/`--frontend-arg` (cpp) · `--rustc-arg` (rust) · `--zig-arg` (zig) | one extra argument for it; repeatable |
| `--extractor` | `--clang` (cpp) | structured fact extractor (clang++/clang-cl) |
| `--extractor-arg` | `--clang-arg` (cpp) | one extra argument for the extractor; repeatable |
| `--namespace` | `--crate-name` (rust, zig) | DCI namespace recorded in the contract |
| `-o`/`--output`, `--triplet`/`--target`, `--debug-json`, `--stub-out`, `--artifact` | — | accepted by every language |

Each language also declares what only it understands: C++ adds `--toolchain`,
`--std`, `-j`, `--include`, `-I`, `--project-root`, `--scan-public-root`,
`--cmake-build-path`, `--compile-flags`; Rust adds `--edition`,
`--deny-rejected`, `--export-active-requests`, `--export-instance`; Zig adds
`--deny-rejected`. Using a parameter that belongs to another language is an
error that names its owner instead of being silently ignored:

```console
$ dci adapter --language rust lib.rs --extractor clang++
dci: error: adapter option(s) belong to another language adapter:
--extractor (--language cpp); this run is --language rust
```

`dci plugins --json` prints the merged table (`parameters`), which is exactly
what a plugin declares — see [EXTENDING.md](EXTENDING.md).

### C++ configuration

| Option | Purpose |
|---|---|
| `--toolchain clang\|gcc\|msvc` | Select the compiler that determines ABI facts |
| `--compiler` (`--cxx`) | Select its executable |
| `--extractor` (`--clang`) | Select the structured extraction helper |
| `--triplet` | Select the target; `--target` is a compatibility alias |
| `-I`, `--std` | Include paths and language standard |
| `--extractor-arg`, `--compiler-arg` | Extra helper or selected-compiler arguments |
| `--cmake-build-path` | Read build flags from `compile_commands.json` |
| `--compile-flags` | Read a flag file or append an inline flag |

Example for a Linux GCC producer, replacing the header and build paths with
those of the library being adapted:

```sh
python tools/dci/dci.py adapter --language cpp include/Api.hpp --toolchain gcc --compiler g++ --triplet linux_x64 --cmake-build-path cmake-build-release -o contracts/Api.dcib
```

Use `--compile-flags=-DFEATURE_ON=1` for an inline flag that begins with `-`.
The underscore forms `--cmake_build_path` and `--compile_flags` are also accepted.

Layout, symbol identity, and aggregate lowering must describe the selected
producer and artifact. Compiler version strings record provenance; they are
not a requirement that the consuming compiler have the same version.

### Rust configuration

```sh
python tools/dci/dci.py adapter --language rust tests/projects/dci_opengeneric/native/lib.rs --namespace native_api --compiler rustc -o contracts/native_rust.dcib --debug-json
```

Use `--triplet` for the target, `--edition` for the edition, repeated
`--compiler-arg` (`--rustc-arg`) for compiler arguments, and `--artifact` for
represented native objects or libraries. `--deny-rejected` makes rejected
exports fail generation, and `--export-instance TYPE<ARGS>` names a closed
generic instance the host references (inside a Vyx build the instances are
discovered from the consumer instead; see "Open generics and Active Adapters").

For a real Cargo package, use the standalone Rust adapter's `--manifest-path`
and select the package with `--package`. These Cargo-specific flags are not yet
declared by the unified SDK CLI's Rust parameter registry. For example:

```sh
python tools/dci/dci_adapter_rust.py --manifest-path probes/gates/dci-rust-ecosystem/native/Cargo.toml --package adler2 --item adler32_slice --locked --offline --target x86_64-pc-windows-msvc --emit-views --native-lib-out contracts/adler2.lib --artifact contracts/adler2.lib -o contracts/adler2.dcib
```

`--features`, `--no-default-features`,
`--locked`, and `--offline` are replayed through Cargo; the adapter records the
resolved dependency graph, build-script environment, and native artifacts
instead of reconstructing `--extern` paths. `--item` selects public producer
paths, `--opaque-type` requests a rustc-measured closed record plus its explicit
drop operation, and `--emit-views` measures named consumer views for Rust fat
references. `--native-lib-out` emits a native static library with linkable
bridges for supported ordinary public functions. The locked Cargo fixture in
[`probes/gates/dci-rust-ecosystem`](../../probes/gates/dci-rust-ecosystem/)
exercises this path against `crc32fast` and `adler2`.
The bridges are generated implementation artifacts; no handwritten C wrapper is
required by the fixture. This is a measured subset, not an arbitrary-crate ABI
or a bridge-free Rust call guarantee.

The Adapter probes the selected rustc for layouts and ABI lowering. Its
supported paths include C/system boundaries and measured Rust-native
representations, references, fat pointers, and trait dispatch. These facts
belong to that producer environment; they do not establish a stable ABI across
rustc versions. Regenerate the contract and matching artifacts when the producer
environment changes.

Unclosed generic definitions are not concrete ABI exports. Data-carrying enums,
missing lifecycle evidence for `Drop`, and signatures the Adapter cannot verify
remain subject to rejection. Open-generic requests use the separate build-time
path below.

## Open generics and Active Adapters

An Active / Online Adapter is a fact-production mode of the Adapter.
“Online” means callable during a build, not a network service.

```text
Vyx call site → closed request → producer constraint checking / instantiation
             → verified facts + implementation artifact → native link
```

The producer performs overload selection, trait/concept checking, template
instantiation, or monomorphization. Vyx does not reproduce those language rules.
Representation-compatible types can be admitted for supported operations;
the path does not perform a universal object-layout conversion.

### Function requests and record layouts

The current [open-generics project](../../tests/projects/dci_opengeneric/README.md)
distinguishes two requirements:

- Generic function calls produce closed `.dci_open` requests for the external backend.
- Generic records need size, alignment, and field offsets before Vyx emits code.
  Their required instance layouts must already be present in the contract.

Instance demand is consumer-driven: lowering records every generic instance it
materializes (including one appearing only as the return type of a called method)
as an `instance <type-text>` line in the `.dci_open` sidecar, and
[`dci_close_instances.py`](dci_close_instances.py) asks the producer adapter to
close exactly those instances into a supplementary `.dcib`; the build re-lowers
against it after failed lowering, with at most three instance-closure rounds
per compile job. This retry path is not a complete global semantic/artifact
fixed point. No hand-written `--export-instance`
list is needed inside a Vyx build — the flag remains the explicit request path
(e.g. for a producer used outside Vyx, or as the closer's own retry when the
producer rejects an instance as unmeasured). Because the same instance may be
spelled the consumer's way, the producer's way, or with C++ primitive words, the
closer canonicalizes names (whitespace, module prefixes, `unsigned int` -> `u32`)
before matching. A multi-root request arrives as one newline-joined
`--descriptor` value; each root is decoded separately.

The [original vector gate](../../probes/gates/dci-vector/README.md) starts with
only `<vector>` and an open Vyx `std.vector<T>` declaration. The consumer writes
`var a = std.vector<i32>{1,2,3,4,5};`; it does not import a pre-exported concrete
vector API. `@[dci_list_init(true)]` asks the producer to check C++ brace
construction, including narrowing rejection. The generated construction entry
and original member symbols remain explicit parts of the boundary.

The SDK 1.1 parameter registry exposes Rust instance-layout and Active request
options through the unified CLI when `--language rust` is selected:

```sh
python tools/dci/dci.py adapter --language rust tests/projects/dci_opengeneric/native/lib.rs --namespace open_generic --export-instance "Pair2<i32,f64>" --export-active-requests -o contracts/open_generic.dcib
```

When a project uses producer-to-consumer spelling mappings or additional instance
layout declarations, merge and then verify them with the producer compiler:

```sh
python tools/dci/merge_instance_layouts.py contracts/open_generic.dcib tests/projects/dci_opengeneric/dci/instance_layouts.json
python tools/dci/verify_instance_layouts.py contracts/open_generic.dcib tests/projects/dci_opengeneric/dci/instance_layouts.json --lang rust --provider tests/projects/dci_opengeneric/native/lib.rs
```

The merger modifies the contract in place. Merging declarations is not evidence
that the producer uses those layouts; the verification step supplies that check.
For the project's complete instance list and generation commands, use
[run_vyx.sh](../../tests/projects/dci_opengeneric/run_vyx.sh).

### Build configuration

The project selects `dci_stub_backend = "external"` and provides:

| Manifest key | Meaning |
|---|---|
| `dci_stub_backend_tool` | Backend executable, such as `python` |
| `dci_stub_backend_tool_args` | Backend script and `--provider` source |
| `dci_stub_backend_source_extension` | Generated source suffix, such as `rs` or `cpp` |
| `dci_stub_backend_dependencies` | Scripts, provider sources, and configuration affecting the output |
| `dci_stub_backend_version` | Backend version/configuration cache salt |
| `dci_stub_backend_capabilities` | Capabilities provided, including `emit-source` and `compile-object` |

See the complete [Vyx.toml](../../tests/projects/dci_opengeneric/Vyx.toml).
From that project directory, build the Rust or C++ target:

```sh
vyxc build --target dci_opengeneric
vyxc build --target dci_opengeneric_cpp
```

The targets use different producer contracts. Their call sites can share a shape;
their contract identities and artifacts remain separate.

### Protocol and artifact APIs

| Module | Responsibility |
|---|---|
| `active_protocol.py` | Queries, resolutions, environment identity, entity/request/bundle keys |
| `active_cpp.py`, `active_rust.py` | Producer sessions and semantic checks |
| `artifact_bundle.py` | Content validation, deterministic manifests, atomic publication |
| `stub_backend.py` | External `emit` / `compile` command protocol |

The Active Adapter modules are Python APIs, not additional CLI subcommands.
Bundles bind facts to implementation artifacts and dependencies. A failed
materialization must not publish partial output.

## Lifecycle, failure, and Stub behavior

- `dci-ownership` annotations state ownership facts that cannot be inferred.
- Rust `dci-lifecycle` annotations bind copy/move/destroy operations to symbols.
  Nontrivial values require lifecycle evidence in addition to layout compatibility.
- `profile.lifecycle_binding = automatic` enables the supported Vyx local
  copy/move/cleanup transformations. It does not provide full control-flow ownership analysis.
- Owning pointer-represented DCI locals release their module-allocated storage
  after destruction. Inline records receive destruction without heap release;
  borrowed parameters retain the producer's ownership.
- `no_unwind` is the default boundary. `translated` uses a producer-compiled
  translator. AOT `shared_abi` implements actual C++ propagation on x86_64
  Windows MSVC (`dci.eh.msvc-cxx.v1`) and x86_64 Linux
  (`dci.eh.itanium-cxx.v1`). Participating frames clean completed scoped objects
  in reverse construction order; failed DCI construction releases storage
  without calling its destructor. The native exception identity is preserved.
  A destructor throwing during unwind terminates. Unknown ABI/version/target,
  nonparticipating callers and unsupported cleanup signatures fail closed.
  This does not synthesize general catch/throw. Rust panic, Zig error and
  `longjmp` require their own supported boundary policy. JIT acceptance is deferred.
- `exports.stub_requests` describes required bridges. Supported
  `forward_direct` and `reverse_override` requests have Consumer synthesis paths;
  other operations require a backend with the corresponding capability.
- Contracts that cross an unwind boundary may opt into the obligation graph
  with `profile.obligation_mode = "required"` and a root `obligations` object
  (`version`, `lifecycle`, `exceptions`, and `edges`). Lifecycle entries bind
  required cleanup symbols; exception entries bind the propagation ABI and
  cleanup policy; edges connect facts emitted by different producer modules.
  `dci validate` and `dci_validate.py` merge all input documents and reject
  missing nodes, conflicting declarations, dependency cycles, or a
  `shared_abi` exception without a `cleanup-before-propagate` edge.

See the [current implementation reference](../../docs/DCI_SPEC.md) for boundaries
and source/test references.

## Tools and validation

| Tool | Purpose |
|---|---|
| `dci.py` | Adapter dispatch, validation, inspection, codec, diagnostics |
| `dci_adapter_cpp.py` | C++ entry point; implementation in `dci_adapter_msvc.py` |
| `dci_adapter_rust.py`, `dci_adapter_zig.py` | Rust and Zig producers |
| `dci_validate.py` | Schema and semantic contract checks |
| `dci_obligations.py` | Cross-module lifecycle/exception fact graph and fail-closed checks |
| `../effect_manifest.py` | Effect manifest and `MOSP-DCI-EXPORT` provenance inspection |
| `dcib.py` | Canonical encoding/decoding |
| `merge_instance_layouts.py`, `verify_instance_layouts.py` | Instance spelling/layout merge and producer verification |
| `dci_close_instances.py` | Build-internal closer: turns `.dci_open` `instance` requests into producer-closed facts (supplementary `.dcib`) |
| `dci_plugin.py` | Command, Adapter, and backend registration |

CLI tools expose `--help`. Example protocol and codec checks:

`VYX_DCI_EXPORT_OUT` selects the SDK compiler's data-only export sidecar.  Before
using it to generate a bridge, run `python tools/effect_manifest.py
validate-dci-export <sidecar> --manifest <same-build-manifest>`;
`inspect-dci-export <sidecar> --json` reports the
sealed EffectPlan fingerprint and authorized declaration rows.  The sidecar
does not itself assert a C++/Rust/Zig ABI or layout.

```sh
python -m unittest tools.dci.tests.test_dcib tools.dci.tests.test_active_protocol tools.dci.tests.test_artifact_bundle tools.dci.tests.test_dci_adapter_parameters
```

Producer and Consumer tests additionally need their toolchains and compiler
artifacts. See [tests/](tests/), the
[open-generics project](../../tests/projects/dci_opengeneric/README.md), and
[DCI specification](../../docs/DCI_SPEC.md).

Build caches include contract content and backend dependencies. When a script
is passed through `dci_stub_backend_tool_args`, list it in
`dci_stub_backend_dependencies` as well; a tool name such as `python` does not
identify the script's contents.

## AOT ecosystem and pressure gates

Build the SDK compiler from the current source tree first and use
`bootstrap_compiler/out/vyxc.exe` as the tested Consumer with its matching
backend/runtime. The Release SDK compiler in PATH supplies Stage 0 for the
initial self-host build. From the repository root on Windows:

```powershell
./probes/gates/dci-vector/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-cpp-ecosystem/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-rust-ecosystem/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-exceptions/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-industrial/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe -Repeat 2 -Scale 2 -Parallel 3
```

The [industrial runner](../../probes/gates/dci-industrial/README.md) records
status, child wall time, logs and sampled process-tree RSS. Missing RSS is
recorded as unavailable, and exit 77 is a skip. It composes real fixtures and
regressions; it does not certify industrial readiness. Linux shared propagation
has a separate [WSL gate](../../probes/gates/dci-exceptions/run-linux.ps1).
Required DCI project gates remain `dci_cpp_trait`, `dci_rust_trait`,
`dci_multilang` and `dci_zig_abi`. The old `dci_spdlog` project and `gate-c`
fixture were retired; do not use them as acceptance commands.

# DCI interoperability reference

[简体中文：核心规范](DCI_SPEC_ZH.md) · [Documentation](README.md) · [Tooling](../tools/dci/README.md) · [Fact Semantic Ownership System](MOSP.md)

- **Name:** Declarative Code Interface (DCI)
- **Core version:** 1.0
- **Implementation review:** 2026-10-01
- **Scope:** Native compiled-language interoperability; Vyx LLVM Consumer

This page summarizes the Core rules and current implementation.
[The Chinese Core text](DCI_SPEC_ZH.md) contains the full normative definitions,
capability levels, and conformance requirements.

## DCI within Fact Semantic Ownership System

Fact Semantic Ownership System treats verified program facts as explicit compilation objects with identity,
validity conditions, and provenance. DCI applies that model across native language
and compiler boundaries.

For an entity `e`, operation `o`, and target `T`, let `R(e,o,T)` be the
required facts and `K_C(e)` the Consumer's verified knowledge. Admission requires:

```text
R(e, o, T) ⊆ K_C(e)
```

The languages need not share source syntax, an object model, or a generic system.
The requested operation needs sufficient verified facts and matching implementation
artifacts. Layout equality alone does not prove call or lifecycle compatibility.

## Roles and boundaries

| Role | Responsibility |
|---|---|
| Producer | Own source-language semantics and generate native implementations |
| Adapter | Obtain or request producer facts, normalize them, and deliver contracts |
| Converter | Map contract entities to host declarations or binding models |
| Consumer | Validate contracts and emit supported native operations |
| Stub backend | Compile bridges for operations requiring producer-side code |

```text
Producer compiler → Adapter → closed .dcib facts + artifacts → Consumer
                       ↑
             optional build-time requests
```

Adapter fact production has three modes: offline extraction, native compiler emission,
and Active / Online production during a build. They use the same logical Adapter role.
“Online” does not require network access.

The Core Consumer must not parse foreign source semantics or select ABI algorithms
from `source.language`. Language-specific work belongs in the Adapter or a
capability-matched backend. Binding syntax such as Vyx's `extern "dci"` is not Core syntax.

## Unified SDK

The Python SDK 1.1 implements Adapter, command, and Stub backend registries.
`OptionSpec` / `param()` describe shared and language-specific parameters;
`AdapterRequest` is the common input to Adapter `build_argv`, `run`, and
`doctor` hooks. Parameters merge by semantic destination, with canonical names
such as `--compiler` and compatibility aliases such as `--rustc`.
The CLI validates which language owns each option.

Built-ins, local plugins, and package entry points use the same registration
interfaces. `dci plugins --json` reports registrations and the merged parameter
schema. SDK 1.1 and Core contract format 1.0 have independent versions.
See the [SDK guide](../tools/dci/README.md) for commands and extension entry points.

## Contracts and validity

Vyx project builds consume canonical binary `.dcib` files. JSON `.dci` /
`.abi.json` is the readable logical representation used by tools and validators.

A contract supplies the facts needed by its operations:

- target triple, pointer width, endianness, ABI/Profile identity, and provenance;
- concrete sizes, alignments, fields, bases, bitfields, and dispatch metadata;
- final `link_name`, calling convention, parameter/return ownership, and ABI lowering;
- lifecycle operations, allocator domains, and runtime type operations when required;
- failure/unwind behavior and required Stub capabilities;
- identity of the implementation artifacts to be linked.

Facts are valid for the recorded producer environment. The consuming compiler does
not need the producer's version number, but it must use compatible target facts and
matching artifacts. A changed producer toolchain, target, or layout-affecting option
requires revalidation and, where facts or artifacts change, regeneration.

Integers passing through the current cJSON model are exact only within `±2^53`.
The current LLVM Consumer supports a narrower target subset than the protocol,
including its 64-bit, little-endian requirement.

## Open generics and semantic closure

The canonical ABI contract carries **closed facts**, not unresolved trait obligations,
template semantics, or source-language overload problems. This does not prohibit
an Adapter from requesting new instances.

1. Vyx resolves its own call and supplies concrete argument identities.
2. An Active Adapter asks the producer to select the entity, check constraints,
   and instantiate or monomorphize it.
3. The producer returns the closed result; the Adapter records the required ABI facts
   and implementation identity.
4. The build validates the result and links the implementation artifact.

Overload resolution, specialization, concepts, and Rust trait checking remain
producer responsibilities. A rejected constraint or unavailable representation
must fail the request rather than trigger a guessed layout or implicit conversion.

Compatible primitives, imported native types, and supported host record
representations can be admitted for the operations a producer supports.
No universal Vyx-to-C++ or Vyx-to-Rust object conversion is required.

### Current build path

The [C++ / Rust open-generics project](../tests/projects/dci_opengeneric/README.md)
uses `stub_backend.py` with `dci_stub_backend = "external"`:

- Generic function calls generate closed `.dci_open` requests.
- The backend uses producer sessions to check and materialize those requests.
- Generic record instances require their size, alignment, and field offsets
  **before Vyx code generation**. Those layouts must already be in the contract.
- **Instance demand is consumer-driven.** Lowering records every generic instance
  it actually materializes as an `instance <type-text>` line in the `.dci_open`
  sidecar — including an instance that appears only as the return type of a
  method the consumer calls. The build-internal closer
  [dci_close_instances.py](../tools/dci/dci_close_instances.py) asks the producer
  adapter to close exactly those instances and writes a supplementary `.dcib`
  (the original contract is never rewritten); after a failed lowering the build
  retries with the supplement, for at most three closure rounds per compile job.
  This bounded instance retry is not a complete global semantic/artifact fixed
  point. No hand-written `--export-instance`
  list is required. The flag remains an explicit override for producers whose
  adapter needs flags the contract does not carry, and the closer also feeds back
  an instance the producer rejects as unmeasured.
- Instance spelling mappings and supplemental layouts are merged and then checked
  with the producer compiler. Because the same instance can be spelled the
  consumer's way (`native.SinkG<i32>`), the producer's way (`crate::SinkG::<i32>`),
  or with C++ primitive words (`SinkG<unsigned int>`), the closer canonicalizes
  names before matching: whitespace, module prefixes, and C++ primitive phrases
  (`unsigned int` -> `u32`) fold to one shape.
- A multi-root request reaches the backend as one newline-joined `--descriptor`
  value (main contract plus supplement); each root is decoded separately and
  provider checks apply to the first root.
- C++ and Rust targets use separate contracts and artifacts.

The Python control-plane APIs define environment, entity, request, and bundle
identity. Artifact bundles bind facts to content and dependencies and support atomic
publication. These APIs and the current sidecar-based build integration are distinct
implementation layers; their presence does not imply that every compiler query uses
a persistent session or every build artifact uses the bundle store.

## Standalone Vyx Converter

The SDK command `vyx-dci convert` (aliases `converter` / `convertor`) reads a
validated contract and emits an editable Vyx module:

```sh
vyx-dci convert contracts/Api.dcib --module native.api --header Api.hpp -o src/native_api.vyx
```

`extern "dci"` retains visible type and method declarations, including const
and virtual signatures for Vyx inheritance and `override`. Authored definitions
are checked by the Consumer against the original contract. An existing file
requires explicit `--force` to replace it. Build-time generation is optional.
The current automatic declaration surface covers global C++ classes and
representable signatures; unsupported host declarations produce diagnostics.

Ownership and lifecycle extraction, validation and generation belong to the
Adapter. Some facts follow from producer semantics and measured operations;
pointer retention, transfer and release require authoritative library semantics
or a protocol. The Converter must not invent those facts from pointer spelling.
Both kinds of fact enter the same contract. Producer layout, base offsets,
dispatch tables and native ABI symbols retain their measured identities.

Export scope is author-controlled. The Adapter must not derive it from Consumer
usage, inline definitions or library-name heuristics. Within the selected scope
it emits representable entities and reports rejected ones; base/layout closure
supplies validation dependencies. The optional build preparation requires exact
`export_types` / `export_functions`, or explicit `export_all = true`.
Unknown ownership must be declared once by the author in a `dci-ownership`
annotation or explicitly selected reusable `ownership_headers`. The tool must
not substitute a guessed borrow for a missing fact.

Project contracts are persistent inputs, conventionally in `contracts/`.
An existing offline Adapter contract is validated and consumed without
re-extraction or rewriting it. Deleting `.cache` only invalidates disposable
definitions, materialization and build stamps. Preparation MUST NOT treat a
missing cache stamp as evidence that the contract's ABI facts are invalid.
Invalid or incompatible supplied contracts produce diagnostics.

The optional `source.preparation_inputs` extension records version 1 producer
header hashes, target, C++ standard/flags, exception boundary and declared export
scope. Build-generated contracts use it to recognize known producer input
changes independently of the cache. Compiler version provenance is not a
consume-time equality requirement. Plain offline contracts may omit this
extension; library version freshness remains the provider's responsibility.
Explicit `cpp-import --force` can request a new measurement. Selected Qt
connection operations must already carry measured bridge facts to be
re-materialized from a supplied contract; missing operations are diagnosed.

`parameter.default` is a producer declaration fact, not an ABI parameter or an
additional overload. A `constant` contains a typed, range-checked scalar/null
value; a `producer_expression` records an unsupported/default-evaluation need.
Supported constant defaults become Vyx call-site defaults on the original
signature. Unresolved defaults require explicit arguments and diagnostics.
Neither path generates a default-argument wrapper. Native definitions include
original API headers; optional producer operations are selected separately.

## Direct and Stub

**Direct** emits the native ABI operation described by the contract without an
additional DCI bridge. Existing ABI costs, such as virtual dispatch, remain.

**Stub** generates producer-side code where a bridge is needed. It may handle
reverse overrides, producer object construction, or supported failure translation.
The selected backend must provide the requested capability.

The LLVM Consumer implements supported `forward_direct` and `reverse_override`
wrapper synthesis paths. This does not cover every wrapper shape.
`translate_unwind` uses a producer-compiled translator; the Consumer must not turn
a throwing function into a plain `nounwind` forwarder.

Vyx build backends include `direct`, `clang-cpp`, `rustc`, `auto`, and
`external`. The default is `direct`; a foreign declaration does not implicitly
select a Stub compiler. Multiple imported contracts retain their separate identities.

## Producer support

### C++

The compiler selected by `--toolchain` is the ABI authority.
Clang may assist structured extraction; final facts must describe the selected
producer and artifact. The Adapter supports selected Clang, GCC, and MSVC paths.

The extraction model includes measured nested records, enum representations,
and supported aggregate parameter/return lowering. Unsupported or incomplete
facts are rejected; support for one aggregate does not establish support for
every nontrivial C++ class.

The consumer can request the original open `std::vector<T>` template from a
header containing only `<vector>`. In the
[vector gate](../probes/gates/dci-vector/README.md), Vyx declares `std.vector<T>`,
constructs `var a = std.vector<i32>{1,2,3,4,5};`, and calls original vector
members. The producer closes `i32` and `f64` instances and checks brace
construction; a generated placement-construction entry supplies the initializer
list boundary. No vector specialization or replacement API is exported by the
fixture. Ordinary members and destruction use the standard library symbols.

The [C++ ecosystem gate](../probes/gates/dci-cpp-ecosystem/README.md) uses the
pinned ICU 78.3 headers, import library, DLL and Unicode data. It consumes the
original `UnicodeString` object and compares AOT results at O0/O2 with an
independent C++ reference. Its source alias exposes ICU's macro-defined namespace
to inventory; it does not replace the class or methods. Borrowed mutator returns
require explicit ownership facts.

### Rust

The selected rustc supplies measured representations and ABI lowering.
`repr(C)`, `repr(transparent)`, and C/system exports form one subset.
Supported Rust-native paths also cover default record layouts, Rust calls,
thin references, fat pointers, and in-crate trait dispatch.

These are facts about the selected rustc environment, not a stable ABI promise
between rustc versions. Unclosed definitions are not concrete exports;
Active Adapter requests provide a separate route to instantiate supported generics.
Data-carrying enums, missing `Drop` lifecycle contracts, and unverifiable
signatures remain subject to rejection.

The built-in `rustc` Stub backend compiles the identity producer crate separately
from the Stub static library. This preserves the producer's mangled identities.
See [dci_rust_trait](../tests/projects/dci_rust_trait/) and
[dci_multilang](../tests/projects/dci_multilang/).

The Cargo path replays the locked, offline dependency graph selected by Cargo,
including features, build-script environment and dependency artifacts. It can
measure explicit consumer views for Rust fat references and emit a native bridge
for ordinary public functions while retaining the producer's rustc identity.
The real-crate AOT gate uses `crc32fast` and `adler2` from their Cargo registry
packages and compares Vyx output with an independent Cargo oracle:
[dci-rust-ecosystem](../probes/gates/dci-rust-ecosystem/).
Cargo-specific options currently use the standalone `dci_adapter_rust.py`
entry point; they are not yet registered in the unified SDK CLI. The generated
bridge is an explicit boundary artifact, so this gate is not evidence of a
bridge-free Rust call or universal support for arbitrary crates, enums or traits.

A **generic** producer trait becomes inheritable only as a closed instance.
For `pub trait SinkG<T>`, the build discovers the instance from the consumer:
lowering emits an `instance` line, the closer asks the producer adapter for it,
and the result adds a layout (`native.SinkG<i32>` -> rustc path
`crate::SinkG::<i32>`), a vtable, and the `consume` symbol; the host then writes
`class VyxHostG : native.SinkG<i32>` and the emitted stub is
`impl native::SinkG::<i32> for __dci_vyx_stub_VyxHostG`. `--export-instance
'SinkG<i32>'` produces the same facts when passed explicitly, which remains the
supported way to request an instance outside a Vyx build. The open `SinkG` has
no vtable in the contract, so a base clause that names it without arguments is
rejected while emitting the Stub (with a source location) rather than at link
time.

### Zig

A Zig Adapter is registered in the CLI. Its support is defined by the exports it
can validate. The C++/Rust Active Adapter path does not imply equivalent
open-generic materialization support for Zig.

### Measured capability matrix (baseline 2026-10-01)

This records fixture coverage, not full Core conformance or an industrial
readiness claim. The newest AOT results cover Windows x86_64; the exception
gate also links and runs Linux x86_64 objects under WSL. Older rows identify
existing coverage and must be rerun for another toolchain or target.
**"unverified" means no verified end-to-end result is recorded for that shape;
it must not be advertised as support or read as impossibility.** "rejected"
means a negative test or measured limit rejects it with a diagnostic.

Consumer -> producer (Direct):

| Capability | C++ (clang / gcc / msvc) | Rust (rustc) | Zig |
|---|---|---|---|
| Free functions, scalars, pointers / references | yes ([dci_abi_stress](../tests/projects/dci_abi_stress/)) | yes ([dci_rust_generic](../tests/projects/dci_rust_generic/)) | yes ([dci_zig_abi](../tests/projects/dci_zig_abi/)) |
| Aggregates by value (direct / indirect / byval / sret) | yes ([dci_complex_abi](../tests/projects/dci_complex_abi/)) | yes ([dci_opengeneric](../tests/projects/dci_opengeneric/README.md), 16-byte sret) | yes (`Pair` by value) |
| Bitfields, enum representations | yes (dci_abi_stress) | yes (dci_rust_generic) | unverified |
| Class layout, inheritance, base adjustment, direct virtual calls | yes (dci_complex_abi multiple inheritance; dci_abi_stress polymorphic tiers) | yes (dci_rust_generic) | partial (records only) |
| Real third-party library | yes ([dci-cpp-ecosystem](../probes/gates/dci-cpp-ecosystem/README.md): pinned ICU 78.3 `UnicodeString`, original native symbols, O0/O2 reference parity). Qt/spdlog are older fixtures, not evidence for this current result | yes ([dci-rust-ecosystem](../probes/gates/dci-rust-ecosystem/README.md): locked Cargo `crc32fast` + `adler2`, measured `&[u8]` views, generated native bridge, O0/O2 oracle parity) | unverified |
| Original standard library template | yes ([dci-vector](../probes/gates/dci-vector/README.md): open `std::vector<T>`, consumer-selected i32/f64, producer-checked brace construction, growth and bounds-unwind stress) | unverified for arbitrary standard library generics | unverified |
| External toolchain provider (nvcc / CUDA via a pure C++ header contract) | yes ([dci_cuda](../tests/projects/dci_cuda/); free functions with `void*` fall back to `Translated`) | — | — |
| Open generics closed at the call site (generic record member methods included) | yes (dci_opengeneric cpp target) | yes (dci_opengeneric rust target) | rejected (no Active path) |
| Multiple point-to-point contracts in one consumer | yes ([dci_multilang](../tests/projects/dci_multilang/): Rust + Zig + MSVC C++) | yes | yes |

Producer -> consumer reverse override (Stub; vtables measured by the producer):

| Capability | C++ | Rust | Zig |
|---|---|---|---|
| Inherit named base / trait + `override` | yes ([dci_cpp_trait](../tests/projects/dci_cpp_trait/): measured C++ virtual dispatch); [Qt counter](../probes/gates/dci-qt-counter/README.md) verifies Windows AOT construction, native event overrides and cleanup | yes ([dci_rust_trait](../tests/projects/dci_rust_trait/): `SinkG<i32>`) | unverified |
| Inherit a **closed** generic trait instance | yes ([dci_cpp_trait](../tests/projects/dci_cpp_trait/): `VyxHostI32 : SinkG<i32>`, consumer-discovered demand and producer instance supplement; no handwritten `--export-instance`) | yes ([dci_rust_trait](../tests/projects/dci_rust_trait/): the same instance-demand path; `--export-instance 'SinkG<i32>'` is the equivalent explicit request) | unverified |
| Generic Vyx stub class (`class VyxHostTG<T> : native.SinkG<T>`) | yes (dci_cpp_trait, 2026-09-24: `VyxHostTG<i64>` / `VyxHostTG<VyxBox>` with direct virtual calls) | yes (dci_rust_trait, 2026-09-23: i32 / i64 / char) | unverified |
| Consumer-owned `@[repr(C)]` record as a generic argument | yes (dci_cpp_trait, 2026-09-24: 12-byte `VyxBox`, indirect + sret) | yes (dci_rust_trait, 2026-09-23: 12-byte `VyxBox`, indirect + sret) | unverified |
| Producer record as a generic argument of an inherited base | yes (dci_cpp_trait, 2026-09-24: 8-byte `Sample`. Win64 MSVC member functions return every UDT through the hidden pointer — the adapter records the member return as `sret` — while the override thunk crosses as an `extern "C"` free function, so an 8-byte record is coerced to one register in both directions) | rejected (measured limits for `SinkG<Sample>`; free-function generic records are unaffected) | rejected |

Mechanism gates ([probes/gates](../probes/gates/)) cover cross-language
machinery rather than any single producer language:

| Gate | Coverage |
|---|---|
| [dci-active](../probes/gates/dci-active/) | Active Adapter: on-demand closure of generic calls (rust + cpp), publishing producer objects into the bundle cache, offline replay from the bundle alone, llvm-nm symbol uniqueness |
| [dci-failure](../probes/gates/dci-failure/) | Error paths: `Result<i32, dci.Failure>` Err payload (message / payload / producer_tag) across the contract |
| [dci-inalloca](../probes/gates/dci-inalloca/) | Large aggregate by value (4×i64 `Blk`) via the inalloca path |
| [dci-landingpad](../probes/gates/dci-landingpad/) | Exception propagation through contract calls (landingpad) |
| [dci-storage](../probes/gates/dci-storage/README.md) | Owning local allocation, destruction and release balance; scopes, reassignment, early return and loop exits |
| [dci-exceptions](../probes/gates/dci-exceptions/README.md) | AOT MSVC/Itanium shared propagation, exact destructor order, constructor failure, inline records and double unwind |
| [dci-vector](../probes/gates/dci-vector/README.md) | Original standard vector template, producer narrowing rejection, allocation accounting and native bounds exceptions |
| [dci-cpp-ecosystem](../probes/gates/dci-cpp-ecosystem/README.md) | ICU 78.3 original C++ API and independent native reference |
| [dci-rust-ecosystem](../probes/gates/dci-rust-ecosystem/README.md) | Locked/offline registry crates, Cargo environment replay and native oracle |
| [dci-industrial](../probes/gates/dci-industrial/README.md) | Repeated real fixtures, bounded parallel orchestration, child wall time and sampled process-tree RSS |

The Qt counter gate now verifies standalone Adapter preparation, visible Vyx
definitions and native event dispatch on Windows AOT, plus O0/O2 connection
lifetime and shared-ABI cleanup. Other retained Qt gates were not evaluated in
this run; earlier failures are recorded in [dci-storage](../probes/gates/dci-storage/README.md).
The old `tests/projects/dci_spdlog` fixture and `probes/gates/gate-c` were retired
on 2026-09-28. Required project acceptance uses `dci_cpp_trait`, `dci_rust_trait`,
`dci_multilang` and `dci_zig_abi`, alongside the relevant mechanism gates.

Rejected outright (negative tests or measured limits): data-carrying ZST
parameters; non-trivial classes by value in parameters or returns; by-value
objects lacking lifecycle evidence; hidden-parameter ABIs; the `split` form for
8/16-byte multi-field aggregates (12-byte indirect + sret is the supported
path); derived Vyx stub classes declaring their own fields; `x.func()` virtual
dispatch syntax; C shim generic exports; a dedicated CUDA language family.

Known current state, recorded honestly: consumer-driven instance discovery
landed on 2026-09-24 — lowering records every generic instance it materializes in
the `.dci_open` sidecar, and the build asks the producer to supplement those
instances, with at most three retry rounds per compile job. Vyx fixtures need no
handwritten `--export-instance` list; `dci_rust_generic` has its own regression
gate. `--export-instance` remains the explicit request
path for a producer used outside a Vyx build.

## Lifecycle and control flow

With `profile.lifecycle_binding = automatic`, the HIR binder implements supported
local copy/move decisions and scope cleanup, validates allocator-domain information,
and restricts reuse of moved-from `destructible_only` values.
Shared values have scope-exit release; retain remains explicit.
These transformations are not a complete path-sensitive ownership analysis.

Layout compatibility never substitutes for nontrivial lifecycle operations.
The contract must identify the required copy, move, destruction, or release behavior.
Owning pointer-represented DCI locals use the generated module's allocation and
release pair: destruction is followed by storage release. Inline stack records
receive destruction without a heap release, and borrowed objects are not freed
by the consumer. See the [storage regression](../probes/gates/dci-storage/README.md).

The default boundary is `no_unwind`:

- `translated` requires a resolvable no-unwind translator; the producer converts
  its exception or panic before the boundary.
- AOT `shared_abi` supports `dci.eh.msvc-cxx.v1` on x86_64 Windows MSVC and
  `dci.eh.itanium-cxx.v1` on x86_64 Linux. Both the module and the called
  operation must participate in that target's versioned propagation ABI.
  Unknown identities, versions, targets and nonparticipating callers are rejected.
- A native C++ exception preserves its identity across Vyx frames. Completed
  scoped objects with resolved MIR Drop operations are destroyed in reverse
  construction order during unwind. This includes DCI inline records and
  native Vyx constructors, factories and aggregate initializers. DCI pointer
  storage is released after destruction; a failed DCI constructor releases only
  its allocated storage. Normal scope-exit order retains existing Vyx semantics.
  Cleanup operations must use a complete-object `void(pointer)` ABI. A second
  exception from destruction during unwind terminates the process.
- JIT shared unwind is deferred and is not part of this implementation's
  validated exception model. Rust panic, Zig errors and `longjmp` do not gain
  shared propagation merely by naming a C++ unwind ABI.
- That cleanup path is not general foreign catch/throw synthesis. Unknown propagation
  behavior is not accepted as implicit compatibility.

### Cross-module lifecycle and exception obligations

Contracts that participate in a shared unwind boundary may publish a versioned
fact graph at the root `obligations` key:

```json
{
  "version": "1",
  "lifecycle": [{"subject": "eh::Guard", "ownership": "unique",
    "on_unwind": "release", "required_operations":
    [{"name": "destroy", "symbol": "guard_destroy", "when": "unwind"}]}],
  "exceptions": [{"subject": "contract_throw", "boundary": "shared_abi",
    "abi": "dci.eh.msvc-cxx.v1", "cleanup": "unwind"}],
  "edges": [{"from": "exception:contract_throw",
    "to": "lifecycle:eh::Guard", "kind": "cleanup-before-propagate"}]
}
```

`profile.obligation_mode = "required"` makes the graph mandatory. The DCI
validator merges all input JSON/DCIB documents before consumption, so a
producer may publish exception and lifecycle facts in separate modules. It
rejects unresolved nodes, conflicting declarations, dependency cycles, and a
`shared_abi` exception without a cleanup edge. This graph carries evidence for
the Consumer; it does not infer a foreign ABI or replace the target-specific
runtime unwind implementation. The Vyx compiler performs the same checks when
it consumes either an explicit `--dci <file.dcib>` or a source-discovered
`@[dci_import]` contract: all contracts are loaded into the live Fact Semantic Ownership System
EffectPlan before HIR lowering. In the Effect manifest representation,
exception and lifecycle subjects are canonicalized as
`dci:exception:<subject>` and `dci:lifecycle:<subject>`, and edge digests are
kept separate from edge kinds.

## Implementation and verification

The repository implements a feature subset, not full Core 1.0 or full L3 conformance.
Source and test entry points:

| Area | Source / coverage |
|---|---|
| Adapter CLI and codec | [tools/dci](../tools/dci/README.md), [CLI tests](../tools/dci/tests/test_dci_cli.py), [codec tests](../tools/dci/tests/test_dcib.py) |
| Active queries | [active_protocol.py](../tools/dci/active_protocol.py), [protocol tests](../tools/dci/tests/test_active_protocol.py) |
| Artifact identity/publication | [artifact_bundle.py](../tools/dci/artifact_bundle.py), [bundle tests](../tools/dci/tests/test_artifact_bundle.py) |
| C++ and Rust instantiation | [Active C++ tests](../tools/dci/tests/test_active_cpp.py), [Active Rust tests](../tools/dci/tests/test_active_rust.py), [open-generics project](../tests/projects/dci_opengeneric/README.md) |
| Lifecycle and wrappers | [HIR binder](../bootstrap_compiler/src/hir/dci_binder.vyx), [LLVM Consumer](../bootstrap_compiler/src/codegen/llvm_lower.vyx), [Consumer tests](../tools/dci/tests/test_dci_consumer.py) |
| Native class/trait use | [dci_complex_abi](../tests/projects/dci_complex_abi/), [dci_rust_trait](../tests/projects/dci_rust_trait/) |
| Object storage and exception cleanup | [llvm_lifetime.vyx](../bootstrap_compiler/src/codegen/llvm_lifetime.vyx), [llvm_unwind.vyx](../bootstrap_compiler/src/codegen/llvm_unwind.vyx), [dci-exceptions](../probes/gates/dci-exceptions/README.md) |
| Ecosystem and pressure acceptance | [C++ ICU](../probes/gates/dci-cpp-ecosystem/README.md), [Rust Cargo](../probes/gates/dci-rust-ecosystem/README.md), [industrial runner](../probes/gates/dci-industrial/README.md) |

Contract validation, producer verification, and linked execution answer different
questions. Run the appropriate layers for the operation being changed.
Tool commands and prerequisites are in the [DCI tools guide](../tools/dci/README.md).
Acceptance uses the SDK compiler built from the current source tree, its matching
backend/runtime, and AOT executables. JIT parity is later
work. Repeated fixture passes establish the named cases; remaining closure,
ownership and ecosystem gaps still need evidence before industrial readiness
can be claimed.

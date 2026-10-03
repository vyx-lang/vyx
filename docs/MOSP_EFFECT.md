# Effect Model

[简体中文](MOSP_EFFECT_ZH.md) · [Fact Semantic Ownership System core features](MOSP.md) · [Documentation](README.md)

Status: design plus current implementation. The model below records the parts
that are normative and the parts currently exercised by the AOT compiler.

Current implementation status: Sema normalizes `@[name(...)]` and
`@[name]` into `EffectDecl`/`EffectFact` rows, validates handler targets and
argument policy, records deterministic plan results and obligations, and emits
the `[MOSP effect]` summary. `VYX_EFFECT_MANIFEST_OUT` publishes a versioned,
escaped unit manifest; `VYX_EFFECT_MANIFEST_IN` validates and merges producer
rows into the consumer plan, rejecting stale target/ABI/scope/source/compiler
stamps. The `reflect` handler authorizes Reflection metadata and `retain`
authorizes a HIR code-generation root (`dce.retain`); `migrate.collect`
authorizes field alias materialization, and `async.lower`/`task.lower`
authorize the coroutine task flag. These lowering boundaries consume
compiler-owned markers rather than trusting raw attribute text. `platform.select`
also gates declaration filtering before semantic resolution; `derive.expand` and
`comptime.evaluate` gate trait synthesis and constant folding. `dci.import`
stamps the accepted declaration tree, and automatic `.dcib` discovery is
admitted to the lifecycle binder only after the import marker is sealed.
`dci.export` is normalized and stamped for the declaration tree. The marker
keeps exported functions as AOT roots, and `VYX_DCI_EXPORT_OUT` selects a
stable data-only `MOSP-DCI-EXPORT` sidecar for bridge tooling. The sidecar is
not a foreign ABI contract; C++/Rust/Zig ABI contracts remain produced by the
DCI adapters. Explicit `--dci` inputs and source-discovered `@[dci_import]`
inputs are both parsed before Sema freezes the plan; their lifecycle,
exception, symbol, edge, and `shared_abi` cleanup obligations enter the same
EffectPlan. A malformed or incomplete contract therefore fails the consumer
even when no bridge manifest was supplied.

The current AOT completion evidence also includes the compiler-owned declarative
`myAttr` schema handler (`level="abi"|"layout"`, optional `retain=bool`).
Argument, target, and imported-manifest validation are enforced; a valid attribute
produces a `handled` result, and only `retain=true` authorizes an otherwise-unreached
declaration into MIR. `tools/effect_manifest.py inspect/validate` uses the same
canonical row ordering, escaping, and fingerprint rules as the SDK and independently
checks emitted artifacts. DCI source content stamps participate in manifest freshness.
The Fact Semantic Ownership System, DCI exception/vector, and nine real DCI project gates run against the
current SDK compiler. Cross-package build orchestration must still pass the producer
source/compiler stamps explicitly; a consumer rejects the manifest when they are absent.

Package-owned declarative schemas are now a data-only extension of the same path. Project
builds discover root-level `.attr` files and the files declared in `[effect]` in `Vyx.toml`
before compiling Vyx units. Each file can contain multiple `attribute` blocks, use Vyx scalar
type names, and reuse parameters from another schema with `extends`. The build merges these
schemas with dependency-package schemas, validates the aggregate, and includes its content
in the object/interface cache stamp. No per-build schema environment variable is needed.

The older validated `MOSP-EFFECT-SCHEMAS\t1` table supplied through `VYX_EFFECT_SCHEMAS_IN`
registers typed `package.attribute` handlers (legacy bool/int/string/enum arguments,
canonical defaults, declaration targets, and a retain/reflect consequence) without
loading executable compiler plugins. The schema content digest is recorded as a
manifest dependency and is checked by consumers; changing a schema invalidates an
otherwise matching manifest. Malformed schemas, unknown targets or types, invalid
arguments, and schema/handler conflicts fail closed.

The EffectPlan also owns a typed dependency-edge table. Registered Effects add
their declaration-to-handler edge, while imported manifests can add
cross-module edges. Edges use a complete deterministic DFS: every outgoing
branch is considered and the lexicographically smallest complete cycle path is
reported. A back edge creates an `EFFECT-CYCLE` obligation and prevents the
plan from sealing. DCI lifecycle and exception obligations use the same
manifest boundary: the DCI validator merges producer modules, and its bridge
encodes `dci.lifecycle`/`dci.exception` records plus `edge:` dependencies for
`VYX_EFFECT_MANIFEST_IN`. The consumer imports those rows into the live
EffectPlan before HIR lowering, so cleanup-before-propagate edges, unresolved
nodes, conflicts, and cycles are checked by one fact graph. Canonical imported
node IDs are `dci:exception:<subject>` and `dci:lifecycle:<subject>`.

The sidecar is selected with `VYX_DCI_EXPORT_OUT=/path/to/file` and uses the
same escaped tab-field convention as the Effect manifest:
`H\tMOSP-DCI-EXPORT\t1\t<unit>\t<plan-fingerprint>` followed by
`R\t<subject>\tdci_export\t<arguments>\t<phase>\t<scope>\tregistered` rows.
The header fingerprint is the sealed EffectPlan fingerprint that authorized
the export.  A bridge or build cache MUST inspect the sidecar before consuming
generated declarations or native objects; `python tools/effect_manifest.py
validate-dci-export <sidecar> --manifest <same-build-manifest>` checks the
header, escaping, canonical row order, duplicate identities, and
compiler-authorized status, then compares the plan dependency digest and the
complete authorized export fact set with the same-build manifest.  This check does
not replace foreign ABI validation; it binds that validation to the exact
EffectPlan provenance.

## 1. Scope and goals

Fact Semantic Ownership System treats verified program facts as explicit compilation objects. Its existing features cover version time, target and layout space, runtime contracts, and compile-time processing. Effect adds the fact dimension: it defines where a fact comes from, where it is valid, and which verifiable compilation consequences follow when it is established.

Effect is not a runtime IO, mutation, scheduling, or exception type system. It describes the impact of metadata on the fact graph, compiler phases, diagnostics, and artifacts. A runtime-pure function may produce an Effect; a function that writes memory may have no Effect.

The model has five goals:

1. Give Migrate, Reflection, DCI, DCE, derive, comptime, async, and platform selection one fact-propagation model.
2. Make every metadata propagation traceable to input facts, handler versions, and output artifacts.
3. Verify layout, ownership, lifecycle, exception, and compile-time lowering facts across modules.
4. Invalidate incremental builds from fact fingerprints instead of hidden compiler state.
5. Make metadata effects on the self-hosting compiler reproducible and fixed-point verifiable.

The model does not let ordinary source code modify the compiler arbitrarily, and it does not require a Consumer to parse C++, Rust, or another producer language.

## 2. Normative language

**MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** have their usual normative meanings in this document.

## 3. The five Fact Semantic Ownership System dimensions

| Dimension | Question | Representative mechanisms |
|---|---|---|
| Time | In which version or migration interval is an entity valid? | `version`, `variant`, Migrate |
| Space | For which target, ABI, layout, module, or allocator domain is a fact valid? | target, layout, DCI, allocator domain |
| Runtime | How will generated code call, own, destroy, and propagate errors? | lifecycle, exceptions, async, calling convention |
| Compile time | At which phase will the compiler discover, retain, generate, or lower code? | Reflection, DCE, derive, comptime, MIR lowering |
| Fact | Why may the compiler know something, where did it come from, and what does it affect? | Fact, provenance, Effect |

The fact dimension crosses the other four. It is not another runtime region; it is the knowledge, source, verification state, and causal relationship of program facts. Effect is the operational semantics of the fact dimension: a fact says what is known, and an Effect says what must happen because it is known.

## 4. Core definitions

### 4.1 Entity

An entity is a stable object that an Effect may observe or affect: a module, type, field, function, method, closed generic instance, DCI operation, Reflection entry, or compiler phase. A display name is not an identity by itself.

An entity identity contains at least:

```text
package identity
module identity
version / variant
declaration kind
declaration identity
generic arguments when closed
```

### 4.2 Fact

A Fact is verified, referenceable program knowledge with a validity scope:

```text
Fact {
    key: FactKey
    subject: EntityId
    value: CanonicalValue
    scope: ValidityScope
    provenance: Provenance
    dependencies: [FactRef]
    status: observed | verified | derived | external
    fingerprint: Hash
}
```

The key, subject, value, and scope define the semantic identity of a fact. Provenance and dependencies define trust and invalidation. A fact value MUST be canonical; an unresolved piece of producer source is not a verified fact.

### 4.3 Validity scope

`ValidityScope` may include:

```text
target triple / operating system
ABI profile and calling convention
toolchain identity and version
build mode and feature flags
package / module version and variant
allocator or runtime domain
source artifact identity
```

A Consumer may use a fact only when its scope contains the fact scope. A mismatch MUST produce an error or an explicit downgrade. Matching names or similar layouts do not widen a scope.

### 4.4 Effect

An Effect is a declarative, phase-scoped, verifiable fact transformation. It reads facts, checks obligations, and produces facts, invalidations, declarations, or artifacts:

```text
Effect(subject, phase, read_facts)
    -> add_facts
    -> invalidations
    -> obligations
    -> declarations
    -> artifacts
    -> rerun_requests
```

An Effect writes to the fact graph and registered compiler capabilities, never to arbitrary compiler globals. Each Effect has a stable ID, handler version, input fingerprint, and provenance.

### 4.5 Effect handler

In the target model, an Effect handler is a deterministic compiler-registered
processor. It defines the input schema, facts it reads, output schema, valid
phases, and capabilities. The current code has this schema and a deterministic
dispatch result table. `reflect.collect`, `dce.retain`, `migrate.collect`, the
async/task lowering handlers, `platform.select`, `derive.expand`, and
`comptime.evaluate` have real consumers. Built-in schemas carry a deterministic
version and capability label; their schema digest is included in exported
manifest dependencies. `dci.import` has a driver/codegen
authorization boundary: source-discovered `.dcib` paths require the
compiler-owned import marker, while explicit `--dci` paths remain compatible
with direct/inline contracts. `dci.export` additionally supplies an AOT root
and an explicitly selected `MOSP-DCI-EXPORT` sidecar through
`VYX_DCI_EXPORT_OUT`; it does not pretend to infer a foreign ABI layout.
Only SDK compiler handlers may execute; ordinary packages cannot inject host
code into the compiler process.

### 4.6 Effect plan

In the target model, an `EffectPlan` is built after version resolution and
Migrate canonicalization. It contains ordered Effect instances, input
fingerprints, dependency edges, phase boundaries, output facts, and obligations.
The current implementation stores normalized `EffectFact` rows, handler
metadata, deterministic result and obligation tables, a typed dependency-edge
table, counters, and a frozen fingerprint that folds all five tables. A compact
manifest codec provides cross-module export and import;
the CLI requires explicit freshness stamps before merging incoming rows.

## 5. Effect result classes

An Effect may produce several result classes, but each must be explicit.

### 5.1 Derived facts

Examples include field offsets derived from layout facts, historical aliases derived from Migrate, and lifecycle capabilities derived from a DCI contract. Derived facts must retain their dependencies and cannot be presented as producer observations.

### 5.2 Compilation obligations

An obligation is a condition that must be checked now or in a later phase:

- an owned value has a matching destroy, release, or allocator domain;
- a DCI boundary has a propagation ABI supported by both sides;
- a Reflection name is unique and its entry is callable;
- a migrated field exists in the selected historical version;
- a closed generic satisfies producer constraints and has a closed layout.

An unfulfilled obligation MUST fail the build. An Effect cannot turn it into an empty diagnostic.

### 5.3 Declarations and bridge artifacts

An Effect may request compiler-registered declarations, compatibility shims, Reflection manifests, DCI stubs, or drop glue. Generated artifacts must carry their source Effect and input fingerprint. An untraceable declaration must not appear implicitly in a source tree.

### 5.4 Retention and elimination

An Effect may add DCE roots, retain metadata, register dynamic entry points, or report facts as invalid. It cannot delete content required by exports, Reflection, destruction, or an external contract.

### 5.5 Lowering and code generation requests

An Effect may select a registered HIR, MIR, or LLVM lowering, such as an async state machine, comptime fold, derive method, or ABI call sequence. It cannot invent undeclared inputs or infer missing layout from a name.

### 5.6 Invalidation and reruns

An Effect may request recomputation of a function, module, target, or global fact. The invalidation scope must be the smallest provable scope. Unknown scope must be widened safely or rejected; stale output cannot be silently reused.

## 6. Effect declaration model

The compiler accepts a contextual top-level `effect` declaration. Existing `@[migrate]`, `@[reflect]`, `@[dci_import]`, `@[derive]`, `@[comptime]`, `@[async]`, `@[platform]`, layout attributes, and custom `@[name(...)]` forms continue to normalize into structured `EffectDecl` and `EffectFact` rows. The declaration is compile-time data: it enters the EffectPlan and manifest, but never becomes a function or runtime symbol.

```text
effect reflect_alias {
    phase: metadata;
    reads: [migrate.field_alias, reflect.registration];
    provides: [reflect.name_alias];
    requires: [stable_identity];
    capability: [retain_metadata];
}
```

`effect` is contextual at declaration position, so an existing identifier or member named
`effect` remains valid in expression position. The required fields are `phase`, `reads`,
`provides`, `requires`, and `capability`; `writes`, `handler`, `version`, `scope`,
`determinism`, and `diagnostics` are optional. Field names are unique and values are
data-only token lists. Unknown, duplicate, empty, or malformed fields are diagnostics.

An effect without `handler` is a declaration-only fact contract. An explicit `handler` must
resolve to a compiler-registered schema and its optional `version` must match. This keeps
source syntax useful for package contracts without introducing executable compiler plugins.

An EffectDecl contains at least:

```text
effect_id
handler_id and handler_version
phase
read fact patterns
write fact keys
required capabilities
scope constraints
determinism declaration
diagnostic policy
```

`reads`, `writes`, and `requires` are audit boundaries. A handler reading an undeclared fact or writing an undeclared fact MUST fail.

## 7. Compilation phases and ordering

Effects follow these logical phases:

| Phase | Main work | Possible Effect results |
|---|---|---|
| Parse | Collect attributes and source provenance | Raw Effect requests |
| Identity | Resolve modules, versions, variants, and entities | Identity facts |
| Migrate | Canonicalize historical relationships | Migration facts and compatibility obligations |
| Semantic | Check types, generics, visibility, and constraints | Type and constraint facts |
| Layout / ABI | Compute layout, calls, lifecycle, and allocator domains | ABI, layout, and drop facts |
| Metadata | Finalize Reflection, DCI, exports, and retention | Manifests, roots, bridge requests |
| MIR | Run registered lowering and local verification | MIR requests and function facts |
| DCE / Codegen | Compute reachability and emit target code | Machine code, symbols, invalidation summaries |
| Link | Verify final artifact identity and dependencies | Link facts and final report |

Version resolution and Migrate precede Effects that depend on them. An Effect may write only to the current or a later phase; it cannot silently mutate a frozen earlier phase.

The self-hosting compiler keeps function-level MIR streaming. Global Effect
information crosses units through the compact manifest and summary paths above;
calculating Effects must not restore a whole-unit rescan or a large-memory
model.

## 8. Evaluation semantics

### 8.1 Determinism

The same source, compiler and handler versions, target, configuration, and input facts MUST produce the same EffectPlan and output fingerprints. An Effect cannot read time, randomness, network responses, undeclared environment variables, or process-global state.

### 8.2 Monotonic facts and explicit invalidation

The default fact graph is monotonic: an Effect adds verified facts but does not silently overwrite an old fact. Replacing or retracting a fact requires an explicit invalidation with a reason.

After the FactBase is frozen, a new write must go through a new EffectPlan generation. A write-after-freeze cannot be silently absorbed.

### 8.3 Fixed points

Effects may iterate to a fixed point over a finite monotonic fact lattice. Every round must:

1. run handlers against a fact snapshot;
2. normalize and order outputs;
3. check additions, invalidations, and conflicts;
4. use new fingerprints for the next round.

An infinite entity stream, non-monotonic cycle, or order-dependent result is a diagnostic failure. An implementation cannot truncate at an iteration limit and report success.

### 8.4 Conflicts

If two Effects produce different canonical values for one fact key or different generated declarations for one symbol, the build must report a conflict. The diagnostic includes both sources, input facts, scopes, and handler versions.

### 8.5 Capability checks

An Effect may run only when the current phase grants its capabilities, such as `retain_metadata`, `emit_stub`, `request_codegen`, `add_dce_root`, or `rerun_phase`. Capabilities are compiler-registered and scoped; they are not arbitrary string labels.

## 9. Migrate and Effect

Migrate establishes identity correspondence along the version axis:

```text
old entity + mapping + version scope
    → current entity
```

Effect reads that relation and propagates its consequences:

```text
Migrate relation
    → reflection alias
    → compatibility diagnostic
    → DCI schema check
    → generated shim
    → invalidation of dependent artifacts
```

The following rules are mandatory:

1. Effects use only canonicalized Migrate relations.
2. An old Effect is not inherited merely because a function signature matches; an entity mapping is required.
3. Field migration describes API and metadata correspondence, not automatic conversion of disk data or memory objects.
4. Incompatible layout, lifecycle, or exception facts create obligations or reject the mapping.
5. Historical entry retention depends on references, exports, and version scope; unknown content is not retained forever.

## 10. Integration with Fact Semantic Ownership System subsystems

### 10.1 Reflection

Reflection Effects read registered names, aliases, member visibility, and layout. They produce type tables, member tables, callable entries, and DCE roots. A `hidden` member cannot be re-exposed by a Reflection Effect.

Runtime lookup validity, bound-object ownership, and method signatures remain separate checks; the Effect makes their requirements explicit.

### 10.2 DCI

DCI Effects read target, ABI, layout, ownership, lifecycle, exception, and implementation facts from `.dcib`. They produce Consumer call checks, Direct or Stub requests, constructor/copy/move/destructor/release entries, generic closure requests, exception conversion obligations, and final link dependencies.

The Consumer must not infer an Effect from a producer language name. `source.language` is origin and diagnostic data; it cannot select layout or lifecycle algorithms.

### 10.3 DCE

DCE Effects turn exports, Reflection, dynamic calls, destruction, external contracts, and observable behavior into roots or retention obligations. Ordinary runtime read/write analysis belongs to MIR analysis and must remain separate from Fact Semantic Ownership System fact writes.

### 10.4 derive, comptime, async, and platform

These attributes can be normalized into built-in Effects:

| Attribute | Effect result |
|---|---|
| `derive` | Generated declarations, trait facts, and constraint diagnostics |
| `comptime` | Deterministic value and input fingerprint |
| `async` | Future facts, MIR await lowering, and cleanup obligations |
| `platform` | Target filtering and scope mismatch diagnostics |
| `repr/align/packed` | Layout facts and ABI revalidation |

Normalization does not change existing semantics. A feature must not be reported as having new Effect behavior until its verification gate exists.

## 11. Compiler Effects

Compiler Effects have three scopes:

```text
program   → facts and code of the generated program
toolchain → target, ABI, linking, and artifacts
compiler  → registered compiler phase capabilities
```

The `compiler` scope may add or invalidate facts, request a registered lowering, request a phase rerun, add diagnostics or DCE roots, and generate verified declarations or manifests.

It must not modify parser tables, replace the type system or MIR verifier, access undeclared files or network state, silently mutate global configuration, or swallow an unsupported capability.

Handler implementations belong to the SDK compiler. A package submits declarations and input facts, not arbitrary host code executed inside the compiler.

## 12. Cross-module artifacts, fingerprints, and caching

Every consumable module artifact must carry a logical Effect manifest containing:

```text
entity identities
exported facts
effect declarations and handler versions
required capabilities
validity scopes
generated declarations and artifacts
input/output fingerprints
invalidation summary
```

The physical encoding may be embedded in `.vyi`, `.dcib`, or another module artifact, or carried by a sidecar. The encoding does not alter the logical requirements.

Cache keys cover at least the source or artifact identity, compiler and handler versions, target and ABI scope, build configuration, input fact fingerprints, and the EffectPlan fingerprint.

Unknown Effect scope must widen invalidation. An old manifest cannot be reused when scope or handler versions do not match.

## 13. Standard-library requirements

Public standard-library types and operations should gradually declare Effect facts for ownership, borrowing, copy, move, destruction, allocation domains, layout, platform support, exception or error propagation, Reflection retention, DCI boundaries, comptime, and async lowering.

Generic libraries must propagate element Effects at instantiation. A name such as `vector`, `string`, or `result` does not prove a layout, release, or exception capability.

## 14. Source syntax and compatibility

The source form is now available on the AOT path. It must:

1. name its phase, reads, provides, and requires;
2. request only registered capabilities;
3. avoid arbitrary host-language execution;
4. diagnose unknown handlers, unknown inputs, and scope mismatches;
5. allow older clients to consume only manifest versions they understand;
6. version schema changes with an explicit compatibility policy.

The declaration is skipped by HIR lowering after semantic validation, so an effect block cannot
silently add a function, global, or runtime side effect. JIT support remains a later alignment
task; the current contract and gates target the native AOT compiler.

### 14.1 Custom attributes: `@[myAttr(...)]`

Custom attributes are the user-facing entry point to the fact dimension. The following form is parsed as declaration metadata:

```vyx
@[myAttr(level="abi", retain=true)]
public class Buffer {
    // ...
}
```

The attribute first produces an `AttributeFact`. It does not execute compiler code merely because it is named `myAttr`. A compilation consequence exists only after the attribute is registered, its arguments pass schema validation, and it is bound to an Effect handler.

There are three attribute classes:

| Class | Behavior |
|---|---|
| Built-in | Registered by the compiler, such as `migrate`, `reflect`, and `async` |
| Declarative custom | Has a schema and Effect handler and may derive facts, obligations, or artifacts |
| Opaque | Preserved for tools and IDEs but has no compiler semantics |

### 14.2 Declaring schemas in `.attr` files

An `.attr` file is a separate declarative input. It uses Vyx type names and literal syntax,
but does not define Vyx functions, imports, classes, or executable handlers. For example,
`acme.attr` can contain several schemas:

```vyx
attribute acme.layout {
    version: 1;
    targets: [type, field];

    arguments {
        mode: enum("abi", "layout");
        align: i32 = 0;
        retain: bool = false;
    }

    consequence: retain;
}

attribute acme.note {
    version: 1;
    targets: [function];
    arguments {
        label: string = "startup";
    }
    consequence: record;
}
```

The current literal-backed parameter types are `bool`, `i8`, `i16`, `i32`, `i64`,
`u8`, `u16`, `u32`, `u64`, `f32`, `f64`, and `string`. Integer values and defaults must
fit their declared width and signedness. Numeric literals use decimal notation, with
exponents allowed for floats; strings use double quotes. `enum("abi", "layout")` describes
a closed set of string values spelled as identifiers. `.attr` does not accept `int`;
choose a Vyx integer type explicitly.
Arbitrary runtime objects and expressions are not schema values.

`version`, `targets`, `arguments`, and `consequence` are required. Supported targets are
`function`, `type`, `field`, `module`, `variant`, `variable`, `use`, and `declaration`; consequences are `record`,
`retain`, and `reflect`. A required parameter has no default. The two latter consequences
authorize the existing compiler-owned retention or reflection path, rather than defining
a new handler body.

After loading `acme.attr`, use its qualified name in Vyx source:

```vyx
@[acme.layout(mode="abi", align=16, retain=true)]
class Packet {
    public id: i32;
}
```

Parameter names are data: an argument named `align` does not itself change layout.
The declared consequence authorizes the existing compiler path. Member facts include
their owner identity, so equally named fields in different types remain distinct.

### 14.3 Project loading and parameter reuse

`vyxc build` discovers `.attr` files in the package root. Files in other directories can be
listed once in `Vyx.toml`:

```toml
[effect]
attr_files = ["attrs/common.attr", "attrs/storage.attr"]
auto_discover = true
```

Paths are relative to the manifest. Auto-discovery loads only `.attr` files in the package root;
schemas in subdirectories do not automatically become compiler inputs. Set `auto_discover = false`
to use only the explicit list. Each dependency package loads its own schemas when compiled;
list shared schema files explicitly when reusing them in another package. Every selected file
is a semantic build input: path and content
changes invalidate object/interface caches and the schema digest in Effect manifests.

Parameters can be shared across schemas and files:

```vyx
attribute acme.base {
    version: 1;
    targets: [type, field];
    arguments {
        align: i32 = 0;
    }
    consequence: record;
}

attribute acme.layout {
    version: 1;
    targets: [type, field];
    extends: [acme.base];
    arguments {
        mode: enum("abi", "layout");
    }
    consequence: record;
}
```

`extends` copies parameters; each schema still declares its own version, targets, and
consequence. References are resolved after all selected files are parsed, so declaration
order does not restrict reuse. Unknown bases, inheritance cycles, duplicate schema names,
and conflicting inherited/local parameter names fail before Vyx source is compiled.
The current parser limits an inheritance chain to 64 edges and reports an explicit depth error.

Arguments are limited to canonicalizable literals, identifiers, paths, lists, and nested attribute values. An attribute cannot carry an arbitrary Vyx expression, host-language closure, or unverified source fragment.

The source spelling may use the short name `myAttr`, but the fact identity includes the registering package and schema version. Built-in names belong to a compiler-reserved namespace. Cross-package use must resolve a qualified identity through the toolchain; two packages having the same short name is not enough.

An attribute applies to its declaration by default. It does not automatically propagate to fields, methods, generic instances, or child modules. A handler must declare propagation and the new subject identity. An attribute is not automatically inherited by a historical entity in a Migrate family.

Across modules, only exported attributes and their canonical `AttributeFact` enter the Effect manifest. An opaque attribute may remain available to tools but cannot trigger Consumer compilation semantics. If the registering package is missing, its version is incompatible, or its schema does not match, the Consumer must fail or perform an explicitly documented downgrade.

The relationship between a custom attribute and Effect is:

```text
@[myAttr(...)]
    ↓ parse / canonicalize
AttributeFact(package.myAttr@schema, subject, arguments)
    ↓ registered handler
EffectPlan
    ↓
derived facts / obligations / artifacts / lowering
```

For example, `myAttr.validate` may read the attribute arguments, layout, and DCI contract, then produce a `stable_layout` obligation and a retention fact. It cannot call `malloc`, modify parser tables, or write compiler-global state.

If a custom attribute participates in Migrate, its attribute facts must have an explicit mapping:

```text
old package.myAttr@1 on old entity
    -- explicit migration mapping -->
new package.myAttr@2 on current entity
```

The compiler must not copy an old attribute merely because the short name and entity signature match. Argument changes, schema changes, and handler changes are compatibility inputs.

An unregistered `@[myAttr(...)]` may still be preserved by the parser as opaque metadata, but it has no semantic effect. Once registered, malformed arguments, an invalid target, an unavailable handler, or a missing capability must be a source-located compilation error.

## 15. Diagnostics and failure rules

Implementations should cover at least these categories:

```text
missing_fact              required fact is absent
scope_mismatch            target or ABI scope does not match
effect_conflict           Effects produce incompatible results
effect_cycle              an illegal dependency cycle exists
write_after_freeze        a frozen fact graph is written
nondeterministic_effect   input or output is not deterministic
unknown_handler           handler is absent or incompatible
missing_capability        the current phase lacks a capability
stale_artifact            a manifest or generated artifact is stale
unfulfilled_obligation    a compilation obligation was not met
```

Diagnostics identify the triggering entity, Effect ID, input facts, scope, and repair direction. `exit=77` means an explicit skip; it cannot represent an Effect failure followed by a successful build.

## 16. Security and trust boundaries

External `.dcib`, module manifests, and adapter output are untrusted inputs. A Consumer verifies format, toolchain identity, scope, fingerprints, and lifecycle facts before using them in an Effect.

Handlers cannot use undeclared files, network, time, randomness, or environment variables. Producer source is interpreted only by its Adapter and producer toolchain; a Consumer must not start parsing C++, Rust, or Zig because an Effect is present.

## 17. AOT, JIT, and runtime boundaries

The first implementation and verification target native AOT. Effects may produce facts, bridges, and lowering requests required by AOT; JIT state cannot replace a verifiable manifest.

If JIT later consumes Effects, it must first obtain a stable, hashable native entity and ABI fact. Dynamic objects without stable facts belong to another protocol and do not extend DCI Core or this Effect model.

## 18. End-to-end examples

### 18.1 Field migration and Reflection

```text
v1: User.name
v2: User.display_name @[migrate(fromVer="1.0.0", fromField="name")]
```

The Migrate Effect produces a canonical `FieldAlias`. A Reflection Effect may then produce a historical reflection alias. A serializer mapping is produced only when explicitly registered. DCE retains a historical entry only while references and exports require it. Two current fields mapping to one historical field produce a conflict.

### 18.2 Owned DCI object

```text
constructor → owned(T, allocator=A)
destructor  → destroy(T, allocator=A)
```

If the Effect graph finds layout and construction but no destruction in the same allocator domain, the DCI Effect rejects Consumer code generation. Exceptions, early returns, and cross-module destruction must consume the same ownership fact.

### 18.3 Open generic

```text
std.vector<T>
```

For `T = i32`, the Effect can use verified layout facts. For an external type, it requests producer-side generic closure, obtains layout, calling, and lifecycle facts, then generates the final contract and call code.

### 18.4 Compiler lowering

```text
@[async] fn load() -> Result<Data, Error>
```

The Async Effect produces Future state facts, an await-lowering request, cancellation and drop obligations, and a Result propagation requirement. It cannot infer network or IO semantics from the name `load`.

## 19. Acceptance and verification

The implementation must eventually cover:

1. Migrate Effect mapping, duplicate mappings, and old-Effect conflicts.
2. Reflection roots and hidden-member behavior.
3. DCI layout, ownership, allocator, destruction, and exception checks.
4. Deterministic `comptime` inputs, outputs, and handler fingerprints.
5. Write-after-freeze, scope mismatch, missing capability, and unfinished obligations.
6. Effect conflicts, cycles, and non-monotonic output diagnostics.
7. Cross-module manifest consumption, stale-artifact rejection, and incremental invalidation.
8. Stage 0 → S1 → S2 → S3 Effect-manifest fixed point.

Verification remains AOT-first. Successful compilation alone is not Effect verification; each project must run the final artifact and record compiler identity, target, and skipped gates.

## 20. Implementation roadmap

### Phase 0: specification and observation

Define Fact, EffectDecl, EffectPlan, Scope, Provenance, Fingerprint, and diagnostics. Add an Effect dump without changing source semantics.

### Phase 1: internal normalization

Normalize `migrate`, `reflect`, `dci_import`, `derive`, `comptime`, `async`, `platform`, and layout attributes into built-in Effects. Keep current stage implementations as compatibility backends.

### Phase 2: core propagation

The AOT baseline now connects Migrate → Reflection, Migrate → DCE, DCI →
lifecycle/exception, and layout → codegen/cache through the Effect ledger. The
cross-module manifest carries typed dependency edges; stale stamps, conflicting
facts, unresolved DCI obligations, and dependency cycles are rejected before
lowering.

### Phase 3: compiler Effects

The current AOT compiler has connected derive, comptime, async/task, platform,
Reflection, DCE, Migrate, source-discovered DCI import authorization, and DCI
export artifact selection to registered handlers. The fixed-point gate compares
the rebuilt compiler and its deterministic cache; the Effect gate checks
manifest freshness, dependency-cycle diagnostics, DCI obligation import, and
lowering evidence. Stub generation remains a separate DCI consumer path.

### Phase 4: standard-library migration

Add facts for collections, strings, Result, async, Reflection, resource management, and DCI adapters. Every public capability enters a real project fixture and an AOT regression.

### Phase 5: restricted user Effects

The first restricted user Effect form is implemented as the validated schema table
described above. It declares typed inputs, a compiler-owned consequence, and the
scope/capability boundary; it does not submit executable compiler plugins. A future
source-level declaration can reuse this wire contract once its syntax and package
resolution are specified.

## 21. Design conclusion

The Effect object is:

```text
verified fact
    → declarative Effect
    → derived facts / obligations / artifacts / lowering / invalidation
```

Migrate handles entity correspondence along the time axis. Effect propagates that relationship through space, runtime, and compile-time. Reflection, DCI, DCE, the standard library, and compiler bootstrap then share one auditable causal model.

Effect is the fifth core feature of the Fact Semantic Ownership System. The current AOT implementation validates registered rules, dependencies, and obligations; source-defined schemas select record, retain, or reflect. Further semantic extensions require their own authority and validation rules.

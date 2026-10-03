# Fact Semantic Ownership System

[简体中文](MOSP_ZH.md) · [Documentation](README.md) · [Getting started](TUTORIAL.md)

Vyx's Fact Semantic Ownership System connects the provenance, meaning, consumption
rules, and validity of program facts. A semantic operation requires the relevant
verified facts and authorized rules. Providers, rule authors, and consumers can
belong to different modules or languages. Target, ABI, toolchain, configuration,
and version determine where a fact applies.

| Question | Place in the system |
|---|---|
| Where do facts come from? | Source declarations, compiler inference, and DCI producers, with provenance and evidence |
| How are facts used? | Check arguments, entity identity, and validity before applying the corresponding rule |
| Who uses them? | Compiler phases, registered handlers, and foreign contract consumers |
| Who manages them? | Providers establish provenance; the compiler maintains dependencies, validity, and invalidation |
| What do they do? | Select implementations, query types, generate calls, establish obligations, and determine code retention |

Ownership here means authority and responsibility over the meaning of facts.
Facts can also constrain memory ownership and cleanup. The five core features are
**Migrate, Reflection, DCI, DCE, and Effect**: Migrate relates facts across versions;
Reflection provides queries and access; DCI transfers facts across languages; DCE
uses facts to decide retention; Effect defines the consequences of consuming them.

Detailed rules are in the [Effect specification](MOSP_EFFECT.md). Existing MOSP
file paths and data format identifiers remain compatible.

## Migrate: module versions and historical calls

Versions and variants identify module revisions. Migration annotations connect current
and historical declarations. This project retains two shipping policies.

```text
shipping/
  Vyx.toml
  Vyx.lock
  src/
    main.vyx
    shipping_v1.vyx
    shipping_v2.vyx
```

`Vyx.toml`:

```toml
[package]
name = "shipping"
version = "0.1.0"
entry = "src/main.vyx"

[build]
output_dir = "target"

[target.shipping]
type = "executable"
entry = "src/main.vyx"
```

`Vyx.lock` selects the default module version:

```text
Shipping:2.0.0["standard"]
```

`src/shipping_v1.vyx`:

```vyx
@[version("1.0.0")]
@[variant("standard")]
module Shipping;

public fn quote(weight_kg: i32) -> i32 {
    return 8 + weight_kg * 2;
}
```

`src/shipping_v2.vyx`:

```vyx
@[version("2.0.0")]
@[variant("standard")]
module Shipping;

@[migrate(fromVer="1.0.0", fromSig=fn quote(i32)->i32)]
public fn quote(weight_kg: i32) -> i32 {
    return 5 + weight_kg * 2;
}
```

`src/main.vyx`:

```vyx
use Shipping;

fn main() -> i32 {
    print(quote(3));
    print(quote@1.0.0["standard"](3));
    return 0;
}
```

Run `vyxc --src=project . --run=aot` in the project directory. It prints 11 and 14.
The unqualified call uses the locked version; the explicit version call uses
the historical implementation.

Use `@[migrate(fromVer="1.0.0", fromField="old_name")]` for field renaming.
The compiler checks historical fields and mappings. This describes program evolution,
not automatic conversion of stored data formats.
See [tutorial_migrate](../tests/projects/tutorial_migrate/src/main.vyx) for fields,
constructors, and historical method calls.

## Reflection: discover, bind, call

`@[reflect]` declares reflection names and aliases; `@[hidden]` excludes members.
`std.reflect` reads compiler-generated metadata to query types and operate on instances.

Save as `reflection.vyx`:

```vyx
use std.reflect;

@[reflect("Counter")]
public class Counter {
    public value: i32;

    @[reflect(alias="read")]
    public fn current() -> i32 {
        return self.value;
    }

    @[hidden]
    public fn reset() {
        self.value = 0;
    }
}

fn main() -> i32 {
    var counter = Counter { value: 42 };
    let counter_type = getType("Counter");
    var instance = counter_type.bind(&counter);
    let method = instance.getMethod("read");
    let read = method.as::<fn()->i32>();
    print(read());
    return 0;
}
```

Run `vyxc --src=file reflection.vyx --run=aot`; it prints 42.

- `getType` looks up a registered name. Check `valid` when names come from external input.
- `bind` attaches to an existing instance without taking ownership; keep the instance alive while using the binding.
- `getMethod` accepts registered aliases; `as::<fn(...) -> R>()` specifies the call signature.
- Field and property operations use `getField`, `getProperty`, `view::<T>()`, and `write::<T>()`.
- Compile-time queries such as `T::name` and `T::fields` are separate from runtime name lookup.

Runtime reflection retains metadata and callable code. Reflection entries participate
in the DCE root set. See [tutorial_reflect.vyx](../tests/cases/tutorial_reflect.vyx)
and [reflection_full_model.vyx](../tests/cases/reflection_full_model.vyx) for more operations.

## DCI: producer facts and open generics

### Responsibilities

```text
Producer source
    ↓ Producer compiler: layout, constraint checking, instantiation
Adapter
    ↓ Closed ABI facts and implementation artifacts
.dcib + object files / libraries
    ↓ Validation, call generation, linking
Vyx program
```

The C++ compiler handles C++ semantics; rustc handles Rust semantics.
Vyx consumes declarations and contracts rather than implementing those source-language type systems.

An Active / Online Adapter accepts instantiation requests during a build.
Open requests belong to the control plane; canonical `.dcib` describes closed entities.
The producer checks constraints and instantiates code; returned artifacts must also
be included in the final link.

The Rust Adapter includes both C/system boundaries and supported rustc-measured native ABI paths.
Those facts are specific to the producer environment. See the [DCI reference](DCI_SPEC.md)
for current support and the [tools guide](../tools/dci/README.md) for generation commands.

### Original definitions and Vyx calls

Rust definition from [native/lib.rs](../tests/projects/dci_opengeneric/native/lib.rs):

```rust
pub fn twice<T: std::ops::Add<Output = T> + Copy>(v: T) -> T {
    v + v
}
```

C++ definition from [native/lib.hpp](../tests/projects/dci_opengeneric/native/lib.hpp):

```cpp
template <typename T>
T twice(T v) {
    return v + v;
}
```

Vyx call sites:

```vyx
@[dci_import("../dci/open_generic.dcib")]
extern "dci" {
    fn twice<T>(v: T) -> T;
}

fn main() -> i32 {
    print(twice(21));
    print(twice(1.5));
    return 0;
}
```

This code requires a matching contract, producer source, and Active Adapter build configuration.
The C++ path uses its own `open_generic.cpp.dcib`; a Rust contract does not describe
the C++ artifact.

### Build the existing project

The [open-generics project](../tests/projects/dci_opengeneric/README.md) contains full configuration:

| Target | Producer | Declarations and calls |
|---|---|---|
| `dci_opengeneric` | Rust, `native/lib.rs` | `src/open_generic.vyx` |
| `dci_opengeneric_cpp` | C++, `native/lib.hpp` | `src/open_generic_cpp.vyx` |

With Python, rustc, and the project's C++ toolchain available, run from that project:

```sh
vyxc build --target dci_opengeneric
vyxc build --target dci_opengeneric_cpp
```

The manifest selects the producer and backend through `dci_stub_backend = "external"`
and `dci_stub_backend_tool_args`. Consult the project README for contract generation,
toolchain setup, and end-to-end execution.

The current project distinguishes two requirements:

- Generic function requests close at call sites and reach the producer through `.dci_open`.
- Generic records such as `Pair2<A, B>` need sizes, alignments, and field offsets when
  Vyx generates code. Required instance layouts must therefore already be in the contract;
  member implementations come from the corresponding producer.

Compatible primitives, DCI-imported types, and supported Vyx C-layout types are candidates
for admission. Each operation must satisfy its layout, calling, lifecycle, and
producer-side generic requirements. There is no universal object-layout conversion,
nor a promise that every type or generic can cross the boundary.

### Original templates and ecosystem projects

The [original vector gate](../probes/gates/dci-vector/README.md) imports the
open C++ `std::vector<T>` template; the Vyx consumer selects `std.vector<i32>`
and constructs it with a brace initializer. The
[C++ ecosystem gate](../probes/gates/dci-cpp-ecosystem/README.md) uses ICU's
original `UnicodeString` API, while the
[Rust ecosystem gate](../probes/gates/dci-rust-ecosystem/README.md) consumes
locked Cargo crates through measured views and generated native bridges.
These are AOT regressions for named operations and targets. Adapter commands,
bridge boundaries, storage release, and shared exception cleanup are documented
in the [DCI SDK guide](../tools/dci/README.md) and [DCI specification](DCI_SPEC.md).
JIT parity remains later work.

### Inheritance and native callbacks

DCI also carries object-model facts. In
[dci_multilang](../tests/projects/dci_multilang/src/main.vyx), `VyxSink` derives
from the C++ `abi_complex.AbstractSink` and overrides `consume`; the native
`NativeDriver` dispatches through that base back into Vyx. In the same project,
`VyxHost` derives from the Rust `native.Sink` contract and Rust's dispatch
calls its Vyx override. These reverse calls require producer-measured vtables
and a matching Stub backend. An open generic trait must first be closed to a
concrete instance; see [dci_rust_trait](../tests/projects/dci_rust_trait/).
The [DCI capability matrix](DCI_SPEC.md#producer-support) records which
producer and target combinations have tests.

## DCE: reachability and required behavior

DCE needs no source annotation. It uses call relationships, export requirements,
reflection registration, and side-effect information:

1. Identify entries and required roots.
2. Follow dependencies and materialize reachable function bodies.
3. Remove code that can safely be eliminated during MIR optimization.
4. Pass retained code to LLVM for optimization and machine-code generation.

A function without a direct caller may still be needed for exports or reflection.
Destruction and observable effects must also be preserved.
Reflection and DCE illustrate Fact Semantic Ownership System in practice: metadata about the same entity
affects discovery, invocation, and code generation.

## Effect: fact consumption and semantic consequences

Effect connects declared facts to registered semantic rules. Rules check declaration
targets, arguments, and capabilities before recording facts, establishing obligations,
or authorizing compiler behavior. Migrate manages version relationships; Effect
defines how those relationships and other facts are consumed, and tracks invalidation.

This project declares two schemas, reuses an `i32` argument with `extends`, and
connects `acme.entry` to the compiler's existing `retain` rule.

`attrs/acme.attr`:

```attr
attribute acme.meta {
    version: 1;
    targets: [function];

    arguments {
        priority: i32 = 0;
    }

    consequence: record;
}

attribute acme.entry {
    version: 1;
    targets: [function];
    extends: [acme.meta];

    arguments {
        reason: string = "native callback";
    }

    consequence: retain;
}
```

`Vyx.toml`:

```toml
[package]
name = "effect_demo"
version = "0.1.0"
entry = "main.vyx"

[effect]
attr_files = ["attrs/acme.attr"]
auto_discover = false
```

`main.vyx`:

```vyx
@[acme.entry(priority=10)]
fn native_entry() -> i32 {
    return 42;
}

fn main() -> i32 {
    print("effect-ready");
    return 0;
}
```

Run `vyxc build`, then execute the generated program to print `effect-ready`.
`native_entry` has no direct call but becomes a retention root. `priority` is typed
attribute data; its name does not change scheduling.

Package-root `.attr` files are discovered by default. List other files in
`[effect].attr_files`, relative to the manifest. `auto_discover = false` loads only
that list. Selected schema content is a cache input; edits require validation again.

Current `.attr` consequences are `record`, `retain`, and `reflect`. Arguments use
Vyx scalar names, defaults, and `enum("a", "b")`. Duplicate identities, inheritance
cycles, conflicting arguments, and out-of-range values are rejected. Executable
handlers, layout changes, and transfers of fact authority require corresponding
compiler rules and validation.

See the [project manifest](PACKAGE_MANIFEST.md), [Effect specification](MOSP_EFFECT.md),
and complete [attribute project](../probes/gates/mosp-effect/fixtures/attribute_project/main.vyx).

## Further reading

- [DCI specification](DCI_SPEC.md): contracts, capabilities, and validation.
- [Project manifest](PACKAGE_MANIFEST.md): targets, versions, and build configuration.
- [Advanced features](ADVANCED_FEATURES.md): ownership, generics, inheritance, and compile-time features.
- [Compiler architecture](COMPILER.md): where HIR, MIR, LLVM, and CGUs use these facts.
- [Effect model](MOSP_EFFECT.md): fact propagation, compiler Effects, and implementation phases.

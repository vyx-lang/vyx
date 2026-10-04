<div align="center">
  <img src="vyx.png" width="128" height="128" alt="Vyx logo">
  <h1>Vyx</h1>
  <p>
    <a href="README.md">English</a> ·
    <a href="README.zh-CN.md">简体中文</a> ·
    <a href="https://www.vyxlang.com/">Website</a> ·
    <a href="https://github.com/vyx-lang/vyx/releases/latest">Download</a> ·
    <a href="docs/README.md">Documentation</a>
  </p>
</div>

Vyx is a systems programming language that compiles to native machine code.
Its compiler is written in Vyx and uses LLVM 22. Native AOT is the current
build and testing baseline.

The language has structs, classes, enums, generics, traits, and closures.
Memory management includes value semantics, borrows, pointers, and scope cleanup.
Projects use `Vyx.toml` to configure sources, libraries, and build targets.

Vyx's Fact Semantic Ownership System connects program facts to their provenance,
validity, and consumption rules. Its five core features are Migrate, Reflection,
DCI, DCE, and Effect.

## Run your first program

Download an SDK from [Releases](https://github.com/vyx-lang/vyx/releases/latest),
extract it, and add its `bin` directory to `PATH`. The Windows and Linux SDKs
both bundle the LLVM backend and native linking tools. Keep the SDK directory
intact; no separate LLVM installation is needed to use the SDK.
See [Getting started](docs/TUTORIAL.md) for installation.

Save this as `hello.vyx`:

```vyx
fn main() -> i32 {
    print("Hello, Vyx!");
    return 0;
}
```

Compile and run:

```sh
vyxc --src=file hello.vyx --run=aot
```

For multiple source files, dependencies, and library linking, see the
[project guide](docs/PROJECTS.md). Building the compiler from source has
additional development dependencies; see the [compiler README](bootstrap_compiler/README.md).

## Learn Vyx

| Guide | Topics |
|---|---|
| [Getting started](docs/TUTORIAL.md) | SDK installation, bindings, control flow, functions, structs, classes, and visibility |
| [Intermediate tutorial](docs/INTERMEDIATE_TUTORIAL.md) | Containers, closures, generics, `match`, errors, and multi-file projects |
| [Advanced features](docs/ADVANCED_FEATURES.md) | Ownership, traits, concurrency, reflection, and interoperation |
| [Coming from Rust or C++](docs/MIGRATING_FROM_RUST_CPP.md) | Syntax and project conventions compared side by side |
| [Standard library](docs/STD_LIBRARY.md) | Package paths, APIs, and implementation boundaries |

The [tutorial website](https://www.vyxlang.com/tutorial/) renders the four language guides from
these documents, with chapter navigation and Chinese / English switching.

### Visibility with `@[vis]`

Use `@[vis(scope)]` to control access to a declaration. `public` is shorthand
for `@[vis(world)]`; module and package boundaries use the same attribute.

```vyx
module counter.api;

@[vis(world)]
fn initial_value() -> i32 { return 41; }

@[vis(package)]
fn normalize(value: i32) -> i32 { return value < 0 ? 0 : value; }
```

`world` allows all callers; `package` allows callers with the same first
module-path segment. Use `mod`, `tree`, `in(...)`, and `friend(...)` for more
specific scopes, and combine scopes with `+`, `&`, and `-`. The
[tutorial](docs/TUTORIAL.md) explains the full scope table, defaults, and member access.

## Use C++ and Rust libraries

DCI (Declarative Code Interface) imports types and functions from other languages.
An adapter obtains layouts, calling conventions, and construction/destruction
information from the original language's compiler and writes a `.dcib` contract.
Vyx uses that contract to generate calls and link native libraries. For supported
generics, the adapter can ask the original compiler to instantiate them during the build.

For example, after setting up the DCI declarations and build, Vyx can use the
C++ standard library's `std::vector<T>`:

```vyx
var numbers = std.vector<i32>{1, 2, 3, 4, 5};
let value: i32 = 6;
numbers.push_back(&value);
```

The Vyx caller chooses the type argument. The [vector example](probes/gates/dci-vector/README.md)
contains the full declarations and build steps.

Tests against existing libraries cover ICU's `UnicodeString` and the Cargo crates
`crc32fast` and `adler2`. The adapter generates the call bridges needed by
these Rust examples. See the [DCI specification](docs/DCI_SPEC.md) for
supported platforms and types, and the [DCI tools guide](tools/dci/README.md)
for contract generation.

## Fact Semantic Ownership System: five core features

A semantic operation requires the relevant verified facts and authority to consume
them. Facts come from source declarations, compiler inference, or foreign producers.
The compiler checks their target, ABI, version, and dependencies before applying the
corresponding rules. Fact semantic ownership identifies who supplies a fact, who
defines its meaning, who uses it, and when changed conditions require validation again.

- **Migrate**: use `@[version]`, `@[variant]`, and `@[migrate]` to describe
  module and API version relationships. Callers can select a historical
  implementation when the project provides that version's module.
- **Reflection**: query types at compile time, or register types and members
  with `@[reflect]` for runtime lookup. Use `std.reflect` to access properties,
  bind methods, and call them.
- **DCI**: the original language's producer supplies facts about layouts, calling
  ABIs, lifecycles, and generic instances. Vyx verifies the contract, generates
  calls, and links the implementation.
- **DCE**: the compiler follows dependencies from entries, exports, and
  reflection registrations, removing unused code while retaining side effects
  and required cleanup.
- **Effect**: registered rules consume facts to record information, establish
  compilation obligations, and produce code generation consequences. Projects
  define typed attributes in `.attr` files and load them through `Vyx.toml`.

For example, an attribute schema can connect a function declaration to `retain`,
making it a code retention root. Arguments use types such as `i32`, `bool`, and
`string`. One file can contain several schemas, with `extends` reusing arguments.
Current `.attr` consequences are `record`, `retain`, and `reflect`.

See the [Fact Semantic Ownership System](docs/MOSP.md),
[Effect and attribute definitions](docs/MOSP_EFFECT.md), and
[project configuration](docs/PACKAGE_MANIFEST.md). Documentation paths and
contract format identifiers remain compatible.

## Tools and UI development

`vyxc` includes project creation, builds, tests, and formatting. The repository
also contains `vyxc-lsp`, `vyxc-dap`, and plugins for VS Code / Cursor and
IntelliJ IDEA / CLion. See [tooling](docs/TOOLING.md) for setup.

[Zyn](Zyn/README.md) is a UI framework written in Vyx. It composes interfaces
from views and updates state through application resources and commands.
It has layout containers, MD3 controls, themes, shadows, and stable keyed lists.
Text layout and more complex list components are still being developed.
Runnable projects are in [samples](samples/README.md).

## Project status

Vyx is in Early Access at `1.0.0-alpha.1`. Development and testing currently
use LLVM AOT, with executable, object, and library output. Compile times and
large-project stability are still being improved. JIT parity is later work.

Compiler sources are in [bootstrap_compiler/](bootstrap_compiler/README.md).
After changing the compiler, rebuild the SDK compiler and run hello, the self-host
fixed point, and the relevant project tests. Commands are in the
[verification guide](docs/TESTING_GUIDE.md); parallel build measurements are
in the [compiler pressure tests](probes/gates/compiler-industrial/README.md).

## License

Vyx is dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE),
at your option (`MIT OR Apache-2.0`). Third-party code and assets retain their
own licenses and copyright notices. See [LICENSE](LICENSE).

More: [Documentation index](docs/README.md) · [Contributing](CONTRIBUTING.md)

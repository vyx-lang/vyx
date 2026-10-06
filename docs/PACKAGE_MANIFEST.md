# Vyx.toml Package Manifest

[简体中文](PACKAGE_MANIFEST_ZH.md) · [Project tutorial](PROJECTS.md) · [Documentation index](README.md)

> **Documentation status (2026-09-02):** current self-host implementation reference.
> The package/source-target, target-selection and triplet-link features below
> describe the self-host implementation in
> `bootstrap_compiler/src/core/build_system.vyx`. The frozen host toolchain does
> not consume self-host-only source packages.

## Scope and authority

For a project creation and build walkthrough, use the [project guide](PROJECTS.md).
This page defines manifest fields, target selection, and dependency resolution.

`--target` selects a manifest target and builds only its target-dependency
closure. Legacy `depends_on` edges remain accepted as build-only edges.
`--triplet` selects the LLVM/backend target. Omitting `--target` retains the
manifest-wide build used by compiler bootstrap projects.

`vyxc --src=project <dir>` reads that directory's `Vyx.toml` and uses the same
builder as `vyxc build`, the shorthand for the current directory. Target selection,
dependencies, hooks, DCI contracts, optimization and parallel compilation options
are shared. `--src=file <path>` compiles an individual file.

`vyxc --run=aot --src=project <dir>` selects one executable and builds its dependency
closure before running it. With no `--target`, it prefers an executable named
after the package, then a unique executable; ambiguous selections are errors.
Library and cross-platform targets cannot be launched this way. Arguments after
`--` belong to the program and do not change build options.

`--emit=ir` publishes the selected target's Vyx LLVM modules as `<output_dir>/<target>.ll`;
`--emit=obj` publishes one object or a `<target>.objects` directory when several
objects are needed. Dependencies are built normally; the selected target is not
linked into an executable. `-o <path>` overrides the artifact path. Paths supplied
for `-o` are relative to the caller; manifest paths are relative to the project.
Select `--target` when the manifest has multiple targets and no package-named target.
These output modes cannot be combined with `--run=aot` or `--artifact-file`.

`vyx publish`, `vyx install`, `vyx search`, and `vyx lock` talk to a local
`file://` registry (`VYX_REGISTRY`, default `./.vyx-registry`). `publish`
copies the current package into `pkgs/<name>/<version>/` and updates
`index.txt`. `install name[@ver]` materializes that tree into
`.cache/registry/<name>/<version>/` and writes `Vyx.lock`. A dependency may
use `version = "1.2.3"` or `path = "registry:name@1.2.3"`; the build resolves
those from the cache or the registry root. There is no remote HTTP registry.

## Supported Sections

### `[package]`

```toml
[package]
name = "app"
version = "0.1.0"
entry = "src/main.vyx"
```

Recognized keys:

| Key | Meaning |
|---|---|
| `name` | Package or default target name |
| `version` | Package version string |
| `entry` | Default entry `.vyx` file |

If no explicit target section exists, the build system creates one executable
target using `name` and `entry`.

### `[build]`

```toml
[build]
threads = 8
output_dir = "target"
cache_dir = ".cache"
prebuild = "echo preparing"
postbuild = "echo done"
```

Recognized keys:

| Key | Meaning |
|---|---|
| `threads` | Parallel compile worker count; defaults to hardware concurrency |
| `output_dir` | Default output directory for targets |
| `cache_dir` | Build cache directory; defaults to `.cache` |
| `prebuild` / `pre` | Command(s) run before dependency/target builds |
| `postbuild` / `post` | Command(s) run after all target builds |

`prebuild` / `postbuild` accept either a single command string or a TOML array
of command strings. Each command is executed directly by the compiler runtime;
no shell wrapper is inserted. A command prefixed with `vyx:` is run as a Vyx
script through the current compiler, so build steps can be written in Vyx and
use `std.fs`, `std.process`, and the rest of the standard library.

### `[effect]`

```toml
[effect]
attr_files = ["attrs/common.attr", "../shared/layout.attr"]
auto_discover = true
```

| Key | Meaning |
|---|---|
| `attr_files` | Additional `.attr` schema files, resolved relative to this manifest |
| `auto_discover` | Load package-root `.attr` files; defaults to `true`; `false` uses only the explicit list |

A file may contain multiple `attribute package.name { ... }` schemas. `extends`
reuses parameters across the selected files, including forward references. Parameter
types use Vyx scalar names such as `i32`, `u64`, `f32`, `bool`, and `string`;
`int` is rejected in `.attr` source. See the [Effect schema format](MOSP_EFFECT.md#142-declaring-schemas-in-attr-files).

Selected paths and file contents participate in object/interface cache identity,
including explicit files outside the package directory. Subdirectory files are loaded
only when listed. `VYX_EFFECT_SCHEMAS_IN` remains a compatibility input for canonical
schema tables; duplicate identities across that table and `.attr` files are errors.

### `[template.<name>]`

Reusable target configuration is declared with standard TOML templates:

```toml
[template.base]
parallel_vyx = true

[template.base.dependencies]
core = "std:core"

[template.base.link]
libs = ["base_runtime"]
lib_paths = ["vendor/common/lib"]

[template.application]
extends = ["base"]
auto_sources = false

[target.app]
extends = ["application"]
type = "executable"
entry = "src/main.vyx"

[target.app.link]
libs = ["app_extra"]
libs_remove = ["base_runtime"]
```

Templates may extend other templates. A target may extend multiple templates;
they are applied left to right. Shared ancestors are materialized once, missing
templates are diagnosed, and inheritance cycles are rejected before compiling.

The effective target is merged in this order:

1. matching `[build]` defaults;
2. package-global `[dependencies]`;
3. inherited templates, left to right;
4. the concrete target;
5. `*_remove` filters.

Scalars are overridden by the later layer. Dependency aliases are overridden
by the later layer. Arrays append in stable order and are de-duplicated.
`dependencies_remove` removes aliases; `libs_remove` and `lib_paths_remove`
remove final native-link entries. Vyx.toml deliberately does not invent
non-standard `+=` / `-=` syntax: templates plus final removal arrays express
shared configuration and small per-target differences with valid TOML.

### `[target.<name>]`

```toml
[target.app]
extends = ["application"]
type = "executable"
entry = "src/main.vyx"
sources = ["src/helper.vyx", "native.c"]
link_order = ["support", "vyx_runtime"]
output_dir = "target"
prebuild = "echo target prebuild"
postbuild = "echo target postbuild"
precompile = "vyx: hooks/precompile.vyx"
postcompile = "vyx: hooks/postcompile.vyx"
prelink = "echo before link"
postlink = "echo after link"

[target.app.dependencies]
support = { target = "support", usage = "private" }

[target.app.link]
libs = ["vyx_runtime", "user32"]
lib_paths = ["bootstrap_compiler/out"]
```

Recognized keys:

| Key | Meaning |
|---|---|
| `type` | `executable`, `shared` / `dylib`, `static` / `staticlib`, or `source` |
| `entry` | Target entry `.vyx` file |
| `sources` | Extra `.vyx`, `.c`, `.cc`, `.cpp`, or `.cxx` sources |
| `extends` | Ordered target-template names |
| `libs` | Legacy flat spelling for native/system/prebuilt `-l` entries |
| `lib_paths` | Legacy flat spelling for native/system/prebuilt `-L` entries |
| `public_libs` | Native link libraries propagated to package consumers |
| `public_lib_paths` | Consumer link search paths, resolved from this package root |
| `dependencies_remove` | Dependency aliases removed after all layers merge |
| `depends_on` | Deprecated compatibility spelling for build-only target edges |
| `link_order` | Explicit link ordering metadata |
| `output_dir` | Per-target output directory |
| `prebuild` / `pre` | Target-specific prebuild command(s) |
| `postbuild` / `post` | Target-specific postbuild command(s) |
| `precompile` | Command(s) run before the compile phase for the target |
| `postcompile` | Command(s) run after the compile phase for the target |
| `prelink` | Command(s) run immediately before linking |
| `postlink` | Command(s) run after linking succeeds |
| `dci_file` | One DCI Descriptor (`.dcib`) supplied to Vyx compilation for this target; combined with `dci_files` and `@[dci_import]` into an independent contract list |
| `dci_files` | Array spelling of additional `.dcib` contracts; each file is consumed independently (C++ stays C++, Rust stays Rust). Do not merge them into one mixed document |
| `dci_stub_backend` | `direct`, `external`, `clang-cpp`, `rustc`, `auto`, or a comma list such as `clang-cpp,rustc`; target value overrides `[build]`; omitted means `direct`. `auto` runs `clang-cpp` then `rustc`, each against the same independent contract list |
| `dci_stub_capabilities` | Optional array of capabilities required from the selected Stub backend; build rejects a capability the backend does not advertise |

The self-hosted build system compiles Vyx sources through the compiler pipeline
and native C/C++ sources through the selected native toolchain. It caches build
artifacts under `cache_dir` (default `.cache`) and links the target.
Target dependencies and legacy `depends_on` are validated as one DAG; missing
targets and cycles are diagnosed before compilation.

`[target.<name>.link]` is the canonical link table. The flat target keys remain
compatible. Both participate in the same triplet merge, and nested link entries
are applied after flat entries at each layer. `libs` / `lib_paths` are only for
bare native, system, or prebuilt libraries; Vyx targets belong in the target's
dependency table and are linked automatically from their type/manifest.

`type = "source"` defines a source package. It produces no archive. Its entry
and declared sources are exposed to consumers as explicit module sources;
generic bodies can be instantiated in the consuming target.
Compiled user generic modules have another route: generated `.vyi` files carry
versioned template artifacts for consumer instantiation, including private
definition dependencies. Link the producer's native library for its ordinary
compiled definitions. This behavior is covered by the
[generic interface gate](../probes/gates/generic_interfaces/README.md).

#### Triplet-specific link settings

`libs`, `lib_paths`, `public_libs`, and `public_lib_paths` are cumulative. The
build system appends every key that matches the effective target, from the
portable base through the most specific OS/architecture/toolchain/linkage key:

```toml
[target.codec]
type = "static"

[target.codec.link]
libs = ["codec_core"]
libs_windows = ["bcrypt"]
libs_windows_x64 = ["codec_simd_x64"]
libs_windows_x64_msvc = ["codec_msvc"]
libs_windows_x64_msvc_static = ["codec_static_support"]

lib_paths = ["native/common"]
lib_paths_windows_x64_msvc_static = ["native/prebuilt/win-x64-static"]
```

For that target all five matching `libs` arrays are linked in declaration
specificity order; a specialized key never replaces the portable list. Stable
OS names are `windows`, `linux`, `android`, and `macos`; stable architecture
names include `x64`, `x86`, `arm64`, and `arm`. Toolchain names are derived
from the LLVM triple (`msvc`, `mingw`, `gnu`, `musl`, `ndk`, `clang`, or the
triple's environment token). The final `_static` / `_shared` layer applies to
library targets. The legacy `_apple` OS key remains additive for compatibility;
new manifests should use `_macos`.

Link search paths are also auto-indexed from an optional package-local tree.
Every existing matching level is indexed automatically, from the most-specific
directory back to the generic `libs/` fallback:

```text
libs/
  windows/
    x64/
      msvc/
        static/
        shared/
  linux/
    arm64/
      gnu/
```

This tree removes repeated `lib_paths_*` declarations while preserving explicit
manifest paths as additional search roots. Static package dependencies propagate
their matching public/native link closure to consumers.

#### Cross compilation and target runtimes

`--triplet` is forwarded consistently to MIR/LLVM emission, C/C++ compilation,
the archiver and the final Clang link. Android targets discover the host NDK from
`ANDROID_NDK_ROOT`, `ANDROID_NDK_HOME`, `NDK_HOME`, or the newest NDK below the
configured Android SDK; `--sysroot` remains an explicit override. A missing NDK
sysroot is diagnosed before compilation. NDK `clang++` would otherwise always
link `libc++_shared`; Android Vyx links with `-nostdlib++` by default. Opt in
with `libs_android = ["c++_shared"]` (or `"c++_static"`), C++ sources / DCI C++
stubs, or `VYX_ANDROID_LIBCXX=shared|static`. Set `VYX_ANDROID_LIBCXX=none`
(or `0` / `off`) to suppress the auto-add even when C++/DCI is present. Android
does not add `-lpthread` (pthread lives in Bionic `libc`).

Generated Vyx executables also need a runtime built for the same triplet. Cross
runtimes are kept separate from host artifacts in the compiler-adjacent tree:

```text
runtimes/<os>/<arch>/<toolchain>/
```

For example, from `bootstrap_compiler/`:

```powershell
./out/vyxc.exe build --target vyx_runtime --triplet aarch64-linux-android23
```

produces the Android runtime under
`out/runtimes/android/arm64/ndk/`. A subsequent project build with the same
triplet discovers it automatically. `VYX_RUNTIME_ROOT` may point either at a
runtime leaf directory or at an SDK root containing the `runtimes/` tree. The
linker rejects a missing target runtime up front instead of continuing to an
undefined-symbol failure or accidentally using a host/other-architecture
runtime.

### DCI Target Options

For C++ / Rust open-generic calls, the current project integration uses the
`external` backend and a producer source passed to `stub_backend.py`. Function
requests are materialized during the build; generic record instance layouts
must be in the contract before Vyx code generation. See the
[open-generics manifest](../tests/projects/dci_opengeneric/Vyx.toml) and
[DCI tools guide](../tools/dci/README.md) for the complete setup.

`dci_file` takes precedence over `dci_files`; both permit environment-variable
expansion and are resolved to absolute paths. Multiple `@[dci_import]` paths
are unioned with the manifest list. Each `.dcib` stays an independent contract:
the Consumer loads them side by side and does not rewrite `source.language` or
fuse layouts. Descriptors are target inputs, not a package-wide fallback: set
them on the target that compiles the relevant Vyx sources. Project builds
consume **only** `.dcib`.

`dci_stub_backend` may be set on `[target.<name>]` or as a default under
`[build]`. The default is always `direct`; the build does not infer
`clang-cpp` or `rustc` from `extern "cpp"`, `@[cpp_include(...)]`, source
extensions, or a Descriptor's `source.language`. `direct` emits no Stub;
`clang-cpp` emits C++ stubs from C++ contracts; `rustc` emits Rust stubs from
Rust contracts; `auto` (or `clang-cpp,rustc`) runs both backends independently
against the same contract list. `external` runs the generic emit/compile
backend protocol.

For `external`, `VYX_DCI_STUB_BACKEND_TOOL` is required and
`VYX_DCI_STUB_SOURCE_EXTENSION` is required when a Stub is generated.
`VYX_DCI_STUB_BACKEND_TOOL_ARGS`, `VYX_DCI_STUB_BACKEND_DEPENDENCIES`, and
`VYX_DCI_STUB_BACKEND_VERSION` are optional. Capabilities come from
`VYX_DCI_STUB_BACKEND_CAPABILITIES`; when it is unset, external exposes only
`emit-source` and `compile-object`. Put every script/configuration dependency
that affects output in `VYX_DCI_STUB_BACKEND_DEPENDENCIES`; missing declared
dependencies fail the build and their contents participate in cache invalidation.

The same six settings may be declared in the manifest instead, under
`[target.<name>]` or `[build]`, as `dci_stub_backend_tool`,
`dci_stub_backend_tool_args`, `dci_stub_backend_dependencies`,
`dci_stub_backend_version`, `dci_stub_backend_source_extension` and
`dci_stub_backend_capabilities`. Resolution order is target section, then
`[build]`; an environment variable that is set always wins, because it is the
explicit override. Relative paths are resolved against the project directory,
and path-looking tokens in `dci_stub_backend_tool_args` are resolved and quoted
the same way while flags and bare words are left alone. A tool name without a
path separator still goes through PATH/LLVM_ROOT lookup; a value that is a path
is used verbatim and never gains a `.exe`.

Example:

```toml
[target.rust_stub]
type = "executable"
entry = "src/stub_backend.vyx"
dci_file = "dci/rust_stub_backend.dci"
dci_stub_backend = "external"
dci_stub_capabilities = ["emit-source", "compile-object"]
```

### `[scripts]`

```toml
[scripts]
tri = "vyx: scripts/triangle.vyx"
```

`vyx run <script>` and `vyx run <package:script>` execute entries from this
table. A script value may be a single command string or a string array. Commands
prefixed with `vyx:` are launched through the current compiler as Vyx scripts,
so package scripts can be pure Vyx code instead of shell wrappers.

### Global and target-local dependencies

```toml
[registries]
local = "file:///C:/packages"

[dependencies]
support = "../support@main"

[target.app.dependencies]
json = { path = "std:json", target = "std_json" }
helper = { target = "helper", usage = "private" }
codegen = { target = "schema_codegen", usage = "build" }
```

`[dependencies]` is global and applies to every target. A
`[target.<name>.dependencies]` table belongs only to that target. If a local
table repeats a global alias, the local entry overrides it. Nested dependency
tables are not build targets. Inherited template dependency tables participate
between those layers, and `dependencies_remove = ["alias"]` is applied to the
final alias map.

The build command recursively builds local path dependencies. The dependency
key is an alias used by the consuming project; it is not assumed to be the
target or library name. A dependency may select an explicit target:

```toml
[dependencies]
json = { path = "std:json", target = "std_json" }
```

When `path` is omitted, `target` names another target in the same manifest:

```toml
[target.app.dependencies]
engine = { target = "engine", usage = "private" }
asset_codegen = { target = "asset_codegen", usage = "build" }
```

`usage` defaults to `private`:

| Usage | Meaning |
|---|---|
| `private` | Build and consume the target according to its type (source/VYI/link/runtime) |
| `public` | Same as `private`, and propagate it through this target's consumer contract |
| `build` | Build-order edge only; no source, VYI, link, or runtime injection |

Target type controls automatic consumption:

| Dependency target type | Consumer behavior |
|---|---|
| `source` | Inject exposed Vyx sources into the consuming compilation |
| `static` / `staticlib` | Load VYI metadata and link the archive automatically |
| `shared` / `dylib` | Load VYI metadata, link it, and copy runtime assets |
| `executable` | Allowed only with `usage = "build"` |

This replaces hand-written `depends_on + lib_paths + libs` triples. Existing
`depends_on` declarations continue as build-only compatibility edges.

`std:<name>` is resolved from `VYX_STD_PACKAGES`, then the SDK/compiler-adjacent
`std_packages/` directories (in this source tree: `bootstrap_compiler/std_packages/`).
The dependency's own manifest supplies the
effective target name, library type, output directory, generated `.vyi` root,
link path, and runtime asset directory. The consumer must not repeat
`sources`, `lib_paths`, `libs`, or copy DLL/SO files by hand. A package with
multiple targets must specify `target`; executable targets cannot be consumed
as library dependencies unless their usage is `build`.

Static packages that require native libraries expose them with target-level
`public_libs` and `public_lib_paths` (or use the target's `libs` and `lib_paths`
as the default). Those entries are propagated through the dependency graph and
are resolved relative to the package, so a consumer still has no hand-written
link flags. An explicitly empty public array suppresses the corresponding
static-target default:

```toml
[target.codec]
type = "static"
entry = "src/codec.vyx"

[target.codec.link]
lib_paths = ["native/lib"]
libs = ["codec_native"]

# Optional when the private link settings are also the public contract.
public_lib_paths = ["native/lib"]
public_libs = ["codec_native"]
```

Local path dependencies still do not fetch HTTP archives or authenticate to a
hosted registry. Versioned dependencies are resolved from the `file://`
registry / `.cache/registry` tree written by `vyx install` / `vyx publish`.

When a target resolves one or more source packages, the compiler disables the
bootstrap-private implicit flat standard-library search for that target. Every
standard module it uses must therefore be reachable through global or local
manifest dependencies. Legacy manifests without source-package dependencies
retain compatibility while they migrate.

## Complete multi-target example

`tests/projects/manifest_target_templates/` is a buildable reference project.
It demonstrates template-to-template inheritance, two application targets with
small dependency/link differences, global/template/target alias overrides,
dependency and link removals, a same-manifest static dependency, a public
transitive dependency, a build-only executable tool, and target-scoped selection.

```powershell
cd tests/projects/manifest_target_templates
# Use the SDK compiler freshly built from the current source tree.
$compiler = (Resolve-Path ../../../bootstrap_compiler/out/vyxc.exe).Path
& $compiler build --target app_a
./target/app_a.exe

# Or run both focused checks and automatic artifact cleanup:
./run.ps1 -Compiler $compiler
```

## List Syntax

The parser accepts both comma-separated values and bracketed string lists for
target list fields:

```toml
sources = "src/a.vyx, src/b.vyx"
libs = ["user32", "SDL3"]
```

Values are trimmed and surrounding quotes are removed.

## Preparing declared C++ DCI exports

`dci_imports = ["name"]` in a build target enables import preparation before
source discovery. `[dci.import.name]` supplies `module`, `headers`, explicit
`export_types` / `export_functions` (or `export_all = true`), optional
`profile` (`cpp` or `qt`), `project_roots`, `triplet`, `boundary`, `std` and
`toolchain`, `contract` and `definitions`. Native `cxx`, `cxxflags` and `include_paths` come from the target
and its build defaults.

The compiler invokes the SDK DCI tools before source discovery. `contract`
defaults to `contracts/name.dcib`, outside `.cache`. Optional `definitions`
selects an existing maintained Vyx file, which is never overwritten; otherwise
the compiler adds `.cache/dci/name/import.vyx`. Both modes add prepared native
`producer.cpp`. An existing offline contract is validated and reused without
AST extraction or rewriting it. Deleting the cache rebuilds disposable artifacts
from that contract. Missing contracts and known changes to producer inputs
recorded in `source.preparation_inputs` trigger measurement; invalid or
incompatible supplied contracts fail. `cpp-import --force` explicitly requests
regeneration. Without preparation provenance, the provider manages contract
version freshness. Do not maintain
cache files. Standalone preparation and editable Converter output remain
available. Application usage never selects exports; all representable members
of selected classes remain available. `ownership_headers` explicitly selects
authored library lifetime facts, and unknown ownership is rejected.
`qt_connections` lists exact optional signal-connection operations; a signal or
inline definition alone does not select an operation.
See [the DCI tools](../tools/dci/README.md#optional-build-preparation-for-declared-exports)
and [the Qt project](../probes/gates/dci-qt-counter/README.md).

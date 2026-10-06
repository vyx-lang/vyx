# Create and configure a Vyx project

[简体中文](PROJECTS_ZH.md) · [Documentation](README.md) · [Getting started](TUTORIAL.md) · [Manifest reference](PACKAGE_MANIFEST.md)

Install the SDK and confirm that `vyxc --version` works.
This guide creates a project, adds files, and changes its build configuration.

## 1. Create, build, run

```sh
vyxc new hello_app
cd hello_app
vyxc build --target hello_app
vyxc --run=aot --src=project . --target hello_app
```

The second command builds and runs the selected target using its configured
output path. You can also launch the artifact separately. Windows:

```powershell
.\target\hello_app.exe
```

Linux:

```sh
./target/hello_app
```

The program prints `Hello, Vyx!`. The generated files are:

```text
hello_app/
  Vyx.toml
  src/
    main.vyx
```

Building creates `target/` and `.cache/`.
Use the default template for ordinary applications; `--bare` omits the default
standard-library dependencies.

## 2. Understand the generated manifest

`Vyx.toml`:

```toml
[package]
name = "hello_app"
version = "0.1.0"
entry = "src/main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
threads = 0

[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }

[target.hello_app]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
```

| Setting | Meaning |
|---|---|
| `package.name` / `version` | Package identity |
| `package.entry` | Default entry |
| `build.output_dir` | Output directory |
| `build.cache_dir` | Compilation cache |
| `build.threads` | Concurrent jobs; 0 uses hardware concurrency |
| `dependencies` | Dependencies shared by package targets |
| `target.hello_app` | Build target named `hello_app` |
| `target.entry` | Entry for that target |
| `auto_sources = false` | Explicit source configuration |

`--target hello_app` selects a manifest target. `--triplet` selects a platform;
without it, the compiler builds for the host.
Omitting `--target` builds the manifest's target set rather than selecting just one entry.

`--run=aot --src=project .` builds and runs one executable target. It prefers an executable
with the package name, otherwise the only executable target. Multiple candidates
require `--target`; library targets cannot run. Dependencies, DCI stubs, caching
and build hooks use the same manifest builder. Failed builds never launch an old
artifact. The command returns the program's exit code. Cross-compiled outputs
are not automatically launched on the host.

Put program arguments after `--`:

```sh
vyxc --run=aot --src=project . --target hello_app -- "two words" --config app.json
```

`vyxc build` is shorthand for `vyxc --src=project .`. Every project selector uses
the manifest pipeline, including `vyxc --emit=ir --src=project . --target hello_app`.
IR output uses the target's output directory, dependencies and DCI contracts.
`--src=file` compiles an individual file; manifest scripts use `vyxc run <script>`.
Debug integrations can use `vyxc build --target hello_app -g -O0 --artifact-file launch.txt`.
After a successful build, `launch.txt` contains the executable's absolute path in UTF-8.

## 3. Add a source file

Create `src/math.vyx`:

```vyx
module hello_app;

public fn square(value: i32) -> i32 {
    return value * value;
}
```

Replace `src/main.vyx` with:

```vyx
module hello_app;

fn main() -> i32 {
    print(square(6));
    return 0;
}
```

Add `sources` to the existing target section:

```toml
[target.hello_app]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
sources = ["src/math.vyx"]
```

Rebuild with `vyxc build --target hello_app` and run the program; it prints 36.
Both files belong to the same module; the function used across files is `public`.
With explicit sources, placing a file in `src/` does not add it to a target.
Edit the existing TOML section rather than declaring the same section twice.

### Import a separate module

Use `module` to give each module a name and `use` to import it. Export the
items consumers need with `public`, including enums, constants, and mutable
globals. For example, an enum can be imported with
`use Status = Cross.GlobalEnum.Status;` and constructed as `Status::Ready()`.
The [cross-module project gate](../probes/gates/cross-module/README.md)
shows the complete producer, consumer, interface generation, and native link
for an enum and integer/string globals.

## 4. Change output, concurrency, and optimization

Replace the existing `[build]` section, for example:

```toml
[build]
cache_dir = ".cache"
output_dir = "out"
threads = 4
```

```sh
vyxc build --target hello_app -j4
vyxc build --target hello_app -O2
```

Run `out/hello_app.exe` on Windows or `out/hello_app` on Linux.
A target's `output_dir` overrides the global value; command-line `-j` overrides
the default job count.

For a debug build:

```sh
vyxc build --target hello_app -g -O0
```

Point the IDE debugger at the executable just built. See [Editors and debugging](TOOLING.md).

## 5. Add another target

Create `src/report.vyx`:

```vyx
module report;

fn main() -> i32 {
    print("report");
    return 0;
}
```

Append a target with a different name:

```toml
[target.report]
type = "executable"
entry = "src/report.vyx"
auto_sources = false
```

```sh
vyxc build --target report
```

This produces `out/report.exe` or `out/report`, using the output directory set above.
Other target types include `static`, `shared`, and `source`.
A `source` target exposes sources to consumers, including generic bodies that
need consumer-side instantiation.
Compiled user generic modules can also expose bodies through generated `.vyi`
template artifacts. Consumers still need the producer's native library for
ordinary compiled definitions. The [generic interface gate](../probes/gates/generic_interfaces/README.md)
demonstrates consumption with producer source hidden, qualified generic calls,
private helper dependencies, and repeated instantiations from two consumers.

## 6. Dependencies and native libraries

The generated project declares `std:core` and `std:collections`, for example:

```vyx
use std.collections;
```

Declare a dependency globally under `[dependencies]`, or for one target:

```toml
[target.hello_app.dependencies]
support = { path = "../support", target = "support" }
```

This requires `../support/Vyx.toml` with a target named `support`.
Paths are relative to the project directory; `target` selects a target in the
dependency package. A target in the same manifest can be referenced with
`support = { target = "support", usage = "private" }`.

For a prebuilt native library, supply its actual name and directory:

```toml
[target.hello_app.link]
libs = ["native_support"]
lib_paths = ["vendor/lib"]
```

`libs` contains native library names, not Vyx target names. Connect Vyx targets
through dependency tables. DCI contracts must also match implementation artifacts;
see the [DCI tools guide](../tools/dci/README.md).

Registry operations currently use a local `file://` registry.
See the [manifest reference](PACKAGE_MANIFEST.md) for package retrieval and locking;
do not assume automatic downloads from a remote package service.

## 7. Files to commit

Commit `Vyx.toml`, sources, and the project's `Vyx.lock` when used.
Ignore reproducible output directories such as `target/`, `out/`, and `.cache/`.
For DCI projects, also maintain contract-generation inputs, producer sources,
and build configuration.

See the [manifest reference](PACKAGE_MANIFEST.md) for further options and
[Tools and IDEs](TOOLING.md) for editor installation, LSP, and DAP.

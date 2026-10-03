# Precompiled Library Example

Demonstrates Vyx's precompiled module workflow with a single source of truth: `Vyx.toml`.

```
samples/projects/precompiled_lib/
├── mathlib/                 # Library project — produces .dll + .lib + .vyi
│   ├── Vyx.toml             # type = "shared"
│   └── src/mathlib.vyx
├── app/                     # Consumer project
│   ├── Vyx.toml             # [dependencies] mathlib = "../mathlib"
│   └── src/main.vyx
└── README.md
```

## Build & run

These commands use the SDK compiler `vyxc.exe` on PATH. Repository regressions
use `bootstrap_compiler/out/vyxc.exe` freshly built from the current checkout,
with `vyx_compiler_backend` / `vyx_runtime` from that same build.

```powershell
# Step 1: build the library (produces out/mathlib.dll, out/mathlib.lib, out/mathlib.vyi)
cd mathlib
vyxc.exe build

# Step 2: build the consumer — auto-detects the dependency, builds it if needed,
#         injects -L/-l, copies mathlib.dll into out/
cd ../app
vyxc.exe build

# Step 3: run
.\out\app.exe
```

If you skip step 1, step 2 will run it for you — `vyxc.exe build` recursively builds dependencies whose `.vyi` is missing.

## How it works

**Library side** (`mathlib/Vyx.toml`):

```toml
[target.mathlib]
type = "shared"
entry = "src/mathlib.vyx"
```

`vyxc.exe build` in the library directory:

1. Compiles `src/mathlib.vyx` → `mathlib.obj`
2. Walks the obj's defined external symbols via `llvm-nm` and writes a `.def` file (excluding internal/`_`-prefixed symbols)
3. Links into `mathlib.dll` + import lib `mathlib.lib`
4. Re-invokes the compiler with `--emit-vyi` to produce `mathlib.vyi`

**Consumer side** (`app/Vyx.toml`):

```toml
[dependencies]
mathlib = "../mathlib"
```

`vyxc.exe build` in the consumer directory:

1. Reads `[dependencies]`, resolves each path to an absolute location
2. For each dependency, checks whether `<dep>/out/<name>.vyi` already exists; if not, recursively runs `vyxc.exe build` in the dependency directory
3. Auto-injects `lib_paths += "<dep>/out"` and `libs += "<name>"` for every target in the consumer manifest
4. Compiler scans the lib paths, finds `mathlib.vyi`, and merges the public declarations
5. Linker resolves `mathlib.lib` against `mathlib.dll`
6. Only genuine shared dependencies are copied into the consumer output; the
   canonical Vyx application runtime is linked statically

## What's exported in `.vyi`

Only `public` declarations make it into `.vyi`:

```vyx
public fn add(a: i32, b: i32) -> i32 { ... }      // ← exported
public class Vec2 { ... }                          // ← exported
internal fn _unused_helper() -> i32 { ... }        // ← NOT exported
private fn module_only() -> i32 { ... }            // ← NOT exported
```

The generated `mathlib.vyi`:

```vyx
// Auto-generated .vyi interface file. Do not edit.
module mathlib;

public fn add(a: i32, b: i32) -> i32;
public fn multiply(a: i32, b: i32) -> i32;
public fn factorial(n: i32) -> i64;
public class Vec2 {
    public x: i32;
    public y: i32;
    public static fn new(x: i32, y: i32) -> Vec2;
    public fn length_squared(self: Vec2) -> i32;
}
```

## Inline-table form for dependencies

```toml
[dependencies]
mathlib = { path = "../mathlib" }
otherlib = { path = "../path/to/other" }
```

Both `name = "path"` and `name = { path = "..." }` are accepted.

## Notes

- The dependency path is resolved relative to the consumer's `Vyx.toml`. Both relative (`../mathlib`) and absolute (`E:/libs/mathlib`) paths work.
- Internal helpers (`internal`, `private`) are filtered out of the generated `.def` file, so they aren't exported from the DLL.
- `.vyi` parsing currently ignores generic methods on imported classes; only concrete public signatures appear in the interface file.

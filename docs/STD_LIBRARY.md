# Vyx standard-library reference

[简体中文](STD_LIBRARY.zh-CN.md) · [Tutorial](STANDARD_LIBRARY_TUTORIAL.md) · [Documentation index](README.md)

> This page describes the public package routes and implementation boundary of
> the self-hosted standard library. For bootstrap, benchmark, and runtime
> correctness, use [the testing guide](TESTING_GUIDE.md) and the current command
> log. An API named here is not, by itself, a Release availability claim.

`bootstrap_compiler/std_packages/` is the single authority for std sources.
`--src=file` loads implicit `std.*` through `bootstrap_compiler/std_packages/registry`,
which forwards to each package's `.vyx` sources. The flat `bootstrap_compiler/std/`
and root `std/` trees are frozen legacy snapshots pending deletion; they are
not scanned when the registry is present and nothing syncs against them.

Authority check: `probes/gates/h/std_single_truth.sh` forbids the same
non-FFI module basename from appearing in two packages, and probes that
the SDK compiler can import a real std module.

## Package routes

Source-generic library packages use `type = "source"`, so the consuming target
performs MIR/monomorphization. Native bridges such as JSON and miniz remain
static packages.

| Path | target | Contents |
|---|---|---|
| `std:core` | `std_core` | defaults, Option/Result, String/Clone, Ref/Box, hash, operators, numbers |
| `std:collections` | `std_collections` | Vec, Dict, Set, List, Deque, Stack, Iterator |
| `std:algorithm` | `std_algorithm` | container algorithms |
| `std:io` | `std_io` | io, fs, path, process, args, env, ffi, vio |
| `std:sync` | `std_sync` | sync, channel, threading, thread_pool, concurrent |
| `std:format` | `std_format` | fmt, color |
| `std:testing` | `std_testing` | testing and mock |
| `std:toml` | `std_toml` | TOML |
| `std:json` | `std_json` | JSON plus cJSON native bridge |
| `std:miniz` | `std_miniz` | compression plus miniz bridge |
| `std:std` | `std` | aggregate package, including JSON/miniz |
| `std:cacao` | `std_cacao` | Cacao graphics/GPU C ABI bindings (opt-in; link Cacao) |
| `std:dll` | `std_dll` | dynamic library loader |
| `std:llvm` | `std_llvm` | LLVM C API bindings |
| `std:network` | `std_network` | curl bindings, HTTP client, WebSockets, gRPC (modules `std.network.*`) |
| `std:security` | `std_security` | OpenSSL bindings and pure-Vyx hashes (modules `std.security.*`) |
| `std:sqlite` | `std_sqlite` | SQLite3 driver |
| `std:stb_image` | `std_stb_image` | `std.image`: stb_image + write + resize2 (stub C implementation macros) |
| `std:win32` | `std_win32` | Win32 API helpers (`@[platform("windows")]`) |
| `std:logger` | `std_logger` | `std.log` via DCI + compiled spdlog (opt-in; package prebuild) |

For example:

```toml
[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }
```

`vyxc new app` creates these common global dependencies. In a multi-target
project, put a dependency under `[target.app.dependencies]` to make it belong
only to `app`.

Each package owns its canonical sources under its own `src/`. The `std:std`
manifest composes package dependencies instead of flattening the same files.
Bindings requiring a separate native SDK or implementation—LLVM, Cacao, curl,
OpenSSL, SQLite, stb_image, Win32, spdlog, and similar—do not belong in that
generic aggregate. Consume an explicit native package with a complete link
closure.

## Current implementation boundary

- `std.math` is source library code directly wrapping common libm functions.
- `std.vio` is source library code containing `Promise<T>` / `Task<T>`,
  platform gates, and thread/fiber/IO helpers.
- Bootstrap semantic analysis recognizes and filters `@[platform]`.
  `posix` matches Linux, Android, and macOS. OS-facing std APIs (`os`, `fs`,
  `path`, `sync`, `threading`, `vio`) use `windows` vs `posix`; Linux-only
  extras such as `io_uring` and x86_64 `syscallN` stay `@[platform("linux")]`.
- `async` / `await` spawn a Future (`Promise`/`Task`). `poll()` is `is_done`
  only (`true` = Ready, `false` = Pending). `await` inside `@[async] fn` is a
  MIR await-split (`poll` + `mir_term_yield`). `await_value()` in a non-async
  caller parks by pumping a stackless executor and firing `Task.sleep` timers.
  `StartCoroutine` still uses OS fibers. Blocking syscalls such as `vio_sleep`
  block the scheduler.
- `@[comptime]` folds `@[comptime] fn` calls with constant integer arguments
  to literals. The interpreter runs `while`/`for`, `break`/`continue`,
  `print`/`println`, and `ct_file_size`. Heap is not interpreted.
- Assignment is move-by-default (`std.clone`). Types that satisfy `Copy`
  (scalars, `impl Copy` / `@[derive(Copy)]`) do not trigger E3100 / V-MOVE-001
  on assignment. Class `string` / `String` is not Copy; use `.clone()` for a
  second owned value.
- `std.ref` exposes class `Ref<T>` / `Box<T>` only. The legacy `_Ref` /
  `_Box` i64-cell function API has been removed.
- Dict/process/FS helpers in the bootstrap runtime still use C++ STL in
  places (`unordered_map` for dict, `recursive_directory_iterator` for
  tree walks). `vyx_bootstrap_mkdir_p` / `sleep_ms` / `file_mtime` /
  `file_stamp` / `cwd` are Win32/POSIX. User-program `i64` sort lives in
  `runtime/src/vyx_runtime.vyx`.
- `std.ffi` is C ABI only: `c_int` / `c_char` / `c_size_t` plus `CStr` /
  `CString`. `c_long` follows the target C ABI (Windows LLP64 = i32;
  POSIX LP64 = i64). It does not wrap C++.
- Android fiber helpers (`vyx_fiber_posix_*`) are no-op stubs: Bionic has
  `ucontext_t` but no `getcontext` / `swapcontext` / `makecontext`. `@[async]`
  uses the stackless pump and does not need them. `StartCoroutine` still does.

## Native-bridge packages

`bootstrap_compiler/std_packages/logger` is the home of `std.log`. The
package `prebuild` hook is a Vyx script (`vyx: scripts/prepare_spdlog.vyx`)
that clones spdlog, cmake-builds the static library, and runs the in-tree
DCI adapter to emit `contracts/spdlog.dcib`. Vyx calls `spdlog::logger::log`
through that contract; a small C++ helper only covers default-logger
lifetime (`shared_ptr` sink). Dependents call `log_info` / `set_log_level`
and read levels through `log_level_*()` (package `.vyi` does not re-export
`let` initializers). Depend on it explicitly:

```toml
[dependencies]
logger = { path = "std:logger", target = "std_logger" }
```

`bootstrap_compiler/std_packages/json` keeps the Vyx API, cJSON source, and
native bridge in one `Vyx.toml` project:

```toml
[dependencies]
json = { path = "std:json", target = "std_json" }
```

The builder recursively builds the package, imports its `.vyi`, adds the static
library, and propagates runtime resources. `std.json` exposes `Result` errors,
object/array/scalar values, deep copy of child values, type checks,
serialization, and deterministic `drop()`. Missing keys, type errors, parse
errors, and allocation errors are distinct failures, not empty-value aliases.

`bootstrap_compiler/std_packages/miniz` provides self-contained zlib-compatible
compression, decompression, CRC32, and Adler32:

```toml
[dependencies]
miniz = { path = "std:miniz", target = "std_miniz" }
```

`CompressedData` owns its native buffer and drops it automatically. Decompression
requires a maximum output length; insufficient capacity, bad arguments,
allocation failure, and malformed data are reported through `Result`.

`bootstrap_compiler/std_packages/stb_image` vendors the stb image family and
compiles it through one stub, the same mixed C/H pattern as `std:json`:

- `vendor/stb_image.h`
- `vendor/stb_image_write.h`
- `vendor/stb_image_resize2.h`
- `native/stb_image_stub.c` defines `STB_IMAGE_IMPLEMENTATION`,
  `STB_IMAGE_WRITE_IMPLEMENTATION`, and `STB_IMAGE_RESIZE_IMPLEMENTATION`,
  then includes those headers (header-only libraries do not emit symbols
  without that translation unit)

```toml
[dependencies]
stb_image = { path = "std:stb_image", target = "std_stb_image" }
```

The consumer does not add stb `.c` / `.h` files or extra `libs`.

Opt-in native SDK bindings (Cacao, curl, OpenSSL, SQLite, LLVM, Win32, and
the rest of the table above) are `type = "source"` packages. They are in-tree
Vyx bindings and do not vendor the C/C++ SDK:

```toml
[dependencies]
cacao = { path = "std:cacao", target = "std_cacao" }
```

Link the corresponding native library on the consumer target (`libs` /
`lib_paths`), the same way `samples/projects/cacao` links Cacao.

The flat `std.cjson` binding was removed. An explicit source package disables
that target's implicit flat-std lookup: a missing dependency is an import error,
not a silent fallback to a compatibility mirror.

## Built-ins

### Output and diagnostics

| Function | Signature | Meaning |
|---|---|---|
| `print` | `print(val)` | print i64/f64/string/bool and compatible values |
| `assert` | `assert(cond: bool)` | stop and report a location when false |
| `panic` | `panic(msg: string)` | immediately terminate with a message |
| `format` | `format(fmt, args...)` | format a string |

### Types and memory

| Function | Signature | Meaning |
|---|---|---|
| `sizeof::<T>()` | `-> i64` | byte size of `T` |
| `alignof::<T>()` | `-> i64` | alignment requirement of `T` |
| `type_name::<T>()` | `-> string` | compile-time type name |
| `typeinfo(T)` | `.fields -> [string]` | compile-time field-name list |
| `alloc(size: i64)` | `-> rawptr` | allocate memory |
| `dealloc(ptr: rawptr)` | | free memory |
| `transmute` | `transmute::<T>(val)` | bit-level conversion (`unsafe`) |

### Pointers and command line

| Function | Signature | Meaning |
|---|---|---|
| `to_rawptr(&val)` | `-> rawptr` | obtain a raw pointer to a value |
| `from_cstr(ptr: rawptr)` | `-> string` | convert a C string |
| `argCount()` | `-> i64` | command-line argument count |
| `getArg(i: i64)` | `-> string` | argument at index |
| `getArgs()` | `-> Vec<string>` | all command-line arguments |

## Module notes

### `std.math`

`std.math` is a libm-oriented source module, not a compiler-provided mock
library. It includes `abs_val` / `abs_f64`, trigonometric and inverse
trigonometric functions, hyperbolic functions, `sqrt` / `cbrt`, `pow`,
`exp` / `exp2`, logarithms, rounding functions, and `pi()` / `tau()` / `e()`.

### `std.vio`

| Component | Role |
|---|---|
| `vio_start` / `vio_stop` | initialize / shut down the runtime |
| `vio_sleep` / `vio_yield` / `vio_now` | scheduling and time |
| `Promise<T>` / `Task<T>` | `new`, `ready`, `failed`, `resolve`, `reject`, `poll`, `await_value` |
| `StartCoroutine` / `StopCoroutine` | coroutine entry wrappers |
| `@[platform]` | platform-gated declaration |
| `vio_run` / `vio_run_wait` | thread-level asynchronous execution |
| `vio_fiber_*` | fiber-scheduling helpers |

At the compiler layer, `@[async]` calls lower to `vio_sched_spawn` and
`await` lower to `await_value()`. `poll()` is the user Future surface. On
Android, missing ucontext means stackless invoke rather than fiber switch
(see the implementation boundary above).

### `std.sync` / `std.threading` / `std.thread_pool`

`Mutex<T>`, `RwLock<T>`, `Condvar`, `Barrier`, `Once`, and `atomic_*` are Vyx
types. The handles are OS objects from `runtime/src/vyx_sync.c` (Windows
SRWLOCK / CONDITION_VARIABLE / CreateThread, POSIX pthread). This is not a
wrapper around C++ `std::mutex` / `std::thread`.

`std.thread_pool` is the work-stealing scheduler: it calls oneTBB
(`tbb::parallel_for`, `tbb::task_group`, `tbb::task_arena`) through a C ABI
in `runtime/src/vyx_tbb.cpp`. Linking a program that `use std.thread_pool`
pulls `tbb12` (Windows) or `tbb` (POSIX). `runtime/scripts/prepare_tbb.vyx`
clones [oneTBB](https://github.com/uxlfoundation/oneTBB) `v2022.1.0` and
places the import library next to the SDK compiler (`vyxc`)
(`vyx: ../runtime/scripts/prepare_tbb.vyx`).

| API | Role |
|---|---|
| `thread_spawn` / `thread_join` / `thread_detach` | OS thread |
| `thread_available_parallelism` / `thread_yield` / `thread_park` | scheduler helpers |
| `Mutex<T>` / `RwLock<T>` / `Condvar` / `Barrier` / `Once` | Vyx sync types |
| `atomic_load` / `atomic_add` / `atomic_cas` | seq_cst i64 |
| `parallel_for` / `task_group_run` / `arena_*` | oneTBB work-stealing pool |

`parallel_for(n, body, ctx)` takes a **typed C-ABI callback**
`cfn(rawptr, i64) -> void`: mark the body `@[no_mangle]` and pass it by name.
Vyx `fn` values are fat pointers and cannot cross a C callback boundary, so
there is no `rawptr` overload. The body runs on pool threads — write only to
disjoint slots (e.g. `ctx + i * elem_size`); there is no implicit
synchronization. See `probes/gates/tbb_pool` for a runnable example.

### `std.time`

The authoritative module is POSIX `clock_gettime` / `gettimeofday` /
`nanosleep` exposing `current_timestamp` / `clock_ms` / `monotonic_nanos` /
`sleep_ms` (i64/void). Struct-by-value `Duration` helpers are deferred
until the class-by-value ABI rule (WP-P0a) is the lowering path.
`--src=file` resolves `use std.time` through `std_packages/registry` to
`bootstrap_compiler/std_packages/std/src/time.vyx`.
The previous 448-line Windows QPC/`Sleep` copy is not the public API.

### Core values and collections

`Option<T>` / `Result<T, E>` originate in `std.option` / `std.result`; `Some`,
`None`, `Ok`, and `Err` should come from std declarations rather than hard-coded
semantic-analysis or code-generation cases.

`Vec`, `Dict`, `Set`, `Stack`, `Queue`, and `Iterator` similarly come from std
source modules. Compatibility constructors are std-layer facilities, not
compiler built-ins.

| Type | Module | Principal operations |
|---|---|---|
| `Vec<T>` | `std.vec` | `new`, `push`, `get`, `set`, `pop`, `count`, `capacity`, `destroy` |
| `Dict<K, V>` | `std.dict` | `new`, `put`, `get`, `contains`, `remove`, `destroy` |
| `Set<T>` | `std.set` | `new`, `add`, `contains`, `remove`, `destroy` |
| `Stack<T>` / `Queue<T>` | `std.stack` | push/pop or enqueue/dequeue operations |
| `Iterator<T>` | `std.iter` | iteration cursor operations |
| `String` / `str` | `std.string` | owning/borrowed UTF-8 text and character/byte iteration |

Do not modify a container's structure while an iterator is live. A `String`
cursor also borrows the source storage: changing or destroying the source
invalidates it.

### Reflection, hash, and other modules

`std.reflect` is `Type` / `Field` / `Method` / `Instance` (not TypeBuilder).
`Type.of::<T>()` / `getType(name)` intern the compiler CSV once into a hash
table; `getField` / `getMethod` / `Instance.getProperty` are O(1) after intern.
Teach `@[reflect("…", alias=[…])]` and `@[hidden]`. Contract:
`tests/cases/tutorial_reflect.vyx`. `std.hash` supplies `Hashable`;
`std.collections` supplies `Iterable`.
Other checked-in standard modules cover filesystem/process I/O, formatting,
testing, TOML, synchronization, algorithms, JSON, and miniz.

For the Chinese detailed source/API inventory, use
[the Chinese reference](STD_LIBRARY.zh-CN.md). Treat both pages as documentation
of the tree; verify a particular package with a fresh compiler and its matching
project or contract.

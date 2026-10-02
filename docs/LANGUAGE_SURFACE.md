# Vyx language surface

[简体中文：语言表面](语言表面_ZH.md) · [Documentation index](README.md)

Use this index to find the lesson for a language construct. For the rule
behind it, see [language design](LANGUAGE_DESIGN.md); for the compiler stage
that handles it, see [compiler architecture](COMPILER.md). Claims about current
support should be checked with the SDK compiler freshly built from the current
source tree and the linked cases or `probes/gates/` programs.

## How to use this page

1. Look up the construct.
2. Run the linked lesson's complete example with the [verification guide](TESTING_GUIDE.md).
3. Read any scope note in the row as the current implementation boundary.

Editor plugins (VS Code / IntelliJ) talk to `vyxc-lsp` / `vyxc-dap`. They are
not additional language features.

## Lessons 1–10 — [tutorial](TUTORIAL.md)

| Construct | Lesson |
|---|---|
| `fn main() -> i32`, `print`, `;` | 1 |
| `let` / `var`, `${}` / `f"…"`, `"""…"""`, `//` `/* */` `///` | 2 |
| `i8`…`i64`, `u8`…`u64`, `isize`/`usize`, `f32`/`f64`, `bool`, `char`, `string` | 3 |
| `0x`/`0o`/`0b`, integer suffixes, `[e,…]` / `[elem; n]`, `(e, e)` / `.0` | 3 |
| `if` / `elif` / `else`, `? :`, `if` as expression | 4 |
| `while`, `for`, C-style `for`, `break`, `continue`, `'outer: while` / `break 'outer` | 5 |
| functions, defaults, `f(name: expr)`, `return` | 6 |
| `struct`, `Name { field: expr }` | 7 |
| `class`, `self`, `public` / `@[vis(...)]`, `ClassName(args)` constructor | 8 |
| nested `struct` / `class` in a record body, `Outer<i32>::Inner<f64> { … }` | 7 + 8 |
| `panic` | 9 |
| `defer` | 10 |

## Lessons 11–20 — [intermediate](INTERMEDIATE_TUTORIAL.md)

| Construct | Lesson |
|---|---|
| `Vec` / `Dict` / `Set` | 11 |
| closures `\|x\| { … }` and `[cap](x) =>` | 12 |
| `fn f<T>(…) where T: Trait`, `const N` | 13 |
| `enum`, `match` / `case` (statement or value expression) | 14 |
| `string` methods, `f"…"` / `"""…"""` | 15 |
| `Option` / `Result` / `?` / `fail Type.Variant` | 16 |
| `+=` and bitwise operators | 17 |
| `for (x in xs)` | 18 |
| `module`, `Vyx.toml`, `sources`, `vyxc build` / `new` / `repl` / `install` / `test` / `bench` / `fmt` / `doc` | 19 |
| `extern "C"`, `cfn(…) -> R` | 20 |
| DCI `extern "dci"` / `"cpp"`, `@[dci_import]` | 20 + [42](ADVANCED_FEATURES.md#lesson-42-dci--c-abi-interop) |

Local `file://` registry (`vyx publish` / `install` / `lock`) is documented in
[PACKAGE_MANIFEST.md](PACKAGE_MANIFEST.md), not as a language construct.

## Lessons 21–43 — [advanced](ADVANCED_FEATURES.md)

| Construct | Lesson | Honesty |
|---|---|---|
| `&T` / `&mut T`, `*T`, `->` | Safety model | E3101 on conflicting borrows |
| `Ref` / `Weak` / `Box` | 21 | single-threaded refcount |
| generic `Vec` iteration | 22 | iterator invalidation on structural mutation |
| closure captures | 23 | |
| `@[derive(Clone, Hashable, Eq, Ord, Debug, Display, Copy)]` | 24 | fieldwise `.clone()`; `rawptr`/`*T` fields rejected |
| `@[async]` / `await` / `Task` / `Promise` | 25 | `poll()` is the Future surface; MIR await-split + stackless pump + `Task.sleep`; StartCoroutine still uses OS fibers; run with AOT |
| enum payloads | 26 | |
| `trait` / `impl` | 27 | solver + blanket `where` |
| `dyn Trait` | 28 | |
| `\|>` pipe | 29 | |
| `unsafe`, `rawptr`, `@[repr(C)]`, `@[no_mangle]`, `@[link_name]` | 30 | C ABI only; C++ / Rust and later compiled languages go through [DCI lesson 42](ADVANCED_FEATURES.md#lesson-42-dci--c-abi-interop) |
| `drop()` | 31 | does not replace `Vec.destroy()` |
| generic enum, `<...Ts>` packs | 32 | |
| `@[comptime]` fold | 33 | integer locals, `while`/`for`, `break`/`continue`, `print`/`println`, `ct_file_size`; no heap |
| move E3100, `Copy`, clone-before-move | 34 | `string`/`String` class is not Copy; scalars are |
| `class Child : Base`, `class Mix : Left, Right`, `override` | 35 | diamond inheritance is an error |
| `interface` default methods, super-interface | 36 | `interface` / `trait` / `protocol` share one keyword slot |
| `type Alias = …`, associated `const` | 37 | |
| `@[platform("windows"\|"posix"\|"linux"\|"macos"\|"android")]` | 38 | `posix` = Linux + Android + macOS |
| `sizeof::<T>()` / `alignof::<T>()` / `static_assert` | 39 | fold to `i64` |
| `operator +` / `operator[]` methods | 40 | |
| `@[version]` / `@[variant]` / `@[migrate]` / `@[discard]` / `name@ver["tag"]` | 41 | `fromSig` is not a matcher; E2400 on bad migrate/discard |
| `@[dci_import]`, `extern "dci"` / `"cpp"` | 42 | Adapter `.dcib`; Direct vs Stub; [current AOT coverage](DCI_SPEC.md); JSON integers ±2^53 |
| `std.reflect` `getType` / `Type.of` / `Instance` | 43 | interned hash tables; `@[reflect]` aliases; not `reflect_*` builtins |

## Standard library (not extra syntax)

Use the [standard-library tutorial](STANDARD_LIBRARY_TUTORIAL.md) and
[reference](STD_LIBRARY.md) for `std.collections`, `std.sync`, `std.vio`,
`std.reflect` (`Type.of` / `getType` / `Instance`; interned hash lookup, lesson 43),
`std.ffi` (`c_long` follows LLP64 vs LP64), `os`/`fs`/`path` (`windows` vs
`posix`). Cross-compile Android from Windows:
[TESTING_GUIDE.md](TESTING_GUIDE.md#cross-compilation-windows-host).

## Parsed constructs with limited support

These forms have narrower behavior than their syntax might suggest:

| Construct | Note |
|---|---|
| `T?` types | parsed as a `try` / `?` node, the type itself is **rejected**; propagate errors with `?` / `try` (lesson 16) |
| `new T(...)` / `delete` | ordinary heap allocation: `let a = new i32(1)`, `delete a`; containers also take `Vec::<T>.new()` (lesson 9) |
| `match` as an expression | compatible arm values, boolean guards, enum payloads and block tails; use `default` for fallback; no general exhaustiveness guarantee ([lesson 14](INTERMEDIATE_TUTORIAL.md#lesson-14-match)) |

## Reserved words that are not tutorial topics

These tokenize as keywords (or identifiers) but are **not** a supported
learning path in 1.0.0-alpha.1 ea. Do not infer a feature from the token:

| Token | What to use instead |
|---|---|
| `asm`, `macro`, `bench`, `volatile` | not taught; no user contract |
| `concept`, `requires` | `where T: Trait` (lesson 13) |
| `foreach` | `for (x in xs)` (lesson 18) |
| `error` (keyword) | `Result` / `?` (lesson 16) |
| `newtype` | `class` or `type` alias (lessons 8, 37) |
| `task` / `fail` as keywords | `@[async]` + `std.vio` `Task` (lesson 25); `fail Type.Variant` is lesson 16 |
| `extern "C++"` as language FFI | [DCI lesson 42](ADVANCED_FEATURES.md#lesson-42-dci--c-abi-interop) |
| `--verify-hir2` / `--verify-mir2` | compiler IR checks, not user type errors |

`new` is an ordinary identifier (heap helpers live on types such as
`Vec::<T>.new()`), not a keyword.

## When a tutorial snippet disagrees with the SDK compiler

Trust the SDK compiler freshly built from the current source tree and the
matching `probes/gates/` program. File the doc fix against this page and the
lesson, not by adding a compiler special case.

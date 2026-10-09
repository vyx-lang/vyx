# Vyx language design

[中文：语言设计规范](设计文档_ZH.md) · [Documentation index](README.md) ·
[Language surface](LANGUAGE_SURFACE.md)

**Source:** the current self-hosted compiler in `bootstrap_compiler/`.
This page is reconstructed from `src/core/syntax/lexer.vyx`, `src/core/sema/policy.vyx`,
`src/core/syntax/parser.vyx`, `src/core/sema/sema.vyx`, `src/core/syntax/diag.vyx`,
`src/core/driver/main.vyx`, and `src/hir/builder/hir_builder.vyx`. Older host-era version
lists and examples do not describe the current compiler.

Tutorials teach this surface with runnable files. The [compiler architecture](COMPILER.md)
shows where HIR, MIR, and LLVM process it. If a snippet and the SDK compiler
freshly built from the current source tree disagree, update this page against
the compiler behavior.

## Design invariants

1. Observable `drop` / `defer` / panic / FFI / DCI calls are not dead-code
   eliminated as pure expressions.
2. Layout, packed ABI, and foreign alignment beat optimizer convenience.
3. Higher optimization levels must not change `O0` semantics.
4. Unproven alias / lifetime / effect facts stay conservative.
5. MIR2LLVM is the acceptance backend. `--emit=cpp` is experimental.

## Keywords (`policy.vyx` `policy_keyword_kind`)

Reserved: `fn` `if` `as` `is` `in` `let` `var` `use` `for` `mut` `asm`
`true` `enum` `impl` `elif` `else` `case` `null` `self` `task` `fail` `type`
`when` `bool` `char` `void` `false` `class` `error` `while` `match` `break`
`yield` `async` `await` `const` `defer` `bench` `trait` `where` `macro`
`isize` `usize` `struct` `import` `module` `export` `extern` `return`
`public` `static` `unsafe` `rawptr` `default` `private` `newtype` `concept`
`foreach` `continue` `internal` `override` `comptime` `protocol` `requires`
`volatile` `interface` `protected` `static_assert` plus integer/float widths
(`i8`…`u64`, `f32` `f64`).

`new` is **not** a keyword (kind 5 = identifier). `interface` / `trait` /
`protocol` share kind **11**. `string` / `str` are primitive **type names**
(`policy_primitive_type_kind` → 13), not keywords.

## Lexical (`lexer.vyx`)

- Whitespace; `//` line comments; nested `/* … */`; `///` doc lines attach
  to the next token (`skip_whitespace_and_comments` / `flush_pending_doc_to_last`).
- Integers: decimal, `0x`/`0X`, `0o`/`0O`, `0b`/`0B`. Optional suffixes
  `i8`…`i64` / `u8`…`u64` / `isize` / `usize` (`consume_int_suffix`).
  Float suffixes are **rejected**. Underscores in numbers are not a contract.
- Strings: `"…"` with escapes `\n \t \r \\ \" \' \0 \$`. `${expr}` inside a
  normal string becomes interpolation (`parser_raw_string_has_interpolation`).
  `f"…"` is `tk_FSTRING_RAW`. `"""…"""` is a multi-line string.
- Char literals `'x'` (ASCII fast path). `'ident` (no closing quote next)
  is a label token, not a char.
- `true` / `false` are bool literals before keyword lookup.

## Types (`parser.vyx` `parse_type`)

Primitives: `i8` `i16` `i32` `i64` `isize`, `u8` `u16` `u32` `u64` `usize`,
`f32` `f64`, `bool`, `char`, `str` / `string` (same `ty_STRING()` kind),
`rawptr`, `void`.

Type constructors:

| Syntax | Meaning |
|---|---|
| `&T` / `&mut T` | shared / unique borrow |
| `*T` | typed pointer; member access is `p->field` |
| `[T; N]` or `[N; T]` | fixed array |
| `(T, U, …)` / `()` | tuple; `(T)` is grouping, not a 1-tuple |
| `fn(…) -> R` | Vyx function type |
| `unsafe fn(…) -> R` / `cfn(…) -> R` | C function pointer (`parse_fn_type_ref_after_kw(true)`) |
| `dyn Trait` | trait object (`dyn` is an identifier prefix, not a keyword) |
| `Path::<Args>` | generic type; `::` or `.` in type paths |
| `…` in parameter / type position | C varargs / pack |

`T?` is **rejected** (`finish_type_suffix`: “nullable type suffix requires
the standard library option type”). Use `Option<T>`.

`type Name = T;` and `newtype Name = T;` are both parsed
(`parse_type_alias_decl`). Associated `const` and associated `type` live on
`trait` / `interface` / `class` / `enum`.

Built-in type queries (sema): `sizeof::<T>()`, `repr_sizeof::<T>()`,
`alignof::<T>()` → `i64`.

## Declarations (`parse_decl`)

Prefix markers (consumed before the item keyword): `export`, `async`,
`comptime`, `unsafe`, `task`, and the misspellings `coroutine` /
`corountine`. After visibility: `static`, then `override` / `virtual` /
`final`.

| Form | Parser |
|---|---|
| `module a.b;` / `module a.b { … }` | `parse_module` |
| `use path;` / `import path;` / `use Alias = path;` / `use path as Alias;` | `parse_use` |
| `use` / `import` version suffix `@1.2["variant"]` | `parse_version_selector_suffix` |
| `fn` / `public fn` / `static fn` | free functions and methods |
| `struct` | `DE_CLASS` **fields only** (`parse_struct`; no methods in the body) |
| `class` | fields, `fn` methods, `ClassName(…)` constructors, `const` / `type` members |
| `enum` / `error` | variants with optional `(payload)`; `error` may also contain class members |
| `trait` / `interface` / `protocol` | one keyword slot |
| `concept` | parsed as interface-like (`\|kind:interface\|kind:concept`); **not** the taught constraint language |
| `impl Trait for T` / inherent `impl T` | `parse_impl_decl`; optional `<T: Bound>` and `where` |
| `let` / `var` / `const` at module or local scope | bindings |
| `type` / `newtype` | aliases |
| `public` / `@[vis(scope)]` | visibility; explicit scopes override the `public` shorthand |
| `override fn` | replaces a parent method |
| `class Child : Base` / `struct V3 : V2` | `|impl:` in `parse_decl_suffix_markers` |
| `extern "C" { … }` | C functions, `type`/`newtype`, `let`/`var` |
| `extern "C++"` / `extern "dci" { class/struct/fn/… }` | **DCI surface** (`extern_abi_uses_dci_surface`), not a second language FFI |

Functions and members:

- Parameters: `name: T`, default `name: T = expr` (init stored on the param
  decl), variadic `...name: T` / `name: ...T`, trailing C varargs `...`
  (`__c_varargs`).
- Receivers: unmarked `self` (C++ `this` escape), `&self`, `&mut self`, `own self` (Rust `self`), `mut self` (Rust `mut self`). `own` is only valid as the compound `own self`.
- Return type `-> T`. Function **bodies** have no implicit last-expression
  return; use `return`. `if`-expressions allow a last expression without `;`
  (`parse_if_expr_block`).
- `operator +` / `operator[]` / `operator++` / … (`parse_operator_decl_name`).
- Generic params `<T, const N>` (`N` stored as `N^const`). `where` and
  `requires` are the same trailing constraint text on `fn` / method / ctor.
- Interface / trait methods may have a `{ body }` (default method) or `;`.
- Constructor: `ClassName(args) { … }` inside the class is a **static** method
  named `constructor` (`parse_constructor`). Call sites do not pass `self`;
  the body still binds `self` (HIR constructor ABI self) and may assign
  fields. A fall-off-the-end constructor returns that `self`.
- Class field `name: T [= expr]` — `;` is optional when the next token is
  clearly another member or `}`.
- `@[…] { …decls… }` applies the attributes to every inner decl
  (`parse_attributed_decl_block`).
- DCI-only: `~Owner();` is a destructor declaration stored as method `drop`.

## Expressions (`parse_expr` chain, low → high)

```
assign          =  += -= *= /= %= <<= >>= &= |= ^=
ternary          cond ? a : b          (right-assoc; C grouping)
coalesce         ??
range            ..  ..=
pipe             |>
logic            ||  &&
bitwise          |  ^  &
compare          == !=  < <= > >=
shift            << >>
add/mul          + -   * / %
cast             as T     (`&n as *T` is `(&n) as *T`)
unary            !  -  ~  ++ --  *  &  &mut  own  try  await  comptime  new  delete
postfix          call  .member  ::path  ->member  [index]  {fields}  ?  ++ --  ...folds
primary          literals  ident  (expr)  if-expr  closures  f-strings
```

Also parsed:

- Interpolating strings: `"… ${expr} …"` and `f"…"` (`parse_fstring_inner`).
  Brace-only `{expr}` inside a string is **not** interpolation.
- `if (cond) { e } elif … else { e }` as an **expression** (`parse_if_expr`).
  Condition **requires** `(…)`.
- Closures: `fn (x: T) -> R { … }`, `|x: T| { … }`, `|| expr`, and
  `[cap, &r](x) => { … }` (`parse_primary`).
- Named arguments: `f(name: expr)` (`parse_call_args`).
- Struct / class literals: `Name { field: expr, … }` (`parse_struct_init`).
- Array literals: `[e, e, …]` and repeat `[elem; n]` (`ex_ARRAY_LIT`).
- Tuple literals: `()` and `(e, e, …)`. A single `(e)` is grouping.
- Turbofish: `Foo::<T>` on types and calls; `Foo@1.2["v"]` version selector.
- Postfix try `expr?` only when the next token is a terminator (`;`, `,`,
  `)`, `}`, `]`, EOF); otherwise `?` is ternary.
- Prefix `try expr` is the same try-`?` node.
- Bindings: `let` is owned immutable; `var` / `let mut` are owned mutable. `&T` / `&mut T` share the method-receiver borrow oracle. `unsafe { }` skips E3100/E3101.
- Contextual `new T(...)` / `new T { … }` / `new::<T>(…)` / optional allocator
  group; contextual `delete` / `delete::<T>(allocator) p`.
- Array allocation brace init: `new [T; N] { … }`.
- Pack folds on idents: `xs...+` / `...-` / `...*` / `...&&` / `...|`.
- `fail Type.Variant` / `fail Type.Variant(payload)` /
  `fail Type.Variant.with(msg)` (`ex_FAIL`; sema types it as the current return
  type).
- `asm("template", …)` → `ex_INLINE_ASM`. Sema types it; MIR lowers to an
  **intrinsic**, not a guaranteed assembler dialect. Not a taught contract.
- `sizeof...()` in primary position is rewritten to the integer `1` (pack
  placeholder).

`string + T` concatenates when either operand is string-like
(`resolve_bin_op` + `str_concat`).

`match` also produces a value: `let result = match value { case 0 => 10,
default => 20, };`. Parentheses around the subject are optional. Arms can use
enum payload bindings, boolean guards, nested expressions, or blocks with a
tail expression (`{ let x = 1; x; }`). The subject is evaluated once.
Value-producing arms must have compatible types; a missing value, an empty
match, or a non-boolean guard is diagnosed with E1000. Use `default` for a
fallback: a bare `_` arm is not supported, although payload patterns such as
`Some(_)` are accepted. General exhaustiveness checking is not guaranteed.
See [lesson 14](INTERMEDIATE_TUTORIAL.md#lesson-14-match) and the
[AOT regression gate](../probes/gates/match-expression/README.md).

## Statements (`parse_stmt`)

`let` / `var` (optional `let volatile` / `var volatile` — local flag, not a
taught feature), `if (cond) { … }` / `elif (cond)` / `else` (**parens
required**), `while (cond) { … }`, C-style `for (init; cond; incr)`,
`for x in xs` / `for (x in xs)` / `foreach`, `match` / `case` / `default`
(optional `if` guard; `=>` or `:`), `break`/`continue` with optional ident
label, labeled `name: stmt`, `return`/`defer` (block or one statement),
`yield expr;` (stored as `ex_AWAIT`; `yield return` is `return`),
`unsafe { … }` (block flag `stmt_flag_unsafe_block`),
`static_assert(cond [, msg…]);`, blocks `{ … }`.

`unsafe` **without** `{` is `parse_skip_stmt` (consumed, replaced by a null
expression) — not a contract.

## Ownership (`sema.vyx`)

- Heap-owning by-value assignment is a **move** (E3100). Using the source
  afterwards is an error. `type_is_heap_owning` treats class `string`/`String`,
  `Vec`/`Dict`/`HashMap`/`HashSet`/`Set`/`Stack`/`Queue`/`Deque`/`List`/
  `PriorityQueue`/`BTreeMap`/`BTreeSet`, `Box`/`Ref`/`Scope`, types with
  instance `drop`, and nested heap fields as owning.
- `type_is_copy`: integer/float/bool/char, `rawptr`, `cfn`/`fn`, and primitive
  `ty_STRING()` are Copy. `&T` / `&mut T` / `*T` are not. Class `string` /
  `String` is **not** Copy. Other classes need `impl Copy` /
  `@[derive(Copy)]` (`type_satisfies_trait`).
- `&T` live + `&mut T` or move of the borrowed local is **E3101**.
- `defer { }` runs on scope exit, reverse order. Class `drop()` is the
  language cleanup hook; `Vec.destroy()` is a library buffer API and is not
  replaced by `drop()`.

`Ref` / `Weak` / `Box` are **std.ref** types, not compiler built-in `makeRef`.

## Built-in methods (`policy.vyx`)

On primitive `string`: `len`/`size`/`count`, `ptr`, `c_str`, `clone`,
`charAt`/`char_at`/`charCodeAt`, `substr`/`substring`, `trim`, `toUpper` /
`toLower`, `replace`, `contains`, `isEmpty`, `startsWith`/`endsWith`,
`repeat`, `toString`, `equals` / `operator==`.

On `*T` / `&T` / `&mut T` / `rawptr`: `offset`/`add`/`sub` (element-scaled),
`byte_offset`/`byte_add`/`byte_sub`, `offset_from` / `byte_offset_from`.

Free names that sema treats as host builtins (`is_host_builtin` — not
undefined): `print`/`println`/`format`, `panic`/`assert`/`assert_eq`,
`sizeof`/`repr_sizeof`/`alignof`, `typeof`/`typeinfo`/`type_name`,
`field_count`/`field_name`/`field_type`, `hash`/`compare`/`operator==`,
`argCount`/`getArg`, `malloc`/`realloc`/`free`/`memcpy`/`memmove`/`memcmp`/
`memset`, `strlen`, `to_rawptr`/`ptr_offset`/`ptr_read_*`/`ptr_write_*`,
`from_cstr`/`from_cstr_len`/`from_cstr_view_len`/`from_raw_string_parts`,
`int_to_string`/`bool_to_string`, `construct`/`alloc`/`dealloc`/`transmute`,
`reflect_*`, `ct_file_size`. Presence in this list is not a tutorial promise;
`print` and `panic` are the taught builtins. Runtime reflection is taught as
`use std.reflect` (`Type.of` / `getType` / `Instance`), not as `reflect_*`.

## Diagnostics (`diag.vyx`)

Structured codes include `E0001`/`E0002` (syntax), `E0101`–`E0104` (lexer),
`E1000` type mismatch, `E1100` trait bound, `E1200` control flow, `E2000`
undefined, `E2100`/`E2101` duplicate/redefinition, `E2200` no overload,
`E2300` ambiguity, `E2400` version policy, `E3000` generic, `E3100` move,
`E3101` borrow, `E3200` mono clone depth, `I0001`/`I0100` internal/codegen.

## Driver (`main.vyx` `show_usage` / `main`)

Commands wired in `main`: `--src=file|project`, `build`, `new`, `dci`, `run`,
`repl` (`vyx_bootstrap_repl_run`), `install`/`publish`/`search`/`lock`
(`pkg_dispatch`, `file://` registry), `test`, `bench`, `fmt`, `doc`,
`version`, `help`.
`--emit=` kinds: `ir obj exe dll lib vyi cpp dci-stubs hir mir hir-verify
mir-verify hir-facts`. LLVM is the acceptance backend; `cpp` is experimental.

`test` / `bench` / `fmt` / `doc` (`cli_tools.vyx`):

- `test [--filter] [path…]` — find `@[test]` functions under `tests/` (or
  explicit paths), wrap a `main` when the file has none, compile with this
  SDK compiler (`--src=file --emit=exe`), run. Non-zero if any fail.
- `bench [--filter] [path…]` — same with `@[bench]` under `benches/`; print
  wall-clock milliseconds around the run. The `bench` **keyword** is still
  unused.
- `fmt [--check] [path…]` — token-reprint `.vyx`. `//` and `/* */` are
  dropped (the lexer does not keep them); `///` is rewritten. `--check`
  exits 1 if a file would change. Not rustfmt.
- `doc [--out <dir>] [path…]` — markdown from `///` on the next
  `fn`/`class`/`struct`/`interface`/`enum`. Default stdout.

## Attributes

The parser stores `@[…]` / `@ident` as declaration meta
(`collect_attribute`). Sema / HIR interpret at least:

| Attribute | Effect | Where |
|---|---|---|
| `@[vis(scope)]` | `world` / `self` / `mod` / `super` / `tree` / `package` / `in(...)` / `friend(...)` / `none`; `+` union, `&` intersection, `-` exclusion; `public` abbreviates `world` | `sema_decl_visibility_expr` / `sema_vis_expr_allows`; [lesson 8](TUTORIAL.md#lesson-8-classes-and-methods) |
| `@[platform("windows"\|"posix"\|"linux"\|"android"\|"macos")]` | keep or drop the decl; `posix` = linux\|android\|macos; host aliases `win32`/`msvc`, `darwin`/`apple` | `sema_platform_token_matches_current` |
| `@[derive(Eq, Ord, Clone, Copy, Hashable, Debug, Display)]` | synthesize impls; `Hash` aliases Hashable; Copy is a marker when fields satisfy Copy | `host_analyze_synthesize_derive_methods` |
| `@[async]` | spawn stackless Future worker (`vio_sched_spawn`); MIR await-split (`poll` + `mir_term_yield`); `Task.sleep` timer | sema |
| `@[comptime]` | fold when `sema_ct_eval_known` succeeds (int/bool/char, unary/bin/ternary, loops, `print`/`ct_file_size`, `@[comptime]` calls) | `sema_try_fold_comptime_call` |
| `@[no_mangle]` `@[export]` `@[export_name]` `@[link_name]` | C ABI / symbol names | `sema_decl_is_c_abi_fn` |
| `@[repr(C)]` `@[packed]` `@[align(N)]` / `repr(… packed … align(N))` | layout flags | `hir_builder_decl_has_repr_c` |
| `@[borrow_args]` | compiler-internal; skip certain move checks | compiler sources |
| `@[version("x.y.z")]` `@[variant("tag")]` | module identity `Name@x.y.z["tag"]` | `policy_module_identity_from_meta` |
| `@[migrate(fromVer, fromField?, fromSig?, desc?)]` | versioned API/field migration | `sema.vyx` (see below) |
| `@[discard(fromVer, desc?)]` | current spelling is not callable; use `@fromVer` | `report_discarded_callable` |

Unknown attributes are stored, not a second language. Do not invent
`@[inline]` as an optimizer contract unless a probe exists.

## Versioned modules and migrate

Modules may carry `@[version("1.0.0")]` and `@[variant("_1")]`. Identity is
`Base@version["variant"]` (`policy_module_identity`). Call and member
selectors use the same `@ver["variant"]` suffix already parsed by
`parse_version_selector_suffix`.

`@[migrate(...)]` / `@[discard(...)]` with `fromVer` queue-import that
historical module (`sema_queue_migration_sources_decl_chain`). Sema then
treats same-base + same-variant identities as a version family
(`sema_versioned_module_pair`) and keeps the newest class while merging
history (`sema_merge_versioned_class_members`).

**Fields** (`sema_field_migrate_*`, `hir_builder_field_migrate_from_field`):

- `@[migrate(fromVer="1.0.0", fromField="n")]` on a field: `fromField` is a
  **lookup alias** for one storage slot. The old spelling must not become a
  second HIR/MIR field.
- `fromField` is only valid on `de_FIELD`. It requires `fromVer`. The named
  source field must exist in that history version (else **E2400**). Two
  current fields cannot migrate the same source field from the same version.
- `@[migrate(fromVer="…")]` **without** `fromField` marks an **added** field.
  It does not consume a historical slot just because the name matches.

**Callables:** `@[migrate(fromVer=…)]` on `fn` / method / constructor causes
the old module to be loaded so historical overloads remain visible. Default
lookup prefers the newer version; `name@1.0.0["_1"](…)` selects the old one.
`@[discard(fromVer=…)]` makes the current name a **E2400** error unless the
caller uses the historical selector (help text: `name@fromVer["variant"](…)`).

`fromSig=` and `desc=` appear in compiler tests as attribute text. Sema
**does not** read `fromSig` (no `policy_named_argument_value(..., "fromSig")`
in `bootstrap_compiler/`). Overload identity is the actual declarations after
version merge, not a signature-string matcher.

Probes: `tests/projects/versioned_module_import`,
`versioned_field_migrate_diagnostic`, `versioned_module_discard_diagnostic`.

## Modules, packages, FFI

- Public enums, constants, and mutable globals can be consumed across module
  boundaries. Import the defining module; an alias such as
  `use Status = Cross.GlobalEnum.Status;` also preserves the enum's identity.
  The [cross-module gate](../probes/gates/cross-module/README.md) covers enum
  variants and integer/string globals from a separately compiled producer.
- Generated `.vyi` interfaces for user generic modules carry a versioned
  template artifact so consumers can instantiate bodies without producer
  source files. Definition-module identity and private helper dependencies are
  retained; private helpers remain inaccessible to ordinary consumer code.
  Corrupt or unsupported artifacts are rejected. See the
  [generic interface gate](../probes/gates/generic_interfaces/README.md).
- Application import: `use std.collections;` (`use` also accepts `.`
  separators). `import` parses as the same `DE_USE` node and uses `::`.
  `use path::*` / `import path::*` stops at `*` (`parse_import_path`).
- Projects: `Vyx.toml` (`PACKAGE_MANIFEST.md`). Local `file://` registry
  exists; there is no remote HTTP registry.
- `extern "C"` and `cfn` describe C ABI calls and callbacks.
  `extern "dci"` imports native fact contracts, including supported C++ and Rust
  layouts, calls, lifecycles, and dispatch. Active Adapters request producer-side
  generic instantiation during a build; the Consumer uses closed facts.
  See [DCI](DCI_SPEC.md) and [Fact Semantic Ownership System](MOSP.md).
- `@[platform]` selects OS-facing std (`windows` vs `posix`).

## Async and comptime (subsets)

- `@[async]` / `await`: `Promise`/`Task` Future (`poll` / `await_value`).
  MIR await-split at `await` (`poll` + `mir_term_yield`); stackless pump
  requeues on Pending. `Task.sleep` / `Task.yield_now` are timer Futures.
  OS fibers remain only for `StartCoroutine`. Blocking `vio_sleep` stalls
  the scheduler.
- `@[comptime]`: fold when `sema_ct_eval_known` succeeds — integer/bool/char
  literals, unary/binary/ternary, `while`/`for`/`break`/`continue`,
  `print`/`println`, `ct_file_size`, and calls to `@[comptime]` functions whose
  arguments are known (function recursion depth cap 32). No heap interpreter.

## Parsed, not a user contract

These tokenize or parse but are **not** taught as available features:

`macro` / `bench` keywords, `concept` (parsed as interface-like; use
`where T: Trait`), `foreach` as a preferred spelling (use `for-in`),
`task` as a keyword (use `std.vio` `Task`), `when` / `is` (keyword kinds,
no parser productions found), `volatile` locals,
`T?` nullable types, `asm(…)` as a user assembler, `Delegate`/`Event`,
language-level C++ class takeover without DCI.

`fail Type.Variant` **is** parsed and typed. Prefer `Result`/`?` in tutorials.
`requires` is accepted as a `where` synonym on functions, not a C++-concepts
language. `repl` is a real `main` command (LLVM JIT via runtime), not a
missing stub.

## Implementation boundary

Current DCI and compiler performance acceptance uses AOT. The LLVM JIT REPL
exists, but JIT parity and pressure testing remain later work; AOT results do
not establish JIT support for the same operation.

Active tree: `bootstrap_compiler/` (lexer → parse → Sema → HIR → MIR →
LLVM). Archived C++ host: repo-root `src/`. Frozen host std: repo-root
`std/`. Canonical std: `bootstrap_compiler/std_packages/`.

Build and gates: [TESTING_GUIDE.md](TESTING_GUIDE.md). Lesson map:
[LANGUAGE_SURFACE.md](LANGUAGE_SURFACE.md).

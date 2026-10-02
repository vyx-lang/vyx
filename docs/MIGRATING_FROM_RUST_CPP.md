# Moving to Vyx from Rust or C++

[简体中文：快速迁移](快速迁移_Rust_CPP.md) · [Documentation index](README.md)

> This is a comparison map, not an ABI or ownership specification. For real
> projects, go by the [language design](设计文档_ZH.md), `TESTING_GUIDE.md`, and
> what the compiler actually accepts; for C / Rust / C++ interop, go by
> `DCI_SPEC_ZH.md`, the current headers, and the project regressions. Every
> claim below was measured with this repository's current compiler, but this
> page is a migration aid, not a complete feature list.

You already know Rust or C++, so this page does not start at "what is a
variable". It answers three questions:

1. **How do I write this in Vyx?** Each section opens with a mapping table.
2. **What actually changes when I switch?** The prose under each table says which differences reshape your code and which are just a different spelling.
3. **Why did Vyx choose this?** Some differences are deliberate trade-offs (methods must live in an `impl` or `class` body, `match` fallbacks are spelled `default` rather than `_`). Knowing the motive is what lets you decide on new ground instead of memorising rules.

Read it by scanning the tables for the mapping, then going back for the "why".
For the language itself, work through the basics and intermediate chapters in
order; this page does not repeat their lesson structure.

Three anchors before you start:

- **Value semantics are the default.** `class` and `struct` are inline records; assignment and argument passing copy bits. To share a heap object, say so with `Ref` / `Box`.
- **Failure lives in the type.** `Result<T, E>` plus `?`; no exceptions, no unwinding.
- **No GC and no hidden destruction.** Container heap storage is released with an explicit `destroy()`; `defer` and `drop()` decide **when** that call happens, not **what** it is.

---

## Basic syntax

| | Rust | C++ | Vyx |
|---|---|---|---|
| Immutable | `let x = 42;` | `const int x = 42;` | `let x = 42;` |
| Mutable | `let mut x = 0;` | `int x = 0;` | `var x = 0;` |
| Type annotation | `let x: i32 = 42;` | `int x = 42;` | `let x: i32 = 42;` |
| Function | `fn foo(x: i32) -> i32` | `int foo(int x)` | `fn foo(x: i32) -> i32` |
| Strings | `String` / `&str` | `std::string` / `string_view` | `String` / `str` |
| Print | `println!("{x}")` | `std::cout << x` | `print("${x}")` |
| Format | `format!("{} {}", a, b)` | `sprintf(buf, "%d %d", a, b)` | interpolation: `"${a} ${b}"` |

### Why `let` / `var` instead of `let` / `let mut`

Rust marks mutability with a modifier (`let mut`); C++ uses a type qualifier
(`const`). Vyx splits it into two keywords: `let` binds a name that cannot be
reassigned, `var` declares one that can.

The payoff is that **mutability is always the first word of the declaration.**
You never scroll back looking for a `mut`, and you never have to work out
whether a `const` is constraining an interface or an implementation. When you
get it wrong the compiler hands you the action, not just a code:

```text
t10.vyx:3:7: error: E3000: cannot assign to immutable binding 'a'
help: change `let` to `var` / `let mut`, or write `mut` on the parameter
```

### Why there is no `format!` / `sprintf`

Vyx folds formatting into the string literal itself:

```vyx
fn main() -> i32 {
    let a = 1;
    let b = 2;
    print("a=${a} b=${b}");
    return 0;
}
```

Output: `a=1 b=2`.

One design removes three problems at once: C's `sprintf` makes you manage a
temporary buffer; Rust's `format!` makes you count placeholders against
arguments; C's `%d` against a mismatched argument is undefined behaviour.
Interpolation is an expression, so type checking happens at compile time.

Two things differ from `println!` and are easy to trip over:

- **Arguments to `print` are concatenated with no separator, and a newline is appended.** To build one line out of several pieces you use interpolation, not commas.
- **Interpolation uses ordinary expression syntax.** String literals can appear directly inside `${...}`; their quotes do not need escaping for the outer string:

```vyx
print("has=${m.contains("a")}");
```

Write `\${...}` to print `${...}` literally.

### `str` and `String`

As in Rust these split into a borrowed view and an owned buffer, but the
reading is looser:

```vyx
var owned: String = "hi";   // owned
owned = owned + "!";        // concatenation produces a new String
let view: str = "abc";      // read-only view
print("${view.len}");       // length is the field len, not a len() method
```

Coming from C++, note the last line: **`str` length is the field `.len`.**
Writing `.length()` or `.size()` is a type error, not a program that runs and
prints the wrong number.

---

## Control flow

```text
// Vyx                          // Rust                         // C++
if (x > 0) {                   if x > 0 {                      if (x > 0) {
    print("pos");                   println!("pos");                cout << "pos";
} elif (x == 0) {              } else if x == 0 {              } else if (x == 0) {
    print("zero");                  println!("zero");               cout << "zero";
} else {                       } else {                        } else {
    print("neg");                   println!("neg");                cout << "neg";
}                              }                               }

for (i in 0..10) { }          for i in 0..10 { }              for (int i = 0; i < 10; i++) { }
for (c in text) { }           for c in text.chars() { }       for (char32_t c : decoded_text) { }
for (b in text.bytes()) { }   for b in text.bytes() { }       for (unsigned char b : text) { }

match (x) {                    match x {                       switch (x) {
    case 1 => { ... }              1 => { ... }                    case 1: ... break;
    case 2 => { ... }              2 => { ... }                    case 2: ... break;
    default => { ... }             _ => { ... }                    default: ...
}                              }                               }
```

### Parentheses are required, and `elif` is deliberate

The condition of `if` / `elif` / `while` / `for` **must be parenthesised**.
Omitting them is a parse error, not a lucky compile:

```text
t12.vyx:3:8: error: E0002: expected '(', got `x`
help: insert '(' before `x`
```

That is a deliberate cut. Once conditions carry parentheses, the ambiguity of
`if (a) - b` — the dangling-else and unary-operator cases — disappears at the
grammar level. No backtracking, and no style advice telling users to add
parentheses after the fact.

The second difference is `elif`: Vyx writes `} elif (...) {`, not `else if`.
Nesting depth therefore **does not grow with the number of branches** — five
branches stay five siblings instead of five indent levels.

### `match` is an expression; the fallback is `default`

```vyx
let code = match state {
    case 0 => 10,
    default => 20,
};
```

`match` produces a value, so it can stand in a `let`, a `return`, or an
argument position. An arm may be a block, and the block's tail expression is
that arm's value (no semicolon, as in Rust).

Payload-carrying arms bind with `case`, and an `if` suffix is a **guard**:

```vyx
enum State { Idle; Busy(i32); Done }

fn label(s: State) -> string {
    return match s {
        case Idle => "idle",
        case Busy(n) if n > 5 => "busy-high",
        case Busy(_) => "busy",
        case Done => "done",
    };
}
```

A guard that fails falls through to later arms, so order carries meaning:
`case Busy(n) if n > 5` has to precede `case Busy(_)`.

**Write `default` for the catch-all; Rust's bare top-level `_` arm is not
supported** (it produces a backend error rather than a diagnostic). A `_`
*inside* a variant pattern is fine, as in `case Busy(_)`.

### Character iteration and byte iteration are separate iterators

```text
for (c in text) { }           for c in text.chars() { }
for (b in text.bytes()) { }   for b in text.bytes() { }
```

`for (c in text)` walks **Unicode characters**; `for (b in text.bytes())` walks
**UTF-8 bytes**. The two must be stated explicitly. C++'s `for (char c : s)`
lets you process multibyte text one byte at a time without noticing; Vyx writes
"which unit am I iterating" into the syntax.

---

## Structs, methods, and classes

### Methods live in an `impl` or a `class` body

```vyx
struct Point { x: f64; y: f64; }

impl Point {
    fn distance(self: Point, other: Point) -> f64 {
        let dx = self.x - other.x;
        let dy = self.y - other.y;
        return (dx * dx + dy * dy);
    }
}

fn main() -> i32 {
    let a = Point { x: 0.0, y: 0.0 };
    let b = Point { x: 3.0, y: 4.0 };
    print("${a.distance(b)}");
    return 0;
}
```

Output: `25`.

**Note: the dotted form `fn Point.distance(...)` currently fails to parse.**
There are exactly two valid spellings — `impl Point { fn ... }`, or the method
written directly inside a `class Point { ... }` body. People arriving from C++
reach for `fn Type.method` by reflex; use one of the two forms above instead.

### Why `self` is an ordinary first parameter

`self: Point` is a normal first parameter, not a pre-bound keyword. That rule
buys something a migrant notices immediately: **value semantics versus
by-reference passing is visible in the signature.**

```text
self: Point        by value (a copy)
self: &Point       borrowed, read-only
self: &mut Point   borrowed, writable
```

In C++, `void f() const` tells you the method does not modify the object, but
whether members are values or references depends on the field declarations. In
Rust, `&self` says borrowed but not copied. Vyx puts "how many copies happen on
the way in" at the front of the signature — and in a value-semantics language
that is the expensive information.

Also: `struct` declarations separate fields with a **semicolon**, literals with
a **comma**:

```vyx
struct Point { x: f64; y: f64; }              // declaration: semicolons
let p = Point { x: 1.0, y: 2.0 };             // literal: commas
```

### Classes, inheritance, and dispatch

| | Rust | C++ | Vyx |
|---|---|---|---|
| Class | struct + `impl` | `class Foo { }` | `class Foo { public fn method() {} }` |
| Inheritance | composition / traits | `class B : A { }` | `class B : A { }` |
| Interface | `trait Drawable { }` | `class I { virtual ... }` | `trait Drawable { fn draw(); }` |
| Dynamic dispatch | `dyn Trait` (vtable) | `virtual` (vtable) | `dyn Trait` → vtable |

```vyx
class Base {
    public x: i32;
    public fn get(self) -> i32 { return self.x; }
}

class Child : Base {
    public y: i32;
    public override fn get(self) -> i32 { return self.x + self.y; }
}

class Left {
    public a: i32;
    public fn fa(self) -> i32 { return self.a; }
}

class Right {
    public c: i32;
    public fn fc(self) -> i32 { return self.c; }
}

class Mix : Left, Right {
    public b: i32;
    public fn sum(self) -> i32 { return self.fa() + self.b + self.fc(); }
}

fn main() -> i32 {
    let c = Child { x: 3, y: 4 };
    print("3+4=${c.get()}, x is still ${c.x}");
    let m = Mix { a: 1, c: 2, b: 4 };
    print("1+4+2=${m.sum()}");
    return 0;
}
```

Output: `3+4=7, x is still 3` and `1+4+2=7`.

**`class B : A` here is not C++ inheritance.** It is **layout concatenation**:
`A`'s fields are laid out first in declaration order, `B`'s own fields follow,
and the result is still one contiguous block that assigns and passes by
copying bits. That is why a single literal can supply parent and child fields
at once (`Child { x: 3, y: 4 }`), and why `Mix { a: 1, c: 2, b: 4 }` orders
fields as "all of Left, all of Right, then Mix's own".

This shapes the mental model you need to bring:

- No vtable, no object header, no hidden pointer indirection. **On a value type, `c.get()` is a statically determined direct call with zero overhead.**
- `override` must match an existing parent method, or it is rejected — a declaration has to be verifiable, not a comment for readers.
- Diamond inheritance (two parents sharing an ancestor) is a hard error: there is no unambiguous answer to which segment `self.a` means. The compiler rejects it rather than inventing an unpredictable selection rule.

Runtime "one variable holding several types" is a separate road:

```vyx
trait Shape { fn area(self) -> f64; }

struct Circle { radius: f64; }

impl Shape for Circle {
    fn area(self) -> f64 {
        return self.radius * self.radius * 3.14159;
    }
}
```

Here `circle.area()` is **static dispatch** — the implementation is fixed at
compile time. Real polymorphism is written explicitly as a trait object
(`dyn Shape`), and only then does a vtable appear. The takeaway for a migrant:
**capability (traits) and layout (inheritance) are two separate mechanisms in
Vyx**, unlike C++ where both live in one class hierarchy. "Circles and squares
can both compute area" no longer forces you to invent a common ancestor.

---

## Error handling

```text
// Vyx                          // Rust                         // C++
error MyErr { NotFound }       // choose or define a type        // no standard error type

fn risky() -> Result<i32, MyErr> { fn risky() -> Result<i32,E> { int risky() {
    if (bad) {                     if bad {                        if (bad)
        fail MyErr.NotFound;           return Err(E::Nf);              throw runtime_error("nf");
    }                              }                               return 42;
    return 42;                     Ok(42)                       }
}                              }

let v = risky()?;              let v = risky()?;               try { auto v = risky(); }
                                                               catch (...) { }
```

### A runnable minimal example

```vyx
error MyErr { NotFound }

fn risky(bad: bool) -> Result<i32, MyErr> {
    if (bad) { fail MyErr.NotFound; }
    return 42;
}

fn caller(bad: bool) -> Result<i32, MyErr> {
    let v = risky(bad)?;
    return v + 1;
}

fn main() -> i32 {
    match (caller(false)) {
        case Ok(v) => { print("ok=${v}"); }
        case Err(_) => { print("err"); }
    }
    match (caller(true)) {
        case Ok(v) => { print("ok=${v}"); }
        case Err(_) => { print("err"); }
    }
    return 0;
}
```

Output: `ok=43` and `err`.

### `fail` is a statement; the success path omits `Ok`

The correspondence with Rust is one line at a time:

| | Rust | Vyx |
|---|---|---|
| Error type | `enum E { Nf }` | `error MyErr { NotFound }` |
| Return failure | `return Err(E::Nf)` | `fail MyErr.NotFound;` |
| Return success | `Ok(42)` | `return 42;` |
| Propagate | `?` | `?` |
| Consume | `match` | `match` (`case Ok(v)` / `case Err(_)`) |

The two easiest mistakes:

- **`fail` is a statement, not an expression.** It ends the current function immediately with a failure, so an arm ending in `if (bad) { fail ...; }` needs no trailing `return`. Copying Rust's `return Err(...)` into Vyx is a type error.
- **The success path does not write `Ok(...)`.** In a function returning `-> Result<i32, MyErr>`, `return 42` is wrapped into `Ok` for you. Writing `return Ok(42)` yields a `Result` of a `Result`.

### Why not exceptions

Failure shows up in the signature: `-> Result<i32, MyErr>`. Every call site can
see that this function may fail, without guessing what it throws behind the
scenes. The cost is equally explicit: **an error must be handled or propagated**,
never silently ignored — which is the point.

C++'s `throw` escapes from arbitrary depth, leaving the caller to infer safety
from documentation and `noexcept` annotations; Vyx turns that inference into a
type check. The criterion for "return `Result` or `panic`" matches Rust:
**could the caller plausibly write correct handling code?** A malformed input
can be reported or replaced by the caller, so return `Result`; a broken internal
invariant leaves the caller powerless, so `panic`.

---

## Ownership and resource management

| | Rust | C++ | Vyx |
|---|---|---|---|
| Ownership | compiler-enforced | manual / convention | written into the type (`Ref` / `Box` / raw pointers) |
| Reference count | `Arc<T>` | `shared_ptr<T>` | `Ref::<T>.new(val)` / `clone()` |
| Exclusive heap value | `Box<T>` | `unique_ptr<T>` | `Box::<T>.new(val)` |
| Weak reference | `Weak<T>` | `weak_ptr<T>` | `Weak::<T>.of(strong)` (`std.ref`) |
| Scope cleanup | `Drop` | destructor | `fn drop()` |
| Deferred cleanup | scope guard crate | RAII helper | `defer { cleanup(); }` |

### What is deliberately absent

**Vyx has no borrow checker.** This is the first thing Rust developers look
for: no aliasing rules enforced at compile time, no "borrowed value does not
live long enough". Ownership here is a **choice written into the type**, not a
whole-program conclusion.

**Vyx has no GC and no implicit destruction.** This is the first thing C++ and
Java developers look for: a `class` instance is an ordinary value copied bit by
bit, with no "destruction point"; container heap storage is not released by
anyone on your behalf.

Together these fix the migration move: **decide who owns the heap block, then
write it down.**

### Three kinds of ownership

```text
Box   One key to the room. Hand it over and it is no longer yours.
Ref   Photocopiable door cards. Everyone points at the same room;
      the room is demolished when the last card is returned.
Weak  A photocopy stamped "void on expiry". It may already be gone
      by the time you call upgrade().
```

```vyx
use std.ref;

fn main() -> i32 {
    let shared = Ref::<i32>.new(42);
    let copy = shared.clone();
    copy.set(100);
    print("both cards see ${shared.deref()}");

    let weak = Weak::<i32>.of(shared);
    match (weak.upgrade()) {
        case Some(strong) => { print("room is alive: ${strong.deref()}"); }
        case None => { print("room is gone"); }
    }

    let unique = Box::<i32>.new(7);
    print("exclusive box holds ${unique.deref()}");
    return 0;
}
```

Output: `both cards see 100`, `room is alive: 100`, `exclusive box holds 7`.

The expectation that needs adjusting is `clone()`: **it is "one more door
card", not "copy the value".** So `copy.set(100)` making `shared.deref()` read
`100` is a direct consequence of the rule, not a coincidence. C++'s
`shared_ptr` copy also only bumps a counter, but the new name does not feel
like it can reach back and mutate the original; `Ref<T>`'s `clone()` is a
visible, deliberately written action.

`Weak` exists for the same reason as in Rust: two objects holding `Ref`s to
each other keep the count above zero forever, so one side has to hold `Weak` to
break the cycle.

### `defer`: separating *when* from *what*

`defer` registers a block against **scope exit**, not against a following
statement. The function runs it no matter which exit it leaves by, so cleanup
never has to be copied in front of every `return`.

**The order is last-in-first-out (LIFO):**

```vyx
fn main() -> i32 {
    defer { print("first registered"); }
    defer { print("second registered"); }
    print("body");
    return 0;
}
```

Output: `body`, `second registered`, `first registered`.

This is not arbitrary: resources naturally depend in the other direction. You
open a file and then take a lock on it, so the lock must be released before the
file closes. Writing in acquisition order makes "release order = mirror of
acquisition order" hold automatically.

Compared with C++ RAII, the difference is that **`defer` does not own
anything** — it only decides **when** the cleanup call happens. What you call
is still up to you. That is why container APIs requiring an explicit
`destroy()` need `defer` to be complete:

```vyx
let mut values = Vec::<i32>.new();
defer { values.destroy(); }
// ... both the normal path and an early return reach this
```

### The boundary with raw pointers

**The moment `*` or `rawptr` appears in a type, ownership stops covering for
you.** Developers from C/C++ will find that familiar; developers from Rust
should treat it as a safety boundary they have to remember themselves.

---

## Generics and containers

### Built-in operators need no bounds

```text
// Vyx                                      // Rust
fn max_of<T>(a: T, b: T) -> T               fn max_of<T: PartialOrd>(a: T, b: T) -> T
{                                            {
    if (a > b) { return a; }                     if a > b { a } else { b }
    return b;                                }
}
```

The Vyx side compiles as written:

```vyx
fn max_of<T>(a: T, b: T) -> T {
    if (a > b) { return a; }
    return b;
}

fn main() -> i32 {
    print("${max_of(3, 9)}");
    return 0;
}
```

Output: `9`.

Rust's `T: PartialOrd` is **mandatory**: without it, `>` inside `a > b` cannot
resolve to a trait implementation and compilation fails. Vyx currently
**instantiates built-in operators implicitly** — `+`, `>` and friends are
expanded per concrete type during monomorphisation, so unconstrained generics
compile.

This is not "Vyx is stronger than Rust"; it is the **same trade-off placed
differently**. Rust checks interface completeness at the generic definition
(what the caller reads is the whole precondition); Vyx pushes it to the
instantiation site (shorter to write, but errors surface during
monomorphisation). Knowing this is what stops you hunting for a generic error
in the wrong place.

### When `where` is genuinely required

The moment you need something **only the trait knows** — an associated constant
or an associated type — `where` stops being optional:

```vyx
trait Bounded {
    const MAX: i32;
}

class U8Like {
    public v: i32;
}

impl Bounded for U8Like {
    const MAX: i32 = 255;
}

fn clamp_max<T>(v: i32) -> i32 where T: Bounded {
    if (v > T::MAX) { return T::MAX; }
    return v;
}

fn main() -> i32 {
    print("clamped=${clamp_max::<U8Like>(300)}");
    return 0;
}
```

Output: `clamped=255`.

`T::MAX` can only come from the bound; the compiler has nothing else to infer
from. So in Vyx:

- Built-in operators only ⇒ no bound needed to compile. **But write one anyway**: it keeps the precondition in the signature, saving both reader and compiler a guess.
- `T::MAX`, associated types, or trait methods ⇒ `where` is required.

Note the call site `clamp_max::<U8Like>(300)`: as with `Vec::<i32>.new()`,
**type arguments can be given explicitly**. C++'s
`requires std::totally_ordered<T>` lines up with the Rust side of this
trade-off: bounds in the signature, checked before instantiation.

### Container APIs

| | Rust | C++ | Vyx |
|---|---|---|---|
| Dynamic array | `Vec<T>` | `vector<T>` | `Vec::<T>.new()` after `use std.collections;` |
| Hash map | `HashMap<K,V>` | `unordered_map<K,V>` | `Dict::<K, V>.new()` |
| Set | `HashSet<T>` | `unordered_set<T>` | `Set::<T>.new()` |
| Push | `v.push(x)` | `v.push_back(x)` | `v.push(x)` |
| Key insert | `m.insert(k, v)` | `m[k] = v` | `m.put(k, v)` |
| Index | `v[i]` | `v[i]` | `v[i]`, or `v.get(i)` |
| Length | `v.len()` | `v.size()` | `v.count()`, or the field `v.len` |

```vyx
use std.collections;

fn main() -> i32 {
    let mut v = Vec::<i32>.new();
    defer { v.destroy(); }
    v.push(3);
    v.push(9);
    print("count=${v.count()} first=${v.get(0)}");

    let mut m = Dict::<string, i32>.new();
    defer { m.destroy(); }
    m.put("a", 1);
    let has = m.contains("a");
    let value = m.get("a");
    print("has=${has} value=${value}");

    let mut s = Set::<i32>.new();
    defer { s.destroy(); }
    s.add(7);
    s.add(7);
    print("set size=${s.len}");
    return 0;
}
```

Output: `count=2 first=3`, `has=true value=1`, `set size=1`.

**Why the names differ.** The `::<T>` in `Vec::<T>.new()` is the **explicit
form of a type argument**, because `new()` takes no argument from which `T`
could be inferred — Rust infers from the return-type position, Vyx asks you to
write it. Likewise `Set` uses `add` (not `insert`) and `Dict` uses `put` (`set`
is an alias). These renames produce compile errors during a migration, but each
is a one-time fix that does not ripple into your structure.

### Three boundaries that affect correctness

**① Containers do not free themselves.** With no GC and no implicit
destruction, heap storage must be returned explicitly — `v.destroy()`, or
`defer { v.destroy(); }`. This is **the single easiest step to miss** when
coming from Rust or C++.

**② Length is available as a method and as a field, and it always means
element count.** On `Vec`, `count()` / `size()` / `length()` are synonyms and
the field `len` is the same data; `Set` only has the field `len`. **None of
them is capacity** — that is `capacity()`. Rust's `len()` also never meant
capacity, but mixing fields and methods is Vyx-specific and makes it easy to
write `s.count()` on a `Set` and collect a "no such function" error.

**③ Indexing infers its type from the container's element type.** On `Vec<T>`,
`v[i]` returns an element reference. Use `get(i)` or an explicit target type
when you need a value copy.

```vyx
let a: i32 = v[0];    // read an i32 value
v[0] = 99;            // modify the element
let b = v.get(0);     // read a value
let c = v[0];         // infer an element reference
print("c=${c}");     // print the element value
```

Direct printing and interpolation both read the referenced element. String
elements are printed using their actual length.

Iteration is uniform: `Vec`, `Set`, and `Dict` all implement `Iterable`, so
`for (x in container) { ... }` works directly.

---

## Concurrency

```vyx
use std.sync;
use std.vio;

@[async]
fn answer() -> i32 { vio_sleep(1); return 42; }

fn main() -> i32 {
    vio_start();
    let value = await answer();
    let mtx = Mutex::<i32>.new(value);
    mtx.lock();
    mtx.set(42);
    mtx.unlock();
    let result = mtx.get();
    mtx.destroy();
    vio_stop();
    print("result=${result}");
    return result - 42;
}
```

Output: `result=42`, exit code 0.

### `@[async]` and `await` are language features, not a library

**Asynchrony is an attribute plus a keyword**, not a `Future` method chain.
`@[async] fn` marks a function that may suspend; `await answer()` suspends the
current task at the call site.

The benefit is that **no runtime glue is required**: no `async fn` returning
`impl Future` type gymnastics, no executor to pick, no `Pin` in the error
messages. The cost is **coarser control**: you decide with `@[async]` which
functions may suspend, rather than with `.await` which step suspends.

`vio_start()` / `vio_stop()` bound the task runtime; async work executes
between them.

### Shared mutable state uses `Mutex<T>`, never `Ref<T>`

`Mutex::<T>.new(v)` puts the value and the lock together; `lock()` /
`unlock()` bracket the critical section and `get()` / `set()` read and write
it. Note that **`Ref<T>` is single-threaded reference counting, not a lock** —
using it for cross-thread sharing is a classic source of concurrency bugs.

When you would rather not bracket locks by hand, `std.sync` also provides
`RwLock`, `Condvar`, `Barrier`, `Once`, `Channel<T>`, and lock-free atomics:

```vyx
use std.sync;

fn main() -> i32 {
    let counter = atomic_new(0);
    atomic_add(counter, 3);
    print("now=${atomic_load(counter)}");
    atomic_destroy(counter);
    return 0;
}
```

Output: `now=3`.

### The psychological gap versus Rust and C++

Rust's ownership system forces cross-thread sharing through `Send` / `Sync`
compile-time gates. Vyx has no such gate: **safety of sharing is guaranteed by
the type you pick** (`Mutex<T>` is safe, `Ref<T>` is not). C++'s `std::thread`
also leaves the responsibility with the programmer, but the type system gives
no hint at all; Vyx's hint is "is this a `Mutex` or a `Ref` in my hand".

---

## C interoperability and builds

```vyx
extern "C" {
    fn abs(x: i32) -> i32;
}

fn main() -> i32 {
    print("${abs(-7)}");
    return 0;
}
```

Output: `7`.

### `cfn`: why C callbacks need their own type

```vyx
extern "C" {
    fn call_cb(cb: cfn(i32) -> i32, x: i32) -> i32;
}
```

**An ordinary Vyx `fn` value is a fat pointer (code pointer plus captured
environment); a C function pointer is a bare address.** The calling
conventions differ, so the two cannot substitute for each other. Pushing a `fn`
into a `cfn` slot is rejected (`E1000`); the conversion must be explicit.

That rule looks like "one more keyword". What it actually prevents is the most
insidious crash class: C calls through one address while Vyx passed two machine
words, so the extra word becomes the first argument — or the other way round,
and the callee reads garbage. **Writing the difference into the type is what
lets the compiler catch it for you.**

### `extern "dci"`: the boundary for C++ / Rust

`extern "C"` is narrow: POD layouts, the C calling convention, no lifetimes.
When you need **native C++ / Rust layouts, lifecycles, or dispatch**, use
`extern "dci"` with a producer contract, and let an Active Adapter request
producer-side instantiation of supported generics during a build.

See the [DCI reference](DCI_SPEC_ZH.md) for the contract model and the
[MOSP examples](MOSP_ZH.md#dci生产端事实与开放泛型) for original definitions,
declarations, and build configuration.

### Build commands

| | Rust | C++ | Vyx |
|---|---|---|---|
| Project file | `Cargo.toml` | `CMakeLists.txt` | `Vyx.toml` |
| New project | `cargo new` | manual | `vyxc new <dir>` |
| Build | `cargo build` | `cmake --build` | `vyxc build --target <name>` |
| Run one file | `cargo run` | manual | `vyxc --src=file app.vyx --run=aot` |
| Test | `cargo test` | `ctest` | `vyxc test` |
| Dependencies | crates.io | vcpkg / conan | `Vyx.toml` plus a local `file://` registry (`vyxc install` / `publish` / `search` / `lock`) |

`vyxc new` produces:

```text
proj1/Vyx.toml
proj1/src/main.vyx
```

```toml
[package]
name = "proj1"
version = "0.1.0"
entry = "src/main.vyx"

[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }

[target.proj1]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
```

`[target.<name>]` is where Vyx differs most from Cargo and CMake: **one package
can emit several targets** (executable, static library, dynamic library), each
declaring its own source set and kind. `vyxc build` builds every target in the
package; `--target <name>` builds one. Multi-artifact projects (library plus
examples plus a test binary) therefore do not need to be split into several
packages.

Dependency resolution goes through a **local `file://` registry**, not a public
network index. That makes toolchain behaviour identical on an intranet or
offline — which matters when porting a C++ project's vcpkg dependencies: stage
the corresponding packages in the local registry, then refer to them by name.
`lock` writes `Vyx.lock` to pin versions, the same role as `Cargo.lock`.

Finally, layer your troubleshooting instead of reading one wall of errors:

```text
vyxc --src=file app.vyx --stop-after-parse   # syntax (seconds)
vyxc --src=file app.vyx --stop-after-sema    # types and semantics
vyxc --src=file app.vyx --run=aot            # end to end
```

---

## In one line

> Treat this as a migration map, not a promise that Vyx replaces Rust or C++.

Four habits to change on purpose:

- **Mutability**: `let mut` / `const` → `let` / `var`.
- **Failure**: `throw` / `Err(...)` → `fail` + `?` (and no `Ok` on the success path).
- **Ownership**: `shared_ptr` / `Arc` / GC → explicit `Ref` / `Box` / `destroy()` / `defer`.
- **Method attachment**: `fn Type.method` → `impl Type { fn ... }` or a `class` body.

Ownership, ABI, and toolchain boundaries are defined by what the current
compiler builds and what the corresponding tests verify.

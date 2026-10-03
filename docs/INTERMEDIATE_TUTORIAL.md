# Vyx intermediate tutorial

[简体中文](进阶教程_ZH.md) · [Documentation](README.md) · [Getting started](TUTORIAL.md) · [Fact Semantic Ownership System core features](MOSP.md)

Lessons 11 through 20 turn the single-file programs from the basics into programs you can actually ship. The thread running through them is: **how data is organised** (containers, closures, generics), **how failure is expressed** (`match`, `Result`, `?`), and **how code is split up** (multi-file projects, C interoperability).

Each lesson still starts from the problem and then states the trade-off. Installing the SDK and commands like `vyxc --src=file main.vyx --run=aot` carry over unchanged from the getting-started tutorial.

## Lesson 11: collection containers

An array `[T; N]` has its length fixed at compile time. Real programs rarely know their data size ahead of time: the number of lines read in, the number of items a user enters. That calls for containers that can grow while the program runs.

```vyx
use std.collections;

fn main() -> i32 {
    // The type argument goes on the method: declare what the container holds first.
    var values = Vec::<i32>.new();
    values.push(20);
    values.push(22);
    print(values.get(0) + values.get(1));
    // Hand the container's storage back explicitly.
    values.destroy();
    return 0;
}
```

This prints `42`.

### Why `destroy()` is yours to write

A container holds heap storage. Vyx schedules no hidden reclamation, so that storage must be returned before the container goes out of scope.

This is not a politeness suggestion — **forgetting it is a leak**. In exchange, the release point is fully predictable: your code decides when memory comes back, not some background thread's schedule. The price is that this line cannot be skipped.

`defer` (lesson 10) fits this pairing exactly:

```vyx
var values = Vec::<i32>.new();
defer { values.destroy(); }
```

Now an early `return` in the middle still cleans up.

### What each container is for

| Container | Use it for | Key methods |
| --- | --- | --- |
| `Vec<T>` | An ordered sequence indexed by position | `push` `get` `count` `pop` |
| `Dict<K, V>` | Looking a value up by key | `put` `get` `contains` |
| `Set<T>` | Only caring whether something is present | `add` `contains` `remove` |

All three require `use std.collections;`, and all three need `destroy()`.

```vyx
var scores = Dict::<string, i32>.new();
scores.put("alice", 42);
if (scores.contains("alice")) { print(scores.get("alice")); }

var tags = Set::<string>.new();
tags.add("vyx");
if (tags.contains("vyx")) { print("member"); }

scores.destroy();
tags.destroy();
```

### Why growth is amortised O(1)

`push` looks like a single append, but when capacity runs out the container must allocate a larger block and move the existing elements. Seen on its own, that one call costs O(n); spread across every append before it, the cost is constant.

Two practical consequences follow:

- When you know the size in advance, `reserve` (or `ensure_capacity`) reduces moves to zero.
- Never assume the n-th `push` costs the same as the one before it — occasionally it costs far more.

### Common mistakes

- Using a container and forgetting `destroy()`, leaving the storage behind.
- Mixing up method names: `Set` uses `add`, not `insert`; `Dict` uses `put`, not `set`.

## Lesson 12: closures

Sometimes what you need to hand over is not a fixed piece of logic but "that logic, carrying the current context with it". Defining a named function plus a struct for that is far too much ceremony.

```vyx
fn main() -> i32 {
    let offset = 7;
    // |parameters| { body }; the body can use offset directly.
    let add_offset = |value: i32| { return value + offset; };
    print(add_offset(35));
    return 0;
}
```

This prints `42`.

### What a closure has that a function pointer does not

A plain function value can only get data through its parameters. A closure is different: it **captures** the names visible where it was written, so `add_offset` remembers `offset = 7` without the caller knowing anything about it.

That is why "add the current offset to each item" takes a few lines as a closure. With a function you would thread an extra context parameter through every layer.

### Capturing has a price

Whatever is captured must remain valid for **as long as the closure is used**. This matters most when what is captured is a reference: if the referenced value expires first, reading it through the closure is simply wrong.

A practical rule when writing closures: **if the captured context only lives briefly in the current scope, do not store a still-live closure somewhere outside it.**

### When to reach for a closure

- Callbacks: passing "what to do once the result arrives" as an argument.
- Small local logic used once, not worth a name.
- Higher-order calls that need to carry context.

Conversely, if a piece of logic will be reused, tested on its own, or called from many places, a named function is clearer. A closure earns its keep by being convenient, not by being general.

## Lesson 13: generics

The same operation, for different types, with an identical implementation. Copying it once per type is pure duplication — and later, one change becomes a chain of changes.

```vyx
fn identity<T>(value: T) -> T {
    return value;
}

// where declares the constraint T must satisfy.
fn add<T>(left: T, right: T) -> T where T: Add {
    return left + right;
}

fn main() -> i32 {
    // Supply the type argument explicitly.
    print(identity::<i32>(42));
    print(add::<i32>(20, 22));
    return 0;
}
```

Both print `42`.

### What `where` declares

The body of `add` writes `left + right`. `where T: Add` writes the premise "a `T` supports addition" into the signature: callers see the constraint, and the body may use `+` freely.

Worth stating precisely: a constraint is not an on/off switch that fails the build when omitted — the current implementation allows these operations without one. The value of writing it is **keeping the premise in the signature**: a reader does not have to dig into the body to work out what the generic actually requires, and an instantiation error is easier to locate later.

For capabilities like associated constants (lesson 37's `T::MAX`) the constraint is mandatory: without declaring `T: Bounded`, `T::MAX` does not resolve at all.

### When `::<...>` is required

Most of the time the type argument is inferable from the arguments, so the `::<i32>` in `identity::<i32>(42)` is optional. It becomes necessary when there is **nothing to infer from**, most notably generic containers: `Vec::<i32>.new()` has no argument that could tell you what goes inside, so you must say it.

### The cost: monomorphisation

Generics are not a runtime mechanism. Each instantiation with concrete types produces its own machine code — `add::<i32>` and `add::<i64>` are two separate functions.

The upside is no indirection and no boxing; it runs exactly like the hand-written version. The downside is binary size growing with the number of instantiations. Generics pay off with **few types and many call sites**.

### Common mistakes

- Omitting `::<T>` at a construction site where the compiler has nothing to infer from.
- Using an associated constant without declaring the corresponding trait in the bound.

## Lesson 14: `match`

A chain of `if (x == 0) ... else if (x == 1) ... else ...` can express the same thing, but it hides two facts from the reader: **whether every case is covered**, and **which value is actually being tested**.

```vyx
fn label(value: i32) -> string {
    return match value {
        case 0 => "idle",
        case 1 => "running",
        default => "unknown",
    };
}

fn main() -> i32 {
    print(label(1));
    return 0;
}
```

This prints `running`.

### `match` is an expression

Note the `return match ... ;` — `match` produces a value, so it works both as a statement and, as above, as something you hand back. Every arm must have a compatible value type, otherwise the whole expression has no defined type.

An arm may also be a block of statements:

```vyx
case 1 => { let text = "running"; text; }
```

The last expression in the block is the arm's value (note there is no `return` here, and no trailing `;`).

### Handling cases that carry data

Enums like `Result` carry data, and a pattern can pull it out:

```vyx
match (parse_plus_one("42")) {
    case Ok(value) => { print(value); }
    case Err(_) => { print("invalid number"); }
}
```

`case Ok(value)` binds the success value to `value`; the `_` in `case Err(_)` means "I do not care what is in here".

### Guards: extra conditions beyond the pattern

A pattern can be followed by a boolean condition:

```vyx
return match parse_number(input) {
    case Ok(v) if v > 0 => v,
    case Ok(_) => 0,
    case Err(_) => -1,
};
```

`case Ok(v) if v > 0` reads as "it is `Ok` **and** the value inside is greater than 0". When the guard fails, control falls through to the later arms, so order still matters.

### Current limits

- Write `default` for the catch-all. **A bare `_` as a top-level arm is not supported yet**; it produces a backend error rather than a clean diagnostic.
- The matched value is evaluated only once, so calling a function with side effects there is safe.
- Complete positive and diagnostic cases live in the [match expression gate](../probes/gates/match-expression/README.md).

## Lesson 15: strings

String work clusters tightly around a few actions: finding, slicing, and changing case.

```vyx
fn main() -> i32 {
    let text = "Hello, Vyx!";
    print(text.contains("Vyx"));
    print(text.substring(0, 5));
    print(text.to_upper());
    return 0;
}
```

This prints `true`, then `Hello`, then `HELLO, VYX!`.

### The common methods

| Method | Does | Returns |
| --- | --- | --- |
| `contains(s)` | Whether a substring occurs | `bool` |
| `index_of(s)` | Position of the first occurrence | An index; the not-found value follows the implementation's convention |
| `starts_with(s)` / `ends_with(s)` | Prefix / suffix test | `bool` |
| `substring(start, end)` | Extract a span | Text |
| `to_upper()` / `to_lower()` | Change case | Text |
| `len` | Length | An integer (a field, not a method) |

### `substring` is half-open too

`text.substring(0, 5)` pulls `"Hello"` out of `"Hello, Vyx!"` — indices 0 through 4, five characters. This is the same rule as the ranges in lesson 5: **the end point is excluded**.

The consistency pays off the same way it does for loops: "the first n" is `substring(0, n)`, and its length is simply `n`, with no minus-one arithmetic.

### Length is a field, not a method

A string's length is written `text.len`, with no parentheses. This is worth noticing during migration — in many languages the equivalent is a call, and writing it that way here fails at compile time.

### Interpolation is still the default tool

When several values have to become one sentence, interpolation beats manual concatenation and spares you the conversions in between:

```vyx
let name = "Vyx";
let score = 42;
print("${name}: ${score}");
```

## Lesson 16: errors — Result, Option, and `?`

"This operation can fail" is information that belongs in the **type**. Once it is there, callers see it in the signature, and the compiler holds you to not ignoring it.

```vyx
use std.core;

// Declare a set of named failure reasons.
error ParseError {
    InvalidNumber
}

fn parse_number(input: string) -> Result<i32, ParseError> {
    if (input == "42") { return 42; }
    // fail hands back an error value.
    fail ParseError.InvalidNumber;
}

fn parse_next(input: string) -> Result<i32, ParseError> {
    // ? unwraps on success, and returns the error immediately on failure.
    let value = parse_number(input)?;
    return value + 1;
}
```

`parse_next("42")` yields `Ok(43)`; `parse_next("bad")` yields `Err(ParseError.InvalidNumber)`.

### What `?` actually does

`parse_number(input)?` expands to: call it; on `Ok(v)` take `v` and continue; on `Err(e)` **return right there** with `Err(e)`, abandoning the rest of the function.

It replaces this:

```vyx
let value = match parse_number(input) {
    case Ok(v) => v,
    case Err(e) => { fail e; }
};
```

Nested a few levels deep, what `?` saves is not just lines but the visual noise of "how many frames does this error travel up through".

### Why returned values instead of exceptions

Failure is in the signature: `-> Result<i32, ParseError>` states plainly that this call may fail. Every call site can see it, and nobody has to guess whether some function throws behind the curtain.

The cost is equally direct: **an error must be handled or propagated**, never quietly ignored. That is the point.

### `Option<T>`: only "present" and "absent"

Some failures need no reason; you only care whether a result exists — a key hit, an optional field. Use `Option<T>` there rather than forcing in an error type you will never read.

### `panic` or `Result`

The test is the same as in lesson 9: **could the caller write a correct handler?** Malformed input can be reported to the user or replaced, so return a `Result`. A broken internal invariant leaves the caller with nothing to do, so `panic`.

### Common mistakes

- Treating a `Result` as a plain value and forgetting to unwrap it (`?` or `match`).
- Using `panic` where propagation is meant, turning a recoverable failure into a crash.

## Lesson 17: compound assignment and bit operations

In `x = x + 5` the name `x` appears twice. Once the left side grows complicated — an indexed element, say — writing it twice is two chances to get it wrong.

```vyx
fn main() -> i32 {
    var count = 10;
    count += 5;      // equivalent to count = count + 5
    count *= 2;
    print(count);
    print(0xFF & 0x0F);
    return 0;
}
```

This prints `30`, then `15`.

### Why `+=` is more than a shortcut

The left-hand expression is evaluated once and written once. With `values[index] += 1`, writing `values[index]` twice means the index computation can appear twice — both extra work and a chance for the two copies to disagree. Compound assignment removes that duplication at the language level.

The family also includes `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, and `>>=`.

### Bit operations

| Operator | Meaning |
| --- | --- |
| `&` | Bitwise AND |
| `|` | Bitwise OR |
| `^` | Bitwise XOR |
| `~` | Bitwise NOT |
| `<<` / `>>` | Shift left / right |

```vyx
print(0xFF & 0x0F);   // 15   keep the low nibble
print(1 << 4);        // 16   shift left by four
print(0xFF ^ 0x0F);   // 240  XOR flips the low nibble
```

Bit operations act directly on an integer's bits. Integer widths are fixed at compile time (lesson 3), so these map one-to-one onto machine instructions with no extra checks or conversions.

The same fixed width means bits shifted out are genuinely gone — **the type does not widen automatically**. When you need a wider result, convert the value to a wider type before shifting.

### Common mistakes

- Using `&` as logical AND. Conditions use `&&`; `&` is a bit operation.
- Relying on a shift amount at or beyond the width; the result is not guaranteed.

## Lesson 18: for-in forms

One loop construct, several different things to drive it with.

```vyx
fn main() -> i32 {
    let values: [i32; 3] = [10, 20, 30];
    var total: i32 = 0;
    // Iterate by index.
    for (index in 0..3) {
        total += values[index];
    }
    print(total);
    return 0;
}
```

This prints `60`.

### By index, or straight at the elements

Both get you the data; the difference is whether you need the **position**:

```vyx
for (index in 0..3) { total += values[index]; }   // when the index matters

for (value in values) { total += value; }         // when only the element matters
```

You need the index to write back into the array, to pick elements by position, or to look at neighbours. When none of those apply, do not introduce the index — one fewer variable and one fewer chance to run out of bounds.

Containers iterate the same way:

```vyx
use std.collections;

var values = Vec::<i32>.new();
values.push(10);
values.push(20);
for (v in values) { print(v); }
values.destroy();
```

### Ranges stay half-open

`for (index in 0..3)` visits 0, 1, 2. An array of length 3 is covered exactly by `0..3` — the rule from lesson 5 applied to arrays, and the place it earns its keep most often.

### `while`, `break`, and `continue` are unchanged

Use `while (condition)` when the number of rounds is unknown. `break` ends the loop and `continue` skips the rest of the current round. To leave an outer loop from inside a nested one, use the label from lesson 5.

## Lesson 19: multi-file projects and Vyx.toml

One file eventually runs out of room. What you need then is not merely "split the code up" but answers to three questions: **how files see each other**, **what is exposed**, and **how the build system knows which files to compile**.

Start with this layout:

```text
squares/
  Vyx.toml
  src/main.vyx
  src/math.vyx
```

`src/math.vyx`:

```vyx
module squares;

public fn square(value: i32) -> i32 {
    return value * value;
}
```

`src/main.vyx`:

```vyx
module squares;

public fn main() -> i32 {
    print(square(6));
    return 0;
}
```

`Vyx.toml`:

```toml
[package]
name = "squares"
version = "0.1.0"
entry = "src/main.vyx"

[build]
output_dir = "target"

[target.squares]
type = "executable"
entry = "src/main.vyx"
sources = ["src/math.vyx"]
```

From the project directory:

```sh
vyxc build --target squares
```

Run `.\target\squares.exe` on Windows or `./target/squares` on Linux; it prints `36`.

### Why both files declare the same `module` name

Files with the same module name belong to **one module**: they share a namespace, and references need no qualification at all — `main.vyx` simply calls `square(6)`.

This separates two different things: splitting one module across files, and splitting a project into several modules. The former is physical layout; only the latter creates a boundary. Splitting files by function without rushing to split modules is the right starting point for the vast majority of projects.

### Declare module boundaries with @[vis]

The example exposes `square` using `public`, which can also be written
`@[vis(world)]`. Use `@[vis(package)]` for a package helper, or `@[vis(in(...))]`
for selected modules. Keep `main` public as the build entry point.

```vyx
module squares.math;

@[vis(package)]
fn square(value: i32) -> i32 { return value * value; }

@[vis(in(squares.ui) + friend(squares_tests))]
fn debug_square(value: i32) -> i32 { return square(value); }
```

Here `squares` identifies the package, `squares.ui` is the allowed module tree,
and `squares_tests` is another allowed package. `tree` means module descendants,
not class inheritance. `+` combines scopes, `&` intersects them and `-` excludes
a scope. An explicit `@[vis(...)]` takes precedence over the `public` shorthand.
Splitting files within a module and importing another module are distinct boundaries.
See [lesson 8](TUTORIAL.md#lesson-8-classes-and-methods) for the full scope table
and [visibility_semantics](../tests/projects/visibility_semantics/src/api.vyx)
for a project example.

### Why the manifest lists `sources`

The build system does not guess which files belong to a target. `entry` and `sources` under `[target.squares]` are the **complete input list** — what gets compiled and what gets linked, with nothing left to discovery.

One extra line buys a build result that does not change when an unexpected file shows up in the directory.

### Going further

- `vyxc new`, full build settings, and dependencies: [Create and configure a project](PROJECTS.md).
- Every manifest field: [Package manifest](PACKAGE_MANIFEST.md).
- Independent modules, enums, and constants across module boundaries: [project guide](PROJECTS.md#importing-independent-modules) and the [cross-module gate](../probes/gates/cross-module/README.md).
- How a compiled generic library carries instantiated bodies in its `.vyi` interface: the [generic interfaces gate](../probes/gates/generic_interfaces/README.md).

## Lesson 20: C interoperability with extern

Existing C libraries do not have to be rewritten. Vyx can call them directly; the price is stating exactly what the function's ABI looks like.

```vyx
extern "C" {
    fn abs(x: i32) -> i32;
}

fn main() -> i32 {
    print(abs(-42));
    return 0;
}
```

This prints `42`.

### `extern "C"` declares a convention, not an implementation

An `extern "C"` block holds signatures only, never bodies. Its job is to tell the compiler that **this name is to be found using the C calling convention** — how arguments and return values are laid out, rather than Vyx's own rules.

The implementation comes from a linked library. So `extern "C"` is a **contract**: get the signature wrong and the compiler will not stop you; the problem surfaces at run time. That is exactly why types crossing a language boundary deserve extra care.

### Callbacks use `cfn`

When C expects a function pointer, use the `cfn(...) -> R` type. An ordinary Vyx `fn` value cannot stand in for a C callback, because the two calling conventions are not the same thing — `cfn` is how that difference gets written into the type.

### When DCI is the right tool instead

What `extern "C"` can express is the C ABI: functions, pointers, and by-value primitive types.

Once you need any of the following, the information exceeds what the C ABI can carry:

- C++ classes, inheritance, and virtual functions
- Objects with constructors, destructors, and lifetime semantics
- Cross-language generic instantiations

Those are expressed with [DCI](MOSP.md#dci), where the contract is generated by the producer and carries the real type layouts and symbols, and the consumer generates stubs from it. The producer is not limited to C++ — Rust supplies the same facts through its own adapter.

## What is next

- [Advanced features](ADVANCED_FEATURES.md): ownership and borrowing, async, traits, inheritance, compile-time capabilities, and runtime reflection.
- [Coming from Rust or C++](MIGRATING_FROM_RUST_CPP.md): map the syntax you already know onto Vyx.
- [Fact Semantic Ownership System core features](MOSP.md): version migration, reflection, cross-language interop, and dead code elimination.
- [Getting started](TUTORIAL.md): start here if the fundamentals need another pass.

Related cases: [tutorial_intermediate_core.vyx](../tests/cases/tutorial_intermediate_core.vyx) and
[tutorial_manifest](../tests/projects/tutorial_manifest/Vyx.toml).

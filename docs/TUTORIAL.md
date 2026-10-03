# Vyx tutorial

[简体中文](入门指南_ZH.md) · [Documentation](README.md) · [Fact Semantic Ownership System core features](MOSP.md)

Ten lessons, from the first executable file to classes and scope cleanup. Every lesson follows the same line: first the problem, then why the obvious approach falls short, then the Vyx form and the trade-off behind it.

By the end you should be able to write a few dozen lines of single-file code without looking things up, and to read every line of `tests/cases/tutorial_beginner_core.vyx` and `tests/cases/tutorial_beginner_surface.vyx`.

> **Tip** This tutorial assumes you already know roughly what variables, branches, loops, and functions are. No prior Vyx is needed, and no C, Rust, or C++ background either.

## Install the SDK

Solve "it runs at all" first; everything else depends on it. The Vyx SDK is a self-contained archive — extract it and go. No installer, and no need to clone the repository first.

Windows x86_64: [Download ZIP](https://github.com/vyx-lang/vyx/releases/latest/download/vyx-sdk-windows-x86_64-llvm22.zip)

```powershell
Expand-Archive .\vyx-sdk-windows-x86_64-llvm22.zip -DestinationPath .\vyx
$sdk = (Resolve-Path .\vyx\vyx-sdk-windows-x86_64-llvm22).Path
$env:PATH = "$sdk\bin;" + $env:PATH
vyxc --version
```

Linux x86_64: [Download tar.gz](https://github.com/vyx-lang/vyx/releases/latest/download/vyx-sdk-linux-x86_64-llvm22.tar.gz)

```bash
tar -xzf vyx-sdk-linux-x86_64-llvm22.tar.gz
export PATH="$PWD/vyx-sdk-linux-x86_64-llvm22/bin:$PATH"
vyxc --version
```

Both SDKs already bundle the LLVM backend, so there is nothing else to install before compiling.

### Why "add to PATH" instead of "run an installer"

`vyxc` is just an executable inside the SDK directory. It derives the location of `std`, `std_packages`, and the runtime from where it sits. Adding `bin` to `PATH` is what makes those relative positions complete. Two rules follow:

- Do not copy `vyxc.exe` somewhere else on its own — it will not find the standard library.
- Keep the extracted directory structure intact.

Those commands apply to the current terminal only. A new terminal needs them again, or put them in your shell profile. When `vyxc --version` prints a version, the location is right — that is the precondition for everything below.

## Compile and run

Save each complete example as a `.vyx` file and have the compiler build and run it in one step:

```sh
vyxc --src=file hello.vyx --run=aot
```

`--run=aot` means "compile a native program, then run it immediately". To keep an executable and run it later:

```powershell
# Windows
vyxc --src=file hello.vyx --emit=exe -o hello.exe
.\hello.exe
```

```bash
# Linux
vyxc --src=file hello.vyx --emit=exe -o hello
./hello
```

### Why the beginner lessons stay single-file

Compiling one file removes an entire layer of variables: no manifest, no targets, no dependency resolution. The input is one `.vyx` file; the output is either a program or a compiler error. Once the language itself is familiar, move on to multi-file projects (lesson 19) — you will have half as many suspects when something breaks.

For real work, `--emit=exe` is the better habit. The value of `--run=aot` is that it shortens the edit-run-observe loop to a minimum.

## Lesson 1: entry point and output

Every program has to answer two questions: which line runs first, and how results leave the program. In Vyx those are `main` and `print`.

```vyx
// Execution begins at main. -> i32 declares that it hands back a 32-bit integer.
fn main() -> i32 {
    // print writes each argument as text and appends a newline after the last one.
    print("Hello, Vyx!");
    // return 0 means "finished normally"; the value becomes the process exit code.
    return 0;
}
```

Taking the function apart:

- `fn` starts a function definition, followed by the name.
- The `()` after `main` means it takes no parameters.
- `-> i32` is the return type. **Whatever type you declare, you must actually return.**
- The body lives inside `{ }`, and each statement ends with `;`.

### Why the return type is `i32`, not "nothing"

Because `main`'s return value is not discarded: it becomes the process **exit code**. In a shell that is what `echo $?` prints (in PowerShell, `$LASTEXITCODE`). `0` means success; anything else means failure.

That convention is how a program talks to the world around it. Scripts, CI, and build systems decide whether your program failed by reading this number, not by parsing the text you printed.

### Two behaviours of `print`

- It writes each argument in order as text with **no separator between them**. `print("a", 1)` produces `a1`.
- It appends a **newline** after the last argument, so two `print` calls occupy two lines.

To join text, use string interpolation (lesson 2). To break lines, call `print` twice.

### Common mistakes

- **Forgetting `return`.** A function that declares `-> i32` must hand back an `i32`. This is a compile error, not a warning.
- Using single quotes for strings. Vyx strings use double quotes.
- Dropping the trailing `;`.

## Lesson 2: bindings and mutability

Most values in a program change. The real question is **which ones are allowed to**. If any name could be rewritten at any time, you cannot tell what it holds at a given line just by reading.

```vyx
fn main() -> i32 {
    // let declares a binding: the name can never point at anything else.
    let name = "Vyx";
    // var declares a binding that can be reassigned.
    var score: i32 = 20;
    score = score + 22;
    // Expressions inside ${...} are evaluated and then turned into text.
    print("${name}: ${score}");
    return 0;
}
```

This prints `Vyx: 42`.

### Why `let` is the default

Making immutability the default pays off on the reading side. Seeing `let`, you know the name refers to the same value for the whole scope and never need to scroll back hunting for a reassignment. Changing a value requires `var` explicitly, so **every mutable point is marked in the source**.

When you get it wrong, the compiler hands you the way out:

```text
error: E3000: cannot assign to immutable binding 'a'
note: N3000: change `let` to `var` / `let mut`, or write `mut` on the parameter
```

### `let` does not mean "the object is frozen"

This is the most common misunderstanding. `let counter = Counter(41)` says the **name** may not point elsewhere; the object it points at can still change:

```vyx
let counter = Counter(41);
counter.increment();   // legal: the object changes, not the binding
```

In C++ vocabulary this is closer to `T* const` than to `const T*`. The binding is read-only; the thing it refers to is not.

### When you can omit the type

The `: i32` in `var score: i32 = 20` is usually optional — the initializer `20` is enough for inference. You need to write it when:

- The initializer does not reveal the type, for example a declaration assigned later.
- The type you want differs from the inferred one.

Lesson 3 continues with types.

## Lesson 3: basic types and inference

"Number" is never one thing in a machine. Width, signedness, and floating-point representation decide the range you can express and what each operation costs. Vyx insists this be explicit in the source — written by you, or derived from the initializer.

```vyx
fn main() -> i32 {
    let count: i32 = 42;
    // as is an explicit numeric conversion.
    let total: i64 = count as i64;
    let ratio: f64 = 1.5;
    let ready: bool = true;
    // [T; N] is a fixed-length array; indices start at zero.
    let values: [i32; 3] = [10, 20, 30];
    print("${total}, ${ratio}, ${ready}, ${values[1]}");
    return 0;
}
```

This prints `42, 1.5, true, 20`.

### Why conversions must be written as `as`

`let total: i64 = count;` is not legal, even though widening `i32` to `i64` cannot lose data. The rule has to cover every case: narrowing `i64` to `i32` truncates, and converting a float to an integer drops the fraction. If only the "safe" conversions were implicit, readers would have to carry a mental list of which ones are free and which are not.

`as` costs three characters and buys you this: **every conversion that could lose information is visible in the source**.

### How to write literals

| Form | Meaning |
| --- | --- |
| `42` | Decimal |
| `0x2A` / `0b101010` / `0o52` | Hex / binary / octal — all equal 42 |
| `100i64` | Typed suffix; this one is explicitly 64-bit |
| `255u8` | Unsigned 8-bit, filling the range exactly |

### Common types

| Type | Notes |
| --- | --- |
| `i32` / `i64` | Signed integers; `i32` is the everyday default |
| `u32` / `u64` / `u8` | Unsigned integers, good for bit operations and bytes |
| `f32` / `f64` | Floating point; choose `f64` unless you have a reason |
| `bool` | `true` and `false` only |
| `string` | Text |

### Where inference stops

`let x = 20;` infers `i32`, because integers default to 32 bits. Where you need 64-bit arithmetic, write `20i64` or `let x: i64 = 20`. Integer overflow does not silently widen the type — that is not an oversight but the choice nearly every systems language makes: fix the width at compile time so operations map straight onto machine instructions.

## Lesson 4: conditional branches

Branching is the most basic way for one function to behave differently under different inputs.

```vyx
fn main() -> i32 {
    let temperature = 18;
    // The condition must be wrapped in parentheses.
    if (temperature < 20) {
        print("cool");
    } else {
        print("warm");
    }
    return 0;
}
```

This prints `cool`.

### Why the parentheses are mandatory

Drop them and the program does not compile:

```text
error: E0002: expected '(', got `temperature`
```

Two characters buy consistency: `if`, `while`, and `for` all take their condition in exactly the same shape. The parser never has to guess where an expression ends and a block begins, and you never have to reconstruct a precedence rule in your head. The templated form looks verbose but scans faster.

### Comparison and boolean operators

| Purpose | Operators |
| --- | --- |
| Equality / inequality | `==` `!=` |
| Ordering | `<` `<=` `>` `>=` |
| And / or / not | `&&` `||` `!` |

### Branches produce values

`if` is an expression, so it can be handed back as a value:

```vyx
fn choose(flag: i32) -> i32 {
    return if (flag == 1) { 10 } else { 20 };
}
```

For a plain either-or, `? :` is tighter:

```vyx
let mood = age > 20 ? "adult" : "child";
```

Both branches must have compatible types; otherwise the compiler cannot give the whole expression a type.

### Common mistakes

- Leaving out the parentheses, which triggers `E0002`.
- Writing an assignment where a comparison belongs: `if (a = 1)` is not a comparison; `==` is.

## Lesson 5: loops, `break`, and `continue`

A loop has to settle boundaries: where it starts, where it stops, and which rounds it skips. Vyx writes "where it stops" with a half-open range.

```vyx
fn main() -> i32 {
    var total: i64 = 0;
    // 0..6 is half-open: i takes 0, 1, 2, 3, 4, 5.
    for (i in 0..6) {
        if (i == 2) { continue; }   // skip the rest of this round
        if (i == 5) { break; }      // end the whole loop
        total = total + i;
    }
    print(total);
    return 0;
}
```

This prints `8`.

### Why the range excludes its end

`0..6` has exactly 6 values, and `0..n` has exactly `n`. Two things follow:

- Lengths need no arithmetic — the count is the number you wrote.
- Adjacent ranges tile cleanly: `0..3` and `3..6` neither overlap nor leave gaps, and together they cover `0..6`.

### Where 8 comes from

Lay the rounds out:

| `i` | What this round does | `total` |
| --- | --- | --- |
| 0 | add | 0 |
| 1 | add | 1 |
| 2 | `continue`, skip the add | 1 |
| 3 | add | 4 |
| 4 | add | 8 |
| 5 | `break`, loop ends | 8 |

`continue` skips only the rest of the current round; `break` ends the loop. The difference is precisely whether another round begins.

### Conditional loops and labelled `break`

When the number of rounds is unknown, use `while (condition) { ... }`. To leave an outer loop from inside a nested one, label it:

```vyx
'outer: while (labeled < 10) {
    while (true) {
        break 'outer;   // not just the inner loop
    }
}
```

An unlabelled `break` only affects the innermost loop. Past one level of nesting, indentation stops conveying which loop you meant; the label states it outright.

## Lesson 6: functions

The second time the same logic appears, it should become a function. A function is also **a boundary you draw for your future self**: the inside can change freely as long as the signature holds.

```vyx
// Parameters require types; b carries the default value 1.
fn add(a: i32, b: i32 = 1) -> i32 {
    return a + b;
}

fn main() -> i32 {
    print(add(41));              // default value -> 42
    print(add(b: 22, a: 20));    // named arguments, any order -> 42
    return 0;
}
```

### Why parameters must be typed

The signature is the only contract between modules. With types in the signature, a caller knows what to pass and what comes back without reading the body. The return type works the same way: `-> i32` is a promise.

A function with nothing to return omits `->`, as in `fn increment() { ... }`.

### What defaults and named arguments each solve

- **Default values** make the common call the shortest one: `add(41)`, instead of repeating an obvious value at every call site.
- **Named arguments** remove the burden of position. With several parameters, `f(w, h, x, y)` depends on remembering an order nobody remembers; written as `make_rect(w: 3, h: 4, x: 1, y: 2)` the call explains itself and the order stops mattering.

The two combine.

### Common mistakes

- Leaving a parameter untyped.
- Declaring a return type while some path returns nothing — the same error as the missing `return` in lesson 1.

## Lesson 7: structs

Data that always travels together should be bound into one type, instead of being passed around as parallel variables.

```vyx
struct Point {
    x: i32;
    y: i32;
}

fn main() -> i32 {
    // Build a value with Type { field: value }.
    let point = Point { x: 3, y: 4 };
    // Read fields with .
    print(point.x + point.y);
    return 0;
}
```

This prints `7`.

### Why `struct` exists

Consider doing without it: a point needs `point_x` and `point_y`; a rectangle needs four. As soon as they are passed as arguments the signature balloons into something like `make_rect(x, y, w, h)` — a form where the compiler cannot catch a swapped pair.

`struct` turns "these fields belong together" into a fact the type system knows. The signature shrinks to `fn area(rect: Rect)`, and swapped fields stop being possible.

### How it divides work with `class`

A `struct` packs data without methods or constructors. Its fields are accessible by default and may be restricted with `@[vis(...)]`. A `class` adds behaviour and encapsulation on top. The rule of thumb:

- If you only need to **carry several values as a unit**, use `struct`.
- If the type has **behaviour and invariants of its own**, or must interoperate with a C++ or Rust class, use `class` (lesson 8).

### Field declarations end with `;`

Note the difference from C and C++: fields are terminated with a semicolon, not a comma. In Vyx every *declaration* ends with `;`, and member declarations are no exception.

## Lesson 8: classes and methods

When data and behaviour have to be bound together, reach for `class`.

```vyx
class Counter {
    // public makes this field visible outside the class.
    public value: i32;

    // A constructor shares the class name and produces the object by returning an instance.
    public Counter(start: i32) {
        return Counter { value: start };
    }

    public fn increment() {
        // self refers to the current instance.
        self.value = self.value + 1;
    }
}

fn main() -> i32 {
    let counter = Counter(41);
    counter.increment();
    print(counter.value);
    return 0;
}
```

This prints `42`.

### Why the constructor returns an instance

There is no hidden "initialise `self`" magic in the constructor body. Instead you hand back an instance explicitly: `return Counter { value: start };`.

The benefit is that where an object comes from is visible in the code. The same type may skip a constructor entirely and be built from field values — `Counter { value: 0 }` — because both paths use the same construction syntax. There is no second, hidden rule.

### Visibility: @[vis]

Use `@[vis(scope)]` to specify which callers may access a declaration.
`public` is shorthand for `@[vis(world)]`. More specific scopes are also written
on the declaration. Vyx's visibility syntax does not use `protected`, `private`
or `internal` access levels.

```vyx
module counter.api;

@[vis(world)]
fn initial_value() -> i32 { return 41; }

@[vis(package)]
fn normalize(value: i32) -> i32 { return value < 0 ? 0 : value; }

@[vis(in(counter.ui) + friend(test_tools))]
fn reset_value() -> i32 { return 0; }
```

| Scope | Callers allowed to access the declaration |
|---|---|
| `world` | All callers; equivalent to `public` |
| `self` | Declarations in the current compilation unit, excluding importers |
| `mod` | The declaring module |
| `super` | The declaring module and its immediate parent |
| `tree` | The declaring module and its descendants |
| `package` | The same package, currently identified by the first module-path segment |
| `in(a.b, c.d)` | The named modules and their descendants |
| `friend(a, b)` | The named packages; arguments are first module-path segments |
| `none` | No callers |

Combine scopes with `+` for union, `&` for intersection and `-` for exclusion.
For example, `@[vis(world - in(counter.generated))]` exposes the declaration
except to that module tree. Use parentheses to group longer expressions.

An ordinary unannotated declaration defaults to `self`; a class can access
its own members. This is not C++ class-level `private`. Struct fields are
accessible by default; an explicit `@[vis(...)]` can restrict them.
See [lesson 19](INTERMEDIATE_TUTORIAL.md#lesson-19-multi-file-projects-and-vyxtoml)
for module boundaries.

### Methods can mutate while the binding stays `let`

`counter.increment()` changes `counter.value`, yet `counter` was declared with `let`. The two rules do not conflict:

- `let` constrains the **binding**, not the object it points at.
- What changes is state inside the object, which is the object's own business.

Reach for `var` only when you need to **repoint the binding itself**.

### Common mistakes

- Forgetting `public`, then failing to reach a field from outside.
- Forgetting to `return` the instance in a constructor.

## Lesson 9: preconditions and `panic`

Not every error can be handed back to the caller to solve. Some are calls that, logically, should never happen — passing zero as a divisor, for instance.

```vyx
fn divide(a: i32, b: i32) -> i32 {
    // The precondition is violated; computing on makes no sense.
    if (b == 0) {
        panic("divisor must not be zero");
    }
    return a / b;
}

fn main() -> i32 {
    print(divide(42, 2));
    return 0;
}
```

This prints `21`. Change the call to `divide(42, 0)` and the program prints `divisor must not be zero` and exits with a **non-zero code** — nothing after `panic` runs.

### How to split work between `panic` and returned errors

| Situation | Use |
| --- | --- |
| The caller can reasonably handle it: a missing file, malformed input | Return `Result` / `Option`, see lesson 16 |
| Continuing makes no sense: a broken invariant, an out-of-range argument | `panic` |

The test is **whether a correct handler is even writable**. A missing file can be retried or replaced, so return an error. A zero divisor leaves the caller with nothing to do but change the call, so do not push the burden onto it.

### One trap worth knowing

`panic` must be **written explicitly**. Integer division by zero does not trigger the `panic` inside `divide`:

```vyx
print(1 / 0);   // undefined result; do not expect it to report anything
```

So preconditions like "divisor is not zero" have to be checked by hand — that is the entire reason the `if` at the top of `divide` exists.

### Common mistakes

- Using `panic` as error handling, leaving callers no way to recover.
- Expecting code after `panic` to run as a fallback; it terminates the process.

## Lesson 10: `defer` and scope cleanup

If releasing a resource depends on human memory, it will be forgotten. Vyx puts "what to do when this scope exits" right next to the acquisition.

```vyx
fn main() -> i32 {
    defer { print("finished"); }
    print("working");
    return 0;
}
```

This prints `working`, then `finished`.

### Why last-in-first-out

Register two and watch the order:

```vyx
defer { print("first registered"); }
defer { print("second registered"); }
print("body");
```

The output is `body`, `second registered`, `first registered` — **the last registration runs first**.

That is not arbitrary. Resources naturally depend on earlier resources: you open a file and then take a lock on it, so the lock must be released before the file is closed. The LIFO order of `defer` makes the release order the mirror image of the acquisition order, so you can write code in acquisition order and never reorganise it backwards.

### Early `return` is covered too

`defer` hooks the **scope exit** event, not a particular statement. It runs no matter which exit the function leaves through, so cleanup never has to be copied in front of every `return`.

### Points to watch

- Anything used inside a `defer` block must still be valid when it runs.
- `defer` decides *when* a call happens, not *what* the right call is. Containers, for example, still require an explicit `destroy()` — `defer` helps you schedule it, it does not change it.

## Create your first project

Once a single file runs, the next step is turning it into a real project.

```sh
vyxc new hello_app
cd hello_app
vyxc build --target hello_app
```

Run `target/hello_app.exe` on Windows, or `./target/hello_app` on Linux.

The generated `Vyx.toml` describes the entry, output directory, targets, and dependencies — everything the build system needs to know. Starting from that template saves you a lot of the mistakes that come with hand-writing a manifest in an empty directory.

Follow [Create and configure a project](PROJECTS.md) to add source files, change build settings, and add another target. [Editors and debugging](TOOLING.md) covers plugin installation, LSP, and DAP.

## What is next

- [Intermediate tutorial](INTERMEDIATE_TUTORIAL.md): collections, closures, generics, `match`, error handling, and multi-file projects.
- [Advanced features](ADVANCED_FEATURES.md): ownership and borrowing, async, traits, comptime, inheritance, and runtime reflection.
- [Coming from Rust or C++](MIGRATING_FROM_RUST_CPP.md): map the syntax you already know onto Vyx.
- [Fact Semantic Ownership System core features](MOSP.md): Migrate, Reflection, DCI, DCE, and Effect.
- [Effect](MOSP_EFFECT.md): fact consumption, compiler consequences, and custom attributes.
- [Language surface](LANGUAGE_SURFACE.md): implemented syntax index.

Related language cases: [tutorial_beginner_core.vyx](../tests/cases/tutorial_beginner_core.vyx)
and [tutorial_beginner_surface.vyx](../tests/cases/tutorial_beginner_surface.vyx).

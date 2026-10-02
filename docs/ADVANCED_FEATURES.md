# Vyx advanced features

[简体中文：高级特性](高级特性_ZH.md) · [Documentation index](README.md)

## About this tutorial

The beginner lessons teach you to write programs that run; the intermediate ones teach you to organise data and represent failure. This chapter is about a different kind of question: **when you have to carry more responsibility yourself, which tools does the language give you, and where does each one stop.**

Concretely: ownership of heap objects, async, traits and runtime polymorphism, unsafe and C, `drop`, harder generics, compile-time capabilities, inheritance, platform gates, layout queries, and the Migrate, DCI, and reflection subsystems.

Examples use AOT. See [MOSP core features](MOSP.md) for the full treatment of Migrate, Reflection, DCI, and DCE, and [language design](DESIGN.md) for the language rules.

There are 27 sections; the thread runs in groups:

```text
Lessons 21–24   ownership on the heap, containers, closure captures, derive
Lesson 25       async: waiting without blocking everyone else
Lessons 26–29   enums, traits, dyn, pipes
Lessons 30–32   unsafe / C, drop, harder generics
Lessons 33–38   comptime, move, inheritance, interface, aliases, platform
Lessons 39–43   layout queries, operators, versions, DCI, reflection
```

---

## A safety map first

The most common source of confusion in the advanced material is treating **three entirely different boundaries as one thing**. Separate them first; every lesson after this is just a point on this map.

```text
Plain values, structs, Vec   cleanup follows the scope; defer / drop registers it
Box / Ref / Weak             heap objects: exclusive, shared, or "is it still there"
unsafe, rawptr, FFI          you prove it: non-null, aligned, nobody else is freeing
```

Mapped to tools:

- **Across threads**, use `Mutex<T>` for data several threads mutate.
- **Across C**, use `extern "C"`; **across other compiled languages**, use DCI contracts.
- **Never use `Ref` as a lock.** It governs "when does the house get torn down", not "who may be inside at the same time". This is the easiest trap on the whole map.

`&T` / `&mut T` are borrows, and the rules are two sentences: taking a `&mut T` while a shared borrow is still alive, or moving a local that is still borrowed, is E3101; assigning heap ownership is a move by default (E3100) unless the type is `Copy`.

Member access is written as if you owned the value — the compiler handles the indirection:

```vyx
fn bump(values: &mut Vec<i32>) {
    values[0] = values[0] + 1;
    values.push(9);
}
```

With an explicit `*T`, members are read through `->`, and doing so does not move ownership out. `rawptr` is an untyped unsafe pointer and enjoys none of these rules.

```vyx
class Pair { public value: i32; }
fn read_pair(p: *Pair) -> i32 { return p->value; }
```

Hold on to the dividing line: **once `*` or `rawptr` appears in the type, ownership checking stops covering for you.**

---

## Lesson 21: Ref, Weak, and Box

Start with the question. Who destroys a heap object? C++ hands the decision to documentation and convention, Java hands it to the GC, Python hands it to reference counting. Vyx writes the **ownership style** into the type so you choose it at the declaration:

```text
Box   one key to the room. Hand it over and it is no longer yours.
Ref   photocopied access cards. Everyone points at one room; the room is
      demolished when the last card comes back.
Weak  a photocopy that expires. upgrade() may find the room already gone.
```

```vyx
use std.ref;

fn main() -> i32 {
    let shared = Ref::<i32>.new(42);
    let copy = shared.clone();
    copy.set(100);
    print("两张卡看见同一个数：${shared.deref()}，强引用大约 ${shared.count()}");

    let weak = Weak::<i32>.of(shared);
    match (weak.upgrade()) {
        case Some(strong) => { print("房子还在：${strong.deref()}"); }
        case None => { print("房子没了"); }
    }

    let unique = Box::<i32>.new(7);
    print("独占盒里：${unique.deref()}");
    return 0;
}
```

### Why `clone()` adds a card instead of copying the value

If `clone()` produced an independent `i32`, then `copy.set(100)` could not affect `shared`, and `Ref` would have no reason to exist. **Sharing means exactly this: several paths lead to one piece of data, and a write through any of them is visible from the others.**

Once that is clear, `shared.deref()` printing `100` stops being a coincidence and becomes the direct consequence.

### Why `Weak` is needed

`Ref` has an unavoidable problem: if two objects hold `Ref`s to each other, neither count ever reaches zero and neither is ever demolished — a reference cycle. `Weak` exists for this. It is an **observer**: it does not extend the lifetime, and `upgrade()` tries to promote it to a strong reference, yielding `Option<Ref<T>>`. When it fails, the object is already gone.

### Cost and limits

- `Ref` / `Weak` are **single-threaded** reference counting, not locks. To mutate shared data across threads, use `Mutex<T>`; substituting `Ref` for a lock is a classic source of concurrency bugs.
- Counting is not free: every copy increments, every destruction decrements.
- Forget to break a cycle with `Weak` and you still leak. Reference counting answers "who demolishes", not "can it be demolished".

### Recap

`Box` is exclusive, `Ref` is shared, `Weak` may fail to upgrade. All three are about **ownership**, none of them about **concurrency**.

---

## Lesson 22: generic containers and iteration

A container has to be passable into a function and walkable by a loop.

```vyx
use std.collections;

fn sum(values: Vec<i32>) -> i32 {
    var total = 0;
    for (value in values) {
        total = total + value;
    }
    return total;
}

fn main() -> i32 {
    var values = Vec::<i32>.new();
    values.push(10);
    values.push(32);
    print("10+32=${sum(values)}");
    return 0;
}
```

### Do not change the structure while iterating

This rule deserves its own heading: **while `for (value in values)` is still running, do not `push` / `pop` / `set` / `clear` / `destroy`.**

The reason is that the iterator remembers both "how far along we are" and "where the underlying storage is". A `push` that triggers reallocation invalidates the remembered position, and the iterator carries on reading from a location that no longer means what it did. Its behaviour stops being something you can rely on.

This is the same trap as mutating a container while iterating it in C++, except here it is stated as an explicit rule: collect first, then modify, then iterate again.

### Why a container can be passed by value

`fn sum(values: Vec<i32>)` looks like it copies the whole container. It need not: the compiler can see the body never stores `values` anywhere else, so ownership of the underlying storage can be handed straight to the function.

### Recap

Pass a `Vec` into a function and accumulate with `for-in`; **never open the bag while walking through it**.

---

## Lesson 23: closure captures and higher-order calls

Lesson 12 introduced the pipe-style closure. Here is the other form, which puts the captured names on the outside:

```vyx
fn main() -> i32 {
    let plus7 = |x: i32| { return x + 7; };
    print("35+7=${plus7(35)}");

    let offset = 5;
    let add_offset = [offset](x: i32) => x + offset;
    print("37+5=${add_offset(37)}");
    return 0;
}
```

### Why an explicit capture list

`[offset](x: i32) => x + offset` writes what is captured inside the brackets. The benefit is that a reader does not have to dig through the body to learn **which outer variables this closure depends on**.

The more a closure travels, the more this matters: a call site sees `add_offset` and nothing about the `offset` tethered behind it. The capture list makes that hidden dependency explicit.

### Choosing between the two forms

| Form | Good for |
| --- | --- |
| `\|args\| { statements }` | Several statements in the body, or needing `return` |
| `[captures](args) => expression` | A single expression, with the capture surface written out |

Closures pass into other functions exactly like ordinary function values; both forms work.

### Recap

Write small functions on the spot; to carry values from the enclosing scope, name them in `[…]`.

---

## Lesson 24: `derive` methods

Equality, cloning, hashing, ordering, printing — the implementations are entirely determined by the fields, yet must be hand-written. Worse, they have to **stay consistent with each other**: add a field and forget to update `operator_eq`, and you have a type that claims to be equal when it is not.

`@[derive(...)]` hands the job to the compiler:

```vyx
use std.hash;
use std.clone;
use std.fmt;

@[derive(Eq, Clone, Hashable, Ord, Debug, Display)]
class Pair {
    public x: i64;
    public y: i64;
}

fn main() -> i32 {
    let a = Pair { x: 7, y: 9 };
    let b = a.clone();
    print("相等？${a.operator_eq(b)}");
    print("hash=${a.hash()} compare=${a.compare(b)}");
    return 0;
}
```

You should see equality hold, a non-zero hash, and a comparison result of 0.

### Why `Clone` is a per-field deep copy

`@[derive(Clone)]` calls `.clone()` on every field in turn. For a heap-backed field such as `Vec<T>`, that means **the underlying storage is copied too**, not shared by duplicating the pointer.

That choice is forced: sharing the pointer would leave two objects holding one allocation, and freeing from either leaves the other dangling. The deep copy is slower, but there is exactly one meaning.

Enums are rebuilt per variant via `match`; generic fields imply the corresponding trait bounds.

### Why pointer fields are rejected

`@[derive(Clone)]` refuses a type with a `rawptr` / `*T` field. The reason is direct: **the compiler cannot prove what copying that pointer means** — share the pointee, or copy it? There is no correct default, so you write `clone()` by hand and state the semantics.

### When not to derive

derive gives you **structural equality**: all fields equal means equal. If the domain says "same id means the same object" while other fields may differ, you must write it yourself — the derived version would answer wrongly.

`@[derive(Copy)]` has one extra effect: for a scalar-only class it suppresses E3100 on assignment (see lesson 34).

### Recap

derive synthesises from fields; `Clone` is deep; write pointer fields yourself.

---

## Lesson 25: async, `Task`, `Promise`, and VIO

Up to now every function has run straight through. Call `vio_sleep(1000)` and the whole program sleeps for a second — nobody else makes progress.

What you want is a different way to wait: let **this task** rest while the other tasks in the queue keep going.

```text
time →

slow:  [started]====sleep====[wakes, writes 1]
fast:  [started][immediately writes 2]

start both, then await ──► log becomes 21 (2 first, then 1)
await slow first, then start fast ──► log becomes 12 (nobody could cut in)
```

```vyx
use std.vio;

var log: i32 = 0;

@[async]
fn slow() -> i32 {
    await Task::<i32>.sleep(20);
    log = log * 10 + 1;
    print("slow 醒了");
    return 0;
}

@[async]
fn fast() -> i32 {
    log = log * 10 + 2;
    print("fast 做完了");
    return 0;
}

fn main() -> i32 {
    let a = slow();
    let b = fast();
    await a;
    await b;
    print("log=${log}");
    return 0;
}
```

You should see `fast 做完了` first, then `slow 醒了`, then `log=21`.

### Why starting before awaiting is what interleaves

The point is ordering, not syntax:

1. A function marked `@[async]` **does not run at the call site**; the call immediately produces a task (a future).
2. `let a = slow(); let b = fast();` queues both tasks.
3. `await a` is where the wait begins. While slow rests inside `Task.sleep`, fast gets to finish.

So changing `main` to `await slow(); await fast();` makes `log` become `12` — fast was never queued before slow finished, so there was nobody to cut in. **That is not a difference in the executor; you closed the concurrency window yourself.**

This also explains the awkward-looking `@[async] fn f() -> i32`: the signature says `i32`, but the call site receives a task, and only `await` produces the `i32`.

```vyx
use std.vio;

@[async]
fn delayed_add(a: i32, b: i32) -> i32 {
    await Task::<i32>.sleep(1);
    return a + b;
}

fn main() -> i32 {
    let sum = await delayed_add(20, 22);
    print("sum=${sum}");
    return 0;
}
```

Parameters and local variables preserve their state across `await`, including
assignments to mutable parameters declared as `mut value: i32`. Async return
types can also be `string` or aggregates; `await` produces that declared value.

### Giving up time: three ways to wait

| What you want | How | Can others run? |
| --- | --- | --- |
| Wait a while without stalling the scheduler | `await Task::<i32>.sleep(ms)` | Yes |
| Yield just this tick | `await Task::<i32>.yield_now()` | Yes |
| Put the thread to sleep | `vio_sleep(ms)` | No |

Worth memorising: **using `vio_sleep` where tasks exist is equivalent to pressing pause on the whole executor.**

`Task::<i32>.ready(2)` is an already-computed task; `Promise::<i32>.failed(7001)` is an already-failed result — ask `is_done`, `is_failed`, `error_code` first. `poll()` only answers "is it ready now"; it does not pump the queue.

`main` need not be `@[async]` — the top level is driven by the executor.

### The other road: `StartCoroutine`

`StartCoroutine` plus `yield WaitForNextTick()` uses operating-system fibers, which is a different mechanism from `@[async]` above. **Do not mix the two inside one function.** Only this road needs `vio_start()` / `vio_stop()`.

```vyx
use std.vio;

var steps: i32 = 0;

fn worker(ctx: rawptr) -> i32 {
    steps = 1;
    yield WaitForNextTick();
    steps = steps + 1;
    return 0;
}

fn main() -> i32 {
    vio_start();
    let handle = StartCoroutineHandle(worker as i64);
    if (!handle.is_valid()) {
        vio_stop();
        print("句柄无效");
        return 1;
    }
    StopCoroutineHandle(handle);
    vio_stop();
    print("steps=${steps}");
    return 0;
}
```

There is exactly one reason to choose it: you need **stack switching**. The price is portability — Android's Bionic has no `getcontext` / `swapcontext`, so `StartCoroutine` cannot switch fiber stacks there (it degrades to a no-op). `@[async]` is a stackless pump and does not depend on fibers, so it has no such problem.

### Under the hood

At each `await` inside `@[async]`, the compiler performs rustc-style **MIR splitting**: one `poll()`; if not ready, `mir_term_yield`; poll again on the way back. The executor is a stackless pump — while one task is Pending, its siblings can run to completion. `Task.sleep` and `Task.yield_now()` are timed futures.

### Recap

Start before awaiting or nothing interleaves; yield with `Task.sleep`, never `vio_sleep`; `StartCoroutine` is a separate mechanism.

---

## Lesson 26: enum state

Data with a handful of shapes is easiest to get wrong when expressed as "an integer plus a convention": the convention lives in documentation, the compiler cannot see it, and missing a case is never stopped.

```text
Signal::Stop          no extra data
Signal::Go(42)        this branch carries an i32
```

```vyx
enum Signal {
    Stop,
    Go(i32)
}

fn describe(signal: Signal) -> i32 {
    match (signal) {
        case Stop => { return 0; }
        case Go(value) => { return value; }
        default => { return -1; }
    }
}

fn main() -> i32 {
    print("Go(42) → ${describe(Signal::Go(42))}");
    print("Stop → ${describe(Signal::Stop)}");
    return 0;
}
```

### Why payload-carrying variants are better

`Go(i32)` stores the data **inside the branch**. What that buys is not syntactic sugar but the impossibility of expressing a wrong state: there is no object that claims to be `Stop` while still carrying a number, and no code that extracts a value from `Stop` — the compiler does not allow writing one.

An integer tag plus a parallel field has no such protection: tag and field falling out of sync is always possible.

### Why `default` is still there

`match` currently requires a catch-all arm (lesson 14 explained that a bare `_` is not supported yet), which is why `describe` includes `default`. Its role is to satisfy the language's current requirement, not the semantics.

Prefer payload-carrying variants and named patterns in business code, and **do not depend on variant declaration order** — order is an implementation detail, not part of the interface.

### Recap

Build with `Type::Variant(value)` and take apart with `case Variant(value)`. With the data inside the branch, invalid states cannot be written.

---

## Lesson 27: traits and `impl`

"Can compute an area" is a capability that circles and squares both have. They share no parent class, so expressing this with inheritance forces you to invent an ancestor that does not exist.

`trait` describes the capability directly, and `impl Trait for Type` supplies the answer for one type:

```vyx
trait Shape { fn area(self) -> f64; }

struct Circle { radius: f64; }

impl Shape for Circle {
    fn area(self) -> f64 {
        return self.radius * self.radius * 3.14159;
    }
}

fn main() -> i32 {
    let circle = Circle { radius: 2.0 };
    print("半径 2 的圆面积约 ${circle.area()}");
    return 0;
}
```

You should see roughly 12.57.

### Why traits beat inheritance here

`Circle` and `Square` are not in an "is-a" relationship; they merely "can both compute an area". Using inheritance to express that introduces a fake type hierarchy, mixing genuine parent-child relations (a button *is* a control) with pure capability sharing (both *can* compute an area) into one mechanism, where changes in one pull on the other.

Traits separate the two: **inheritance handles layout and data, traits handle capability.** A type may implement any number of traits with no layout cost, but it has only one parent (lesson 35).

### The current shape

This is still static dispatch: `circle.area()` picks the implementation at compile time, which is just a direct call with no overhead. The next lesson covers what to do when the compiler cannot tell whether the bag holds a circle or a square.

### Recap

A trait is a list of capabilities; an impl is one type's answer sheet.

---

## Lesson 28: dynamic trait objects

The previous lesson assumed the type was known at compile time. With circles and squares in one bag, that assumption is gone, and `dyn Shape` is needed: `area` is looked up at run time.

```vyx
use std.collections;

trait Shape { fn area(self) -> f64; }

struct Circle { radius: f64; }
struct Square { side: f64; }

impl Shape for Circle {
    fn area(self) -> f64 { return self.radius * self.radius * 3.14159; }
}
impl Shape for Square {
    fn area(self) -> f64 { return self.side * self.side; }
}

fn main() -> i32 {
    var shapes = Vec::<dyn Shape>.new();
    shapes.push(Circle { radius: 2.0 });
    shapes.push(Square { side: 3.0 });

    var total: f64 = 0.0;
    var index: i64 = 0;
    while (index < shapes.count()) {
        total = total + shapes.get(index).area();
        index = index + 1;
    }
    print("一圆一方，面积大约 ${total}");
    return 0;
}
```

Roughly 21.57.

### Why `dyn` is needed

`Vec<Circle>` holds circles only. To hold different implementations in one container, the element type has to degrade to "something that implements `Shape`" — which is what `dyn Shape` expresses. Each element then carries not only data but also "which `area` to call", so the call dispatches at run time.

### The cost

| Dimension | Static (`Circle`) | Dynamic (`dyn Shape`) |
| --- | --- | --- |
| Call | Direct, inlinable | Indirect, usually not inlinable |
| Element size | The data itself | Data plus type information |
| Decided | At compile time | At run time |

The conclusion is **try static first**. Introduce `dyn` only when the type genuinely cannot be known at compile time.

### One detail worth catching

`Vec.count()` is the number of elements. `Vec.size()` is a synonym alias, **not the capacity** — capacity is `capacity()`. Close names with different meanings, so worth a second look.

### Recap

`dyn Trait` puts different implementations in one container; calls dispatch at run time.

---

## Lesson 29: the pipe operator

`|>` feeds the value on the left into the **first** argument of the call on the right. It suits a chain of transforms that each take a single value.

```vyx
fn double(value: i64) -> i64 { return value * 2; }
fn add_ten(value: i64) -> i64 { return value + 10; }

fn main() -> i32 {
    let result = 16 |> double() |> add_ten();
    print("16 加倍再加十：${result}");
    return 0;
}
```

You should see `42`.

### Why it is worth writing

`16 |> double() |> add_ten()` means exactly the same as `add_ten(double(16))`; the only difference is reading order. A pipe reads **left to right**, the direction the data flows; nested calls read **inside out**, backwards.

The longer the chain, the bigger the difference. After three or four steps the nesting has more parentheses than anyone counts, while the pipe is still one step per line.

### The cost

The argument is implicitly pinned to the first position. If a function's main argument is not first, the pipe reads awkwardly — write the nested call instead.

### Recap

A pipe changes the reading order; the semantics remain a nested call.

---

## Lesson 30: unsafe and C FFI

The language that can be reached directly is still C. This lesson gathers the tools for dealing with it:

- `extern "C"` to declare functions
- `cfn(...)` for C function pointers
- `@[repr(C)]` / `packed` / `align(N)` to control layout
- `@[no_mangle]` / `@[link_name]` / `@[export_name]` / `@[link(name=)]` to control symbols

Interop with richer compiled languages goes through DCI in [lesson 42](#Lesson 42) rather than another flavour of `extern`.

```vyx
@[repr(C)]
class Point {
    public x: i32;
    public y: i32;
}

extern "C" {
    fn point_sum(p: Point) -> i32;
    fn call_cb(cb: cfn(i32) -> i32, x: i32) -> i32;
}

@[no_mangle]
public fn bump_c(x: i32) -> i32 { return x + 1; }

fn main() -> i32 {
    print("20+22=${point_sum(Point { x: 20, y: 22 })}");
    print("回调 +1：${call_cb(bump_c, 41)}");
    return 0;
}
```

Both steps need a C side that provides `point_sum` / `call_cb`.

### Why `@[repr(C)]` cannot be omitted

Vyx's own layout rules (alignment, field order, padding) are not C's. Passing `Point` by value into a C function means **both sides must agree on what those bytes mean**, otherwise C reads misaligned bytes. `@[repr(C)]` is the declaration "lay this type out by C's rules".

The compiler will not report this class of mistake, because from the C ABI's point of view the signature is perfectly legal. That is the typical shape of an unsafe boundary: **correctness is proved by you, not by the compiler.**

### Why `fn` cannot be a C callback

A Vyx `fn` and a C function pointer do not share a calling convention. A `cfn` slot rejects a fat `fn` (E1000); the conversion must be explicit.

C variadics (`...`) are written on the last parameter of an `extern "C"` declaration.

### Who is responsible for what

The caller handles bounds, null, alignment, and lifetimes — in one sentence: **every assumption across this line is yours to guarantee.** That is what makes it unsafe.

### Recap

C is the language FFI; layout uses `repr(C)`; callbacks use `cfn`.

---

## Lesson 31: `drop` and scope cleanup

Lesson 10's `defer` registers a block of statements. A class can go further: define `drop()`, called automatically when an instance leaves its scope. That fits resources with a definite lifetime — file handles, connections, heap objects.

```vyx
var resource_drops: i32 = 0;

class Resource {
    public id: i32;
    public fn drop() {
        resource_drops = resource_drops + 1;
        print("drop id=${self.id}");
    }
}

fn main() -> i32 {
    {
        let resource = Resource { id: 1 };
        print("还在用 ${resource.id}");
    }
    print("离开内层以后，drop 次数 ${resource_drops}");
    return 0;
}
```

You should see "还在用 1" first, then the drop, with a count of 1.

### Why "leaving the scope" rather than "returning from the function"

Note the inner `{ }` block. `drop()` hooks the **scope**, so leaving those braces is enough; the whole function need not end. That makes a scope the smallest unit of resource lifetime — to release earlier, wrap the usage in a pair of braces.

### Its relationship to `Vec.destroy()`

The two do not conflict, but their jobs differ:

- `drop` is the **language hook**: when it runs is decided by the scope.
- Containers still require an explicit `destroy()`: **what to release** is decided by the container's API.

Two rules follow: do not use it after destroying, and do not destroy twice.

### Recap

`drop()` follows the scope; standard-library containers additionally need `destroy()`.

---

## Lesson 32: generic enums and variadic generics

`Either<L, R>` says "either the left or the right, with possibly different payload types". `<...Ts>` is a type pack, and `args...+` folds addition over every argument — the operator must hold for each one.

```vyx
use std.core;

enum Either<L, R> {
    Left(L),
    Right(R),
}

fn unwrap_left<L, R>(value: Either<L, R>, fallback: L) -> L {
    match (value) {
        case Left(item) => { return item; }
        case Right(_) => { return fallback; }
    }
}

fn sum_all<...Ts>(...args: Ts) -> i64 {
    return args...+;
}

fn main() -> i32 {
    let left: Either<i32, string> = Either::<i32, string>.Left(42);
    let right: Either<i32, string> = Either::<i32, string>.Right("fallback");
    print("左值：${unwrap_left::<i32, string>(left, -1)}");
    match (right) {
        case Left(_) => { print("不该是左"); }
        case Right(_) => { print("右边是字符串"); }
    }
    print("10+20+12=${sum_all(10, 20, 12)}");
    return 0;
}
```

### Why `Either`

A function's failure path and success path are often different types — not "two states of one thing" but **two entirely different things**. Forcing them into a single type means inventing a struct with two fields of which only one is ever used, plus a convention about which one is valid. `Either<L, R>` turns that convention into the type itself.

### What variadic generics solve

`sum_all(10, 20, 12)` has a variable number of arguments, possibly of different types. `<...Ts>` declares a type pack and `args...+` folds over all of them.

The constraint is that **the operator must hold for every argument**: `+` requires every `Ts` to support addition. That constraint is checked at compile time; failing it fails the build.

Nested generics instantiate via turbofish or receiver arguments.

### Recap

Take generic enums apart with match; fold parameter packs with `...`.

---

## Nested records: `Outer<T>::Inner<U>`

A small type used by only one other type is better off inside it than flattened into the module scope — "who does this type belong to" is then obvious at a glance.

A `struct` / `class` may be written inside another record body, to any depth. Nested records are promoted to module scope under a **dotted name** (the `A` inside `FreqArray` is the record `FreqArray.A`). Their parameter list is the **parent's parameter list followed by their own**; when the two lists are **exactly identical**, that is treated as merely restating the parent's parameters and adds nothing.

```vyx
struct FreqArray<T> {
    struct A<T> { public a: T; }        // same list as the parent: adds nothing
    struct B<U> { public b: U; }        // adds U
    struct C<T, V> { public c1: T; public c2: V; }   // restates T, adds V
    struct D<K, V> { public d1: K; public d2: V; }   // all names are new
    public data: *T = null;
}

fn main() -> i32 {
    let a = FreqArray<i32>::A { a: 11 };                 // all arguments from the parent
    let b = FreqArray<i32>::B<f64> { b: 2.5 };           // parent first, then its own
    let c = FreqArray<i32>::C<i32, f64> { c1: 3, c2: 4.5 };
    let d = FreqArray<i32>::D<i64, f64> { d1: 5, d2: 6.5 };
    print("${a.a} ${b.b} ${c.c1} ${c.c2} ${d.d1} ${d.d2}");
    return 0;
}
```

### Why concatenation rather than implicit inheritance

Every use site is written as **"the parent segment's arguments, then the nested segment's"**, and the compiler concatenates them into the record's argument list.

That is why the third form (`C<T, V>`) has to write `<i32, f64>` in full — writing only `<f64>` disagrees with the parameter count and fails to compile (currently as a backend diagnostic, "no record layout", rather than an arity error in the source, but it does fail).

The rule looks verbose, and what it buys is the **absence of implicit capture**: anywhere you write `FreqArray<i32>::C<i32, f64>`, you can count the type's full argument list without looking outward.

The same rule applies in type position:

```vyx
struct Holder {
    c: FreqArray<i32>::C<i32, f64>;
    deep: L1<i32>::L2<f64>::L3<i8>;      // any depth; arguments concatenate level by level
}
```

Nested `class` follows the ordinary `class` rules: `public` members, methods, constructors.

```vyx
class Box<T> {
    class Inner {
        public v: i64;
        Inner(n: i64) { self.v = n; }
        public fn get(&mut self) -> i64 { return self.v; }
    }
    public t: T;
}

fn main() -> i32 {
    var b = Box<i32>::Inner(42);
    print("${b.get()}");
    return 0;
}
```

### Current limits

A nested record is just a **name** in module scope, which produces three consequences:

1. It must be declared before use.
2. Static members on a nested record cannot yet be reached through a qualified path — that path resolves to the record itself.
3. Writing a nested record inside the parent's own body requires the parent arguments in full (`FreqArray<T>::Inner<…>`, or the flat form `FreqArray.Inner::<…>`); there is no implicit capture of outer parameters.

Evidence: `probes/gates/lang/nested_records.sh` (five parameter-list shapes, a nested `class`, a depth chain, and a mandatory error on a mismatched argument count).

### Recap

Nested records are promoted to dotted names; the parameter list is parent first, own second, and identical lists add nothing.

---

## Lesson 33: `@[comptime]` folding

Some computations are already determined when you write the code, and putting them at run time is pure waste: lookup tables, constant expressions, branches on a file's size.

A `comptime` call with **constant integer** arguments is executed during compilation and folded to a literal.

```vyx
@[comptime]
fn triple(x: i32) -> i32 {
    let y = x + 1;
    if (y > 0) {
        return y * 3;
    }
    return 0;
}

@[comptime]
fn sum_to(n: i32) -> i32 {
    var i = 0;
    var acc = 0;
    while (i < n) {
        i += 1;
        acc += i;
    }
    return acc;
}

@[comptime]
fn io_ok() -> i32 {
    print("ct");
    if (ct_file_size("probes/gates/comptime/loop.vyx") > 0) {
        return 1;
    }
    return 0;
}

fn main() -> i32 {
    print("triple(4)=${comptime triple(4)}");
    print("1+…+4=${comptime sum_to(4)}");
    print("文件看得到？${comptime io_ok()}");
    return 0;
}
```

`triple(4)` is 15, `sum_to(4)` is 10; if the file is visible it prints `ct` and the third number is 1.

### Why `comptime` deserves to exist

It is not merely an optimisation. Capabilities like `ct_file_size(path)` **only exist at compile time** — at run time the program no longer knows what the build directory looks like. With it, configuration checks, branches on resource sizes, and generation-time validation all happen during compilation, failing the build instead of surfacing after deployment.

### What the interpreter can run

It executes `while` / C-style `for`, `break` / `continue`, `+=`, `print` / `println`, and `ct_file_size(path)` (the byte size of a file relative to the current working directory, or `-1` on failure).

**Heap allocation is still not interpreted** — that is the current boundary.

### Note

Compile from the repository root so the relative paths in the lesson resolve.

### Recap

`@[comptime] fn` plus constant arguments yields a compile-time result; no heap allocation.

---

## Lesson 34: move, `Copy`, and borrowing

Assigning heap ownership moves rather than copies. This rule makes "two owners of one heap allocation" impossible to express in the type system.

```vyx
use std.collections;

class Bag {
    public items: Vec<i32>;
}

fn main() -> i32 {
    var a = Bag { items: Vec::<i32>.new() };
    a.items.push(1);
    var b = a;
    print("袋子现在在 b 手里：${b.items.get(0)}");
    return 0;
}
```

Writing `a.items.get(0)` after `var b = a` is E3100.

### Why move rather than copy

`Bag` holds a `Vec` on the heap. A shallow copy in `var b = a` would leave both `Bag`s holding one `Vec`, and two `destroy()` calls would be a double free. A deep copy would make every assignment potentially an expensive allocation — and most of the time you do not need the duplicate.

move is the third option: **hand over ownership and leave the source unusable**. The cost is immediately visible (a compile error) and immediately fixable (if you really need two, `clone()`).

### `Copy`: types that may still be read

Scalars and scalar-only classes marked `@[derive(Copy)]` may still be read at the source:

```vyx
@[derive(Copy)]
class Tiny {
    public n: i32;
    public fn drop() {}
}

fn main() -> i32 {
    var a = Tiny { n: 1 };
    var b = a;
    print("两份都还在：${a.n} 和 ${b.n}");
    return 0;
}
```

The test is **whether a bitwise copy is equivalent to an independent value**. Integers and small pointer-width structs pass; types holding heap storage do not.

`string` / `String` is not `Copy`; ask for a second copy with `.clone()`.

### Borrowing

`&T` / `&mut T` are borrows. A conflicting `&` / `&mut` is E3101 — a mutable borrow cannot be taken while a shared borrow is alive, which makes "read half of it, then have it rewritten" impossible.

Receiver forms line up with Rust:

| Form | Meaning |
| --- | --- |
| Unmarked `self` | C++'s `this` |
| `&self` / `&mut self` | Shared / mutable borrow |
| `own self` | Takes ownership |
| `mut self` | Mutable binding |

`let mut` is equivalent to `var`. `unsafe { }` skips ownership diagnostics — that is the escape hatch, and the point where responsibility transfers to you.

### Recap

Anything with a heap moves; want two copies and use `clone` or `Copy`.

---

## Lesson 35: inheritance and `override`

A subclass places its parent's fields **first, in declaration order**, and `override` replaces a parent method. `struct` can concatenate fields the same way (`struct Vec3 : Vec2`).

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
    print("3+4=${c.get()}，字段 x 仍是 ${c.x}");
    let m = Mix { a: 1, c: 2, b: 4 };
    print("1+4+2=${m.sum()}");
    return 0;
}
```

### First, what it is not

**This is not a C++ vtable.** A `class` remains a value record: `Child { x: 3, y: 4 }` is one contiguous block, with `x` first and `y` after it. Inheritance here means **layout concatenation**, not runtime dispatch.

"One bag holding several types" at run time is `dyn Trait` (lesson 28). Do not conflate the two: **inheritance is layout, dyn is dispatch.**

Writing `class Mix : Left, Right` simply lays out Left's whole block, then Right's whole block, then Mix's own fields. That is why `Mix { a: 1, c: 2, b: 4 }` can satisfy both sides.

### Why diamond inheritance is a hard error

If two parents share an ancestor (a diamond), the compiler rejects it outright. The reason is a layout one: the same block of fields would be laid down twice, and then what does `self.a` refer to? There is no unambiguous answer.

Rather than invent a hard-to-predict selection rule, the compiler refuses at compile time. **Another case of making wrong code impossible to write.**

### `override` must match an existing parent method

An `override` with no corresponding parent method is rejected. Like the visibility rules, this is the same design principle: a declaration must be verifiable, not merely a comment for readers.

### Recap

`Child : Base` inherits fields; `Mix : Left, Right` concatenates two layouts in order. `override` must target an existing parent method. Diamond inheritance is a hard error.

---

## Lesson 36: `interface` default methods and super-interfaces

`interface`, `trait`, and `protocol` occupy the same keyword slot — three names for one thing.

An interface may carry default method bodies; `impl` fills in only the required ones:

```vyx
interface Greeter {
    fn greet(self) -> i32;
    fn hello(self) -> i32 { return self.greet() + 1; }
}

class En {
    public n: i32;
}

impl Greeter for En {
    fn greet(self) -> i32 { return self.n; }
}

fn main() -> i32 {
    let e = En { n: 6 };
    print("直接 hello=${e.hello()}");
    let d: dyn Greeter = e;
    print("dyn hello=${d.hello()}");
    return 0;
}
```

Both lines are `7`.

### What default methods solve

Without them, every implementor repeats the same logic — and those copies drift apart. Default methods put the "common way" into the interface itself, so implementors answer only the part that **genuinely varies by type**.

This is the same idea as `@[derive]` in a different shape: derive hands implementations entirely determined by fields to the compiler; default methods hand implementations mostly determined by the interface to the interface.

### Super-interfaces

`interface Drawable : Shape` requires both contracts to hold. The constraint "drawable" then already includes "has a shape", so a caller holding `dyn Drawable` can use `Shape`'s methods without a second bound.

### Default methods go through `dyn` too

In the example above, `d.hello()` dispatches through `dyn Greeter` — the dispatch point is at run time, while the default implementation itself is still the single compiled copy.

### Recap

Default methods live in the interface; impls fill the gaps; dyn can call defaults as well.

---

## Lesson 37: type aliases and associated `const`

`type Ptr<T> = *T` is just a shorter name. A `const MAX` on a trait is an associated constant, read as `T::MAX`.

```vyx
type Ptr<T> = *T;

trait Bounded {
    const MAX: i64;
    fn value(self) -> i64;
}

class U8Like {
    public v: i64;
    const MAX: i64 = 255;
    public fn value(self) -> i64 { return self.v; }
}

fn clamp<T>(x: T) -> i64 where T: Bounded {
    let v = x.value();
    if (v > T::MAX) { return T::MAX; }
    return v;
}

fn main() -> i32 {
    let n: i32 = 9;
    let p: Ptr<i32> = &n;
    print("别名指针看到 ${*p}");
    print("300 夹紧到 ${clamp::<U8Like>(U8Like { v: 300 })}");
    print("123 原样 ${clamp::<U8Like>(U8Like { v: 123 })}");
    return 0;
}
```

### Aliases are free

`Ptr<i32>` and `*i32` are the same type to the compiler; the alias only affects how the source reads. So the trade-off is pure: **worth it when it reads better, not worth it when it is just shorter.**

### What associated constants solve

`clamp` needs to test "this type's ceiling". Writing the ceiling as a global constant means one per type plus a guarantee that they never get mixed up.

An associated constant attaches the ceiling **to the type**: `T::MAX` reads the one `T` declared for itself. With `where T: Bounded`, the compiler knows any incoming `T` certainly has `MAX` and can use it freely.

The test is exactly the same as for `where T: Add` (lesson 13): **whatever the generic body uses must be promised in the constraint.**

### Recap

`type` introduces an alias; `T::MAX` reads an associated constant.

---

## Lesson 38: `@[platform]` gates

The same job differs per operating system: path separators, system calls, available APIs. Handling that with a runtime branch means both sides must compile — including the APIs that do not exist on the target.

`@[platform]` moves the choice to compile time: **the same name keeps one implementation per target OS, and Sema drops the non-matching ones**, so there is no duplicate-name clash.

```vyx
@[platform("posix")]
fn path_sep() -> i32 { return 47; }

@[platform("windows")]
fn path_sep() -> i32 { return 92; }

fn main() -> i32 {
    print("路径分隔符的字节是 ${path_sep()}");
    return 0;
}
```

`47` (`/`) on POSIX, `92` (`\`) on Windows.

### Available gates

- `posix`: Linux, Android, macOS
- `linux`: Linux only (`io_uring` and x86_64 `syscallN` take this route)
- `windows`: Windows

### Why dropping at Sema matters

Left to run time, the non-matching implementation would still have to **pass type checking and take part in compilation**. And it typically references symbols that do not exist on the target — which fails the build outright.

Discarding non-matching declarations at Sema says "this code does not exist on this target", and the problem disappears at the root. The price is that **the compiler only checks the current target**: a mistake in one platform's implementation is found only when that platform is compiled. Platform gates therefore need each target compiled in CI.

Cross-linking Android from Windows is covered in the [testing guide](TESTING_GUIDE.md#cross-compiling-windows-host).

### Recap

`@[platform]` picks one declaration per target; the other does not exist.

---

## Lesson 39: `sizeof`, `alignof`, and `static_assert`

These are **type queries** folded to `i64`, not a C preprocessor. `repr_sizeof::<T>()` belongs to the same family.

```vyx
fn main() -> i32 {
    static_assert(1 + 1 == 2);
    static_assert(sizeof::<i32>() == 4, "i32 is 4 bytes");
    print("i32 大小 ${sizeof::<i32>()} 对齐 ${alignof::<i32>()}");
    print("i64 大小 ${sizeof::<i64>()}");
    return 0;
}
```

### Why `static_assert` rather than a runtime assertion

Once a layout assumption breaks, the program fails in ways that are hard to diagnose: wrong field read, out-of-bounds access, ABI mismatch. Such assumptions **can be verified at compile time**, and leaving them to run time defers a certain failure to its hardest-to-debug moment.

`static_assert` checks during compilation and fails the build — and together with the message in the second argument, it writes "what I assume about this memory" directly into the code.

Incidentally, `sizeof` and `alignof` are queries rather than macros, so they follow ordinary type-system rules and work inside generics.

### Recap

`sizeof` / `alignof` / `static_assert` all speak at compile time.

---

## Lesson 40: operator methods

You can give a class `operator+`, `operator[]`, and friends, so a user-defined type reads like a built-in one.

```vyx
class SymbolBox {
    public value: i32;

    public fn operator[](self, idx: i32) -> i32 {
        return self.value + idx;
    }

    public fn operator+(self, other: SymbolBox) -> SymbolBox {
        return SymbolBox { value: self.value + other.value };
    }
}

fn main() -> i32 {
    let a = SymbolBox { value: 4 };
    let b = SymbolBox { value: 9 };
    let c = a + b;
    print("a[3]=${a[3]}，a+b 的 value=${c.value}");
    return 0;
}
```

You should see `7` and `13`.

### Operators are just method names

`operator+` is merely a method name, and `a + b` is parsed as a call to it. That means the rules are identical to any other method: it can borrow, return a new value, and be constrained by a trait.

**An integer's own `+` does not take this path** — built-in operations still map directly onto machine instructions and cannot be hijacked by anything a user defines. That matters: it guarantees the performance and behaviour of fundamental operations do not change because of what you implemented.

### When to write one

Operators suit operations with **no ambiguity in meaning**: `+` for combining, `[]` for indexing. If the meaning needs explaining (`+` — add, or concatenate?), an ordinary method name is clearer. The payoff of a custom operator is readability, and an unclear meaning cancels it out.

### Recap

Operators are ordinary method names; they apply only to types you have overloaded.

---

## Lesson 41: versioned modules and migrate

Library interfaces change, but callers already compiled against them cannot all be broken at once.

`@[version]` and `@[variant]` mark module versions, and `Vyx.lock` selects the default. `@[migrate]` links a current declaration to a historical one: `fromSig` names the historical function signature and `fromField` describes the field mapping. An explicit historical call is written `quote@1.0.0["standard"](3)`.

### Why it is needed

The ordinary approach is "change the interface, then update every caller". That does not work when the library and its callers are maintained by different people, or when binary artifacts are involved.

Versioning keeps the old signatures around so existing callers keep working against the old contract while new callers use the new one. `@[migrate]` is what makes the correspondence between old and new **explicit**, instead of leaving the compiler to guess it.

The cost is a wider maintenance surface: the project must supply every referenced historical version, and those declarations have to stay in place.

### Further reading

A complete multi-file example and build commands: [MOSP: Migrate](MOSP.md#migrate).
Field, constructor, and method examples: [tutorial_migrate](../tests/projects/tutorial_migrate/src/main.vyx).

---

## Lesson 42: DCI — compiled-language interop

What `extern "C"` can express is the C ABI. Once the other side is a C++ class, a Rust trait, or an object with lifetime semantics, the information to convey exceeds what a function signature can hold: **type layouts, calling conventions, lifetimes, generic instantiations**.

DCI describes those through a facts contract produced by the producer. Vyx consumes external declarations with `@[dci_import("path.dcib")]` and `extern "dci"`; the producer's semantics and generic constraints remain the producer compiler's business.

### Why the producer issues the facts

Handing the definition of layouts and symbols to **the side that actually owns the type** is DCI's central choice.

If the consumer guesses — or a human copies the header by hand — then the day the producer's layout changes and the consumer has not caught up, the failure appears at run time in the ugliest possible form. Generating the contract from the producer at build time, and generating stubs from that contract on the consumer side, means both sides work from the same facts.

Both C++ and Rust have active-adapter open-generics paths. Generic functions close their request at the call site and the producer materialises them during its build; the generic record instance layouts code generation needs must already be present in the contract.

The exact supported surface depends on the adapter, the target, and the operation — **the limits of the older offline adapters must not be generalised to DCI as a whole.**

### Further reading

The full original definitions, Vyx declarations, and configuration: [MOSP: DCI](MOSP.md#dci).
A runnable project: [dci_opengeneric](../tests/projects/dci_opengeneric/README.md); the protocol: [DCI specification](DCI_SPEC.md).

The current AOT regression also covers raw C++ `std::vector<T>` templates, Cargo ecosystem crates, native ICU DLLs, and `shared_abi` propagation with scope cleanup. Adapter commands and target limits are in the [SDK guide](../tools/dci/README.md); the required regressions are in the [verification guide](TESTING_GUIDE.md). JIT parity is still to come.

---

## Lesson 43: runtime reflection

Anything decided at compile time needs no reflection. Reflection handles the other half: **types and members not yet known when the code is written** — plugins looking things up by name, generic serialisation, debugging tools.

Use `@[reflect]` to register a type or member and `@[hidden]` to exclude a member. `std.reflect` provides lookup by name, binding an instance, property access, and typed method calls.

### Why registration is explicit

If every type were reflectable by default, then **every type's metadata would have to be retained** — including those that exist only to pass data around internally. That works directly against what DCE is for.

`@[reflect]` turns "this type must be visible at run time" into an explicit declaration, from which the compiler decides what to keep; `@[hidden]` subtracts further within one type.

One linkage to remember follows from this: **runtime reflection depends on retained metadata and code, and the relevant entry points feed into DCE's root set.** Choose reflection and you accept that this code will not be pruned.

### Further reading

A complete example: [MOSP: Reflection](MOSP.md#reflection).
More API: [tutorial_reflect.vyx](../tests/cases/tutorial_reflect.vyx).
Pruning rules: [MOSP: DCE](MOSP.md#dce).

## What is next

That is the end of the advanced lessons. For going deeper, these are worth keeping at hand:

- [Language surface](LANGUAGE_SURFACE.md): an index of the implemented syntax.
- [Language design](DESIGN.md): the reasoning behind the language rules.
- [DCI specification](DCI_SPEC.md): the complete cross-language contract protocol.
- [MOSP core features](MOSP.md): the full treatment of Migrate, Reflection, DCI, and DCE.

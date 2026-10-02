# Vyx standard-library tutorial

[简体中文：标准库教程](标准库教程_ZH.md) · [Documentation index](README.md)

## Using this guide

This guide covers collections, strings, resource management, concurrency, and reflection by module.
See the [standard-library reference](STD_LIBRARY.md) for full APIs and the
[beginner tutorial](TUTORIAL.md#compile-and-run) for execution commands.
See [MOSP](MOSP.md) for reflection and the other core features.
To create a multi-file application, follow the [project guide](PROJECTS.md);
its fields are defined in the [manifest reference](PACKAGE_MANIFEST.md).

```text
collections   vectors, maps, sets           → lessons 11, 22
string        view vs own                → lesson 15
ref           Box / Ref / Weak           → lesson 21
sync          locks                      → not the same as Ref
vio           tasks, sleep, the loop     → lesson 25
reflect       types by name              → lesson 43
```

---

## 1. `collections`: `Vec`, `Dict`, `Set`

Collections are introduced in [lesson 11](INTERMEDIATE_TUTORIAL.md#lesson-11-collection-containers). This is one file you can run as a recap.

The checked API is `push` / `get` / `set` / `pop`. `*_unchecked` hands capacity, bounds, destruction, and iterator invalidation to you. Do not change structure while iterating. Call `destroy()` when finished.

```vyx
use std.collections;

fn main() -> i32 {
    var values = Vec::<i32>.new();
    values.push(10);
    values.push(32);
    print("two slots: ${values.count()} first ${values.get(0)}");
    values.set(1, 33);
    print("pop ${values.pop()}");
    values.destroy();

    var scores = Dict::<string, i32>.new();
    scores.put("alice", 42);
    print("alice=${scores.get("alice")} present? ${scores.contains("alice")}");
    scores.destroy();

    var seen = Set::<i32>.new();
    seen.add(7);
    seen.add(7);
    print("7 in the set? ${seen.contains(7)}");
    seen.destroy();
    return 0;
}
```

---

## 2. `string`: `str`, `String`, and iteration

`str` views; `String` owns. `for-in` walks Unicode scalars; `.bytes()` walks UTF-8. Do not modify or destroy the source while a cursor lives. Full story: [lesson 15](INTERMEDIATE_TUTORIAL.md#lesson-15-strings).

```vyx
use std.string;

fn main() -> i32 {
    let view: str = "A¢€𐍈";
    var chars: i64 = 0;
    for (_c in view) { chars += 1; }
    var bytes: i64 = 0;
    for (_b in view.bytes()) { bytes += 1; }
    print("${chars} characters, ${bytes} bytes");

    var text = String.from("hello");
    var sum: i64 = 0;
    for (c in text) { sum += c as i64; }
    let borrowed: str = text.as_str();
    print("as_str still ${borrowed}, scalar sum ${sum}");
    text.destroy();
    return 0;
}
```

The five scalars in `hello` sum to 532.

---

## 3. `Ref`, `Weak`, `Box`

Defined in `std.ref`. Picture: [lesson 21](ADVANCED_FEATURES.md#lesson-21-ref-weak-and-box).

```vyx
use std.ref;

fn main() -> i32 {
    let a = Ref::<i32>.new(10);
    let b = a.clone();
    b.set(42);
    print("both cards see ${a.deref()}, strong ${a.count()}");
    let unique = Box::<i32>.new(7);
    print("unique box ${unique.deref()}");
    return 0;
}
```

---

## 4. `sync`: `Mutex`

`Mutex<T>` is a lock plus the value it protects. A critical section that mutates shared state must cover `lock()` through `unlock()`. Do not block for a long time while holding the lock. Call `destroy()` when finished.

This is not `Ref`. `Ref` answers “is the object still alive?”; `Mutex` answers “who may change it right now?”.

```vyx
use std.sync;

fn main() -> i32 {
    let counter = Mutex::<i32>.new(0);
    counter.lock();
    counter.set(counter.get() + 42);
    counter.unlock();
    print("inside the lock: ${counter.get()}");
    counter.destroy();
    return 0;
}
```

---

## 5. `vio`: tasks and the event loop

The full story is [lesson 25](ADVANCED_FEATURES.md#lesson-25-async-task-promise-and-vio). This table exists so the handbook does not send you to `vio_sleep`.

| Goal | Write |
|---|---|
| A function that can interleave | `@[async] fn` |
| Wait for a task’s value | `await …` |
| Yield time | `await Task::<i32>.sleep(ms)` or `yield_now()` |
| Already ok / already failed | `Task.ready` / `Promise.failed` |
| “Ready yet?” | `poll()` (**does not** pump the queue) |
| OS sleep | `vio_sleep` — freezes the scheduler; do not use it inside async to “wait a bit” |
| Old fiber API | `StartCoroutine` + `vio_start` / `vio_stop` |

```vyx
use std.vio;

@[async]
fn answer() -> i32 {
    await Task::<i32>.sleep(1);
    return 42;
}

fn main() -> i32 {
    let value = await answer();
    print("answer=${value}");
    return 0;
}
```

---

## 6. `reflect`: `Type`, fields, and bound methods

Lookup by name, write fields, bind methods: [lesson 43](ADVANCED_FEATURES.md#lesson-43-runtime-reflection). Contract: `tests/cases/tutorial_reflect.vyx`. Do not treat `reflect_*` builtins as the tutorial API.

```vyx
use std.reflect;

@[reflect("MeterType", alias=["MeterAlias"])]
public class Meter {
    @[reflect("value", alias=["reading"])]
    public value: i64;

    public fn bump(delta: i32) -> i32 {
        return (self.value as i32) + delta;
    }
}

type BoundBump = fn(i32) -> i32;

fn main() -> i32 {
    var src = Meter { value: 7 };
    let t = getType("MeterAlias");
    print("found ${t.name}");
    var inst = t.bind(&src);
    let prop = inst.getProperty("reading");
    inst.write::<i64>(prop, 11);
    let bound: BoundBump = inst.getMethod("bump").as::<BoundBump>();
    print("value=${src.value} bump(1)=${bound(1)}");
    return 0;
}
```

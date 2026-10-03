# Vyx 标准库教程

[English: Standard-library tutorial](STANDARD_LIBRARY_TUTORIAL.md) · [文档目录](README.md)

## 使用说明

本教程按模块介绍集合、字符串、资源管理、并发与反射。
完整 API 见 [标准库参考](STD_LIBRARY.zh-CN.md)，运行命令见 [入门教程](入门指南_ZH.md#编译与运行)。
反射与其他核心特性的关系见 [事实语义所有权系统](MOSP_ZH.md)。
创建多文件程序请按[项目指南](PROJECTS_ZH.md)操作；字段含义见
[清单参考](PACKAGE_MANIFEST_ZH.md)。

```text
collections   动态数组、字典、集合          → 第 11、22 课
string        字符串视图与所有权            → 第 15 课
ref           Box / Ref / Weak          → 第 21 课
sync          锁                        → 同步原语
vio           任务、睡眠、事件循环      → 第 25 课
reflect       类型与成员查询                → 第 43 课
```

---

## 1. collections：Vec、Dict、Set

集合基础见 [第 11 课](进阶教程_ZH.md#第11课集合容器)。这里是一份能一次跑完的复习。

checked API 是 `push` / `get` / `set` / `pop`。`*_unchecked` 把容量、边界、销毁状态、迭代器失效全部交给你。还在遍历时不要改结构。用完 `destroy()`。

```vyx
use std.collections;

fn main() -> i32 {
    var values = Vec::<i32>.new();
    values.push(10);
    values.push(32);
    print("两格：${values.count()} 第一格 ${values.get(0)}");
    values.set(1, 33);
    print("弹出 ${values.pop()}");
    values.destroy();

    var scores = Dict::<string, i32>.new();
    scores.put("alice", 42);
    print("alice=${scores.get("alice")} 在表里？${scores.contains("alice")}");
    scores.destroy();

    var seen = Set::<i32>.new();
    seen.add(7);
    seen.add(7);
    print("7 在集合里？${seen.contains(7)}");
    seen.destroy();
    return 0;
}
```

---

## 2. string：str、String 与迭代

`str` 只看；`String` 拥有。`for-in` 走 Unicode 标量；`.bytes()` 走 UTF-8 字节。游标还活着时不要改或销毁源。详见 [第 15 课](进阶教程_ZH.md#第15课字符串操作)。

```vyx
use std.string;

fn main() -> i32 {
    let view: str = "A¢€𐍈";
    var chars: i64 = 0;
    for (_c in view) { chars += 1; }
    var bytes: i64 = 0;
    for (_b in view.bytes()) { bytes += 1; }
    print("${chars} 个字符，${bytes} 个字节");

    var text = String.from("hello");
    var sum: i64 = 0;
    for (c in text) { sum += c as i64; }
    let borrowed: str = text.as_str();
    print("as_str 仍是 ${borrowed}，码点和 ${sum}");
    text.destroy();
    return 0;
}
```

`hello` 五个字符的码点和是 532。

---

## 3. Ref、Weak、Box

定义在 `std.ref`。直觉和图见 [第 21 课](高级特性_ZH.md#第21课资源管理refweak-与-box)。

```vyx
use std.ref;

fn main() -> i32 {
    let a = Ref::<i32>.new(10);
    let b = a.clone();
    b.set(42);
    print("两张卡都看见 ${a.deref()}，强引用 ${a.count()}");
    let unique = Box::<i32>.new(7);
    print("独占盒 ${unique.deref()}");
    return 0;
}
```

---

## 4. sync：Mutex

`Mutex<T>` 是一把锁加上它保护的值。改共享值时，临界区必须从 `lock()` 盖到 `unlock()`。持锁时不要做会长期阻塞的事。用完 `destroy()`。

它不是 `Ref`。`Ref` 管「对象还在不在」；`Mutex` 管「同一时刻谁能改」。

```vyx
use std.sync;

fn main() -> i32 {
    let counter = Mutex::<i32>.new(0);
    counter.lock();
    counter.set(counter.get() + 42);
    counter.unlock();
    print("锁里的数 ${counter.get()}");
    counter.destroy();
    return 0;
}
```

---

## 5. vio：任务与事件循环

完整故事在 [第 25 课](高级特性_ZH.md#第25课asynctaskpromise-与-vio)。这里只放一张对照表，避免手册把人带去 `vio_sleep`。

| 目的 | 写法 |
|---|---|
| 声明可交错的函数 | `@[async] fn` |
| 等任务结束拿到值 | `await …` |
| 让出时间 | `await Task::<i32>.sleep(ms)` 或 `yield_now()` |
| 已经成功 / 已经失败 | `Task.ready` / `Promise.failed` |
| 问一句好了没有 | `poll()`（**不**泵队列） |
| 操作系统睡觉 | `vio_sleep`——会把调度器一起卡住，async 里不要用来「等一会儿」 |
| 旧 fiber API | `StartCoroutine` + `vio_start` / `vio_stop` |

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

## 6. reflect：Type、字段与绑定方法

类型与成员查询、改字段、把方法绑成函数：见 [第 43 课](高级特性_ZH.md#第43课运行时反射)。契约 `tests/cases/tutorial_reflect.vyx`。不要用 `reflect_*` 内建当课内 API。

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
    print("找到 ${t.name}");
    var inst = t.bind(&src);
    let prop = inst.getProperty("reading");
    inst.write::<i64>(prop, 11);
    let bound: BoundBump = inst.getMethod("bump").as::<BoundBump>();
    print("value=${src.value} bump(1)=${bound(1)}");
    return 0;
}
```

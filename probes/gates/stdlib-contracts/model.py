"""Generate repeatable Vec operation traces with Python list value oracles."""
from __future__ import annotations

import json
from pathlib import Path
import random


def write_project(directory: Path, seed: int = 935867, steps: int = 128, block_size: int = 16,
                  kinds: tuple[str, ...] = ("i32", "i64", "u64", "f64", "string")) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    lines = ["module tests.std_model;", "use std.collections;", "use std.core;", "use std.string;"]
    functions = []
    for kind in kinds:
        rng = random.Random(seed)
        values = []

        def value():
            n = rng.randint(-10000, 10000)
            if kind == "string":
                return rng.choice(("", "a", "b", "ab", "a\0b", "a\0c", "中文", "quoted\"text", "line\nbreak"))
            if kind == "u64":
                return (n + (1 << 63)) & ((1 << 64) - 1)
            if kind == "f64":
                return n / 4
            return n

        def literal(v):
            if kind == "string":
                return json.dumps(v, ensure_ascii=False).replace("\\u0000", "\\0")
            return f"{v!r} as {kind}"

        functions.append("trace_" + kind)
        lines += [f"fn check_{kind}(values: &Vec<{kind}>, expected: *{kind}, count: i64, label: string) {{",
                  "    assert(values.len == count, label);", "    var index: i64 = 0;",
                  "    while (index < count) {", "        unsafe { assert((*values.get_ptr(index)) == expected[index], label); }",
                  "        index += 1;", "    }", "}"]
        lines += [f"fn trace_{kind}() {{", f"    var values = Vec::<{kind}>.new();"]
        # Force growth before the mixed trace; random clears otherwise keep
        # most traces below the initial capacity.
        for _ in range(32):
            v = value()
            values.append(v)
            lines.append(f"    values.push({literal(v)});")
        for part in range((steps + block_size - 1) // block_size):
            lines.append(f"    trace_{kind}_part_{part}(&mut values);")
        lines.append("}")
        for step in range(steps):
            if step % block_size == 0:
                if step:
                    lines.append("}")
                lines += [f"fn trace_{kind}_part_{step // block_size}(values: &mut Vec<{kind}>) {{"]
            op = rng.choice(("push", "push", "pop", "insert", "remove", "set", "reverse", "sort", "clear", "find", "clone"))
            label = f"{kind} seed={seed} step={step} {op}"
            if op == "push" or (not values and op in ("pop", "remove", "set")):
                v = value()
                lines.append(f"    values.push({literal(v)});")
                values.append(v)
            elif op == "pop":
                v = values.pop()
                lines.append(f'    assert(values.pop() == {literal(v)}, "{label}");')
            elif op == "insert":
                index = rng.randrange(len(values) + 1)
                v = value()
                values.insert(index, v)
                lines.append(f"    values.insert({index}, {literal(v)});")
            elif op == "remove":
                index = rng.randrange(len(values))
                v = values.pop(index)
                lines.append(f'    assert(values.removeAt({index}) == {literal(v)}, "{label}");')
            elif op == "set":
                index = rng.randrange(len(values))
                v = value()
                values[index] = v
                lines.append(f"    values.set({index}, {literal(v)});")
            elif op == "reverse":
                values.reverse()
                lines.append("    values.reverse();")
            elif op == "sort":
                values.sort()
                lines.append("    values.sort();")
            elif op == "clear":
                values.clear()
                lines.append("    values.clear();")
            elif op == "find":
                v = value()
                index = values.index(v) if v in values else -1
                boolean = "true" if v in values else "false"
                lines.append(f'    assert(values.indexOf({literal(v)}) == {index}, "{label} index");')
                lines.append(f'    assert(values.contains({literal(v)}) == {boolean}, "{label} contains");')
            elif op == "clone":
                lines.append(f'    {{ let copy = values.clone(); assert(copy == *values && copy.hash() == values.hash(), "{label}"); }}')
            expected = "[" + ", ".join(literal(v) for v in values) + "]" if values else f"null as *{kind}"
            lines.append(f'    check_{kind}(values, {expected}, {len(values)}, "{label}");')
        lines.append("}")
    lines += ["fn main() -> i32 {", *(f"    {name}();" for name in functions),
              '    print("std model contracts ok");', "    return 0;", "}"]
    (directory / "main.vyx").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (directory / "Vyx.toml").write_text('''[package]
name = "std_model_contracts"
version = "0.1.0"
entry = "main.vyx"
[build]
cache_dir = ".cache"
output_dir = "target"
[dependencies]
collections = { path = "std:collections", target = "std_collections" }
[target.std_model_contracts]
type = "executable"
entry = "main.vyx"
auto_sources = false
''', encoding="utf-8")
    return {"seed": seed, "steps_per_type": steps, "types": list(kinds),
            "operations": (steps + 32) * len(functions), "block_size": block_size,
            "oracle": "Python list operations and ordering"}

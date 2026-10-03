#!/usr/bin/env python3
"""Upgrade &self methods that consume/mutate self after the first receiver rewrite."""
from __future__ import annotations

import re
import sys
from pathlib import Path

CLASS_START = re.compile(r"^\s*(?:public\s+)?class\s+(\w+)\b")
FN_START = re.compile(
    r"^(\s*)((?:public|private|protected)\s+)?(?:static\s+)?fn\s+(\w+)\s*(?:<[^>]*>)?\s*\((.*)\)(.*)$"
)
PRIMITIVE_RET = re.compile(
    r"->\s*(?:bool|i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|string|rawptr|Result<)"
)

RECV_AND = re.compile(r"^&self\b")
RECV_MUT = re.compile(r"^mut self\b")
RECV_MUTREF = re.compile(r"^&mut self\b")
RECV_OWN = re.compile(r"^own self\b")


def brace_delta(s: str) -> int:
    in_str = False
    esc = False
    d = 0
    for ch in s:
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
        elif ch == "{":
            d += 1
        elif ch == "}":
            d -= 1
    return d


def receiver_of(args: str) -> str | None:
    a = args.strip()
    if not a:
        return None
    for p in ("&mut self", "own self", "mut self", "&self"):
        if a == p or a.startswith(p + ",") or a.startswith(p + ":"):
            return p
    if a == "self" or a.startswith("self,") or a.startswith("self:"):
        return "self"
    return None


def replace_receiver(args: str, new_recv: str) -> str:
    a = args.strip()
    old = receiver_of(a)
    if old is None:
        return args
    if a == old:
        return new_recv
    if a.startswith(old + ","):
        return new_recv + a[len(old) :]
    if a.startswith(old + ":"):
        return new_recv + a[len(old) :]
    return args


def iter_methods(lines: list[str]):
    class_name = None
    class_depth = 0
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        cm = CLASS_START.match(line)
        if class_name is None and cm:
            class_name = cm.group(1)
            class_depth = brace_delta(line)
            if class_depth <= 0:
                class_depth = 1
            i += 1
            continue
        if class_name is not None:
            fm = FN_START.match(line.rstrip("\n"))
            if fm and " static " not in line and not line.lstrip().startswith("static "):
                recv = receiver_of(fm.group(4))
                if recv is not None:
                    start = i
                    depth = brace_delta(line)
                    j = i
                    if depth <= 0:
                        j += 1
                        if j < n:
                            depth += brace_delta(lines[j])
                    while j + 1 < n and depth > 0:
                        j += 1
                        depth += brace_delta(lines[j])
                    yield {
                        "class": class_name,
                        "start": start,
                        "end": j,
                        "name": fm.group(3),
                        "recv": recv,
                        "args": fm.group(4),
                        "rest": fm.group(5),
                        "indent": fm.group(1),
                        "vis": fm.group(2) or "",
                    }
                    i = j + 1
                    continue
            class_depth += brace_delta(line)
            if class_depth <= 0:
                class_name = None
                class_depth = 0
        i += 1


def method_body(lines: list[str], m: dict) -> str:
    return "".join(lines[m["start"] : m["end"] + 1])


def choose_upgrade(rest: str) -> str:
    if PRIMITIVE_RET.search(rest) or "->" not in rest:
        return "&mut self"
    return "mut self"


SELF_ASSIGN = re.compile(r"\bself\s*=")
SELF_FIELD_ASSIGN = re.compile(r"\bself\.\w+\s*=(?!=)")
SELF_MUT_VEC = re.compile(
    r"\bself\.\w+\.(?:push|remove|set|destroy|insert|clear|removeAt|reset|pop)\s*\("
)
SELF_CALL = re.compile(r"\bself\.(\w+)\s*\(")


def process_file(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    methods = list(iter_methods(lines))
    mut_names: dict[str, set[str]] = {}
    for m in methods:
        if m["recv"] in ("mut self", "&mut self", "own self"):
            mut_names.setdefault(m["class"], set()).add(m["name"])

    changed: list[str] = []
    # apply from the end so indexes stay valid... we only edit the fn header line
    for m in methods:
        if m["recv"] != "&self":
            continue
        body = method_body(lines, m)
        header = lines[m["start"]]
        need = False
        if SELF_ASSIGN.search(body) or SELF_FIELD_ASSIGN.search(body) or SELF_MUT_VEC.search(body):
            need = True
        else:
            for call in SELF_CALL.findall(body):
                if call == m["name"]:
                    continue
                if call in mut_names.get(m["class"], set()):
                    need = True
                    break
        if not need:
            continue
        new_recv = choose_upgrade(m["rest"])
        new_args = replace_receiver(m["args"], new_recv)
        new_header = header.replace("(" + m["args"] + ")", "(" + new_args + ")", 1)
        if new_header == header:
            continue
        lines[m["start"]] = new_header
        changed.append(f"{path.as_posix()}:{m['start']+1} {m['class']}.{m['name']} &self -> {new_recv}")

    if changed:
        path.write_text("".join(lines), encoding="utf-8")
    return changed


def directly_mutates(body: str) -> bool:
    return bool(SELF_ASSIGN.search(body) or SELF_FIELD_ASSIGN.search(body) or SELF_MUT_VEC.search(body))


def revert_false_mutref(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    methods = list(iter_methods(lines))
    mutating: dict[str, set[str]] = {}
    for m in methods:
        body = method_body(lines, m)
        if m["recv"] in ("mut self", "own self") or directly_mutates(body):
            mutating.setdefault(m["class"], set()).add(m["name"])
    changed_any = True
    while changed_any:
        changed_any = False
        for m in methods:
            if m["name"] in mutating.get(m["class"], set()):
                continue
            body = method_body(lines, m)
            for call in SELF_CALL.findall(body):
                if call in mutating.get(m["class"], set()):
                    mutating.setdefault(m["class"], set()).add(m["name"])
                    changed_any = True
                    break

    changed: list[str] = []
    for m in methods:
        if m["recv"] != "&mut self":
            continue
        if m["name"] in mutating.get(m["class"], set()):
            continue
        new_args = replace_receiver(m["args"], "&self")
        header = lines[m["start"]]
        new_header = header.replace("(" + m["args"] + ")", "(" + new_args + ")", 1)
        if new_header == header:
            continue
        lines[m["start"]] = new_header
        changed.append(f"{path.as_posix()}:{m['start']+1} {m['class']}.{m['name']} &mut self -> &self")
    if changed:
        path.write_text("".join(lines), encoding="utf-8")
    return changed


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "Zyn/src")
    mode = sys.argv[2] if len(sys.argv) > 2 else "upgrade"
    files = sorted(root.rglob("*.vyx"))
    all_changed: list[str] = []
    for f in files:
        if mode == "revert":
            all_changed.extend(revert_false_mutref(f))
        else:
            all_changed.extend(process_file(f))
    for line in all_changed:
        print(line)
    print(f"changed {len(all_changed)} methods")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

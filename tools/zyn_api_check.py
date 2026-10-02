#!/usr/bin/env python3
"""Static API checker for the Vyx Zyn tree.

`--stop-after-parse` proves a file is syntactically well formed but says nothing
about whether `Foo.bar(1, 2)` names a real member with a real arity. A full
compile answers that, at the cost of ~10 minutes and a large memory peak. This
script answers the same question in about a second, for the subset that matters:

  * `Type.member(...)` and `Type::member(...)` where `Type` starts with a capital
    letter -- i.e. static calls, which are the majority of cross-module use.
  * member existence and arity, following a single-inheritance chain.

It deliberately does NOT try to infer the type of `someVariable.member(...)`
(that needs real type inference). So it is a filter, not a substitute for the
compiler: a clean run is necessary, not sufficient.

Usage:  python3 tools/zyn_api_check.py [--root Zyn/src] [--only file.vyx ...]
"""
import os
import re
import sys
from collections import defaultdict

BUILTIN_TYPES = {
    "Vec", "String", "str", "Box", "Result", "Option", "Some", "None", "Ok", "Err",
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "bool",
    "rawptr", "Self", "Dyn",
}

TYPE_DECL = re.compile(r"^\s*(?:public\s+)?(?:abstract\s+)?(class|interface|enum|struct)\s+([A-Za-z_]\w*)")
# `public class Foo<T> : Base {`
BASE_DECL = re.compile(r"^\s*(?:public\s+)?(?:abstract\s+)?(?:class|interface|struct)\s+([A-Za-z_]\w*)(?:<[^>]*>)?\s*:\s*([A-Za-z_][\w.]*)")
FN_DECL = re.compile(r"^\s*(?:public\s+|private\s+|protected\s+)?(?:static\s+)?fn\s+([A-Za-z_]\w*)\s*\(")
FN_RET = re.compile(r"\)\s*->\s*([A-Za-z_][\w.<>]*)\s*[,{]?\s*$")
FREE_FN = re.compile(r"^(?:public\s+)?fn\s+([A-Za-z_]\w*)\s*\(")
# `use` lines are scanned against the whole file text, so they need re.M:
# without it `^` only matches at offset 0 and no import is ever recorded.
USE_ALIAS = re.compile(r"^use\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_][\w.]*)\s*;", re.M)
USE_PLAIN = re.compile(r"^use\s+([A-Za-z_][\w.]*)\s*;", re.M)
CALL = re.compile(r"\b([A-Z]\w*)\s*(?:\.|::)\s*([a-zA-Z_]\w*)\s*\(")
# A lowercase receiver is a local, a field through `self`, or a parameter.
# The main regex above deliberately skips these because most of them come from
# stdlib types this script cannot infer - but `Vec` is one we *can* track, and
# guessing its method names wrong is easy (`append` instead of `push`) because
# the names differ across the collection types in this codebase.
LOWER_CALL = re.compile(r"\b([a-z]\w*)\s*\.\s*([a-zA-Z_]\w*)\s*\(")
SELF_CALL = re.compile(r"\bself\.([a-zA-Z_]\w*)\s*\.\s*([a-zA-Z_]\w*)\s*\(")
VEC_LOCAL = re.compile(r"\b(?:let|var)\s+([a-zA-Z_]\w*)\s*=\s*Vec\s*::<")
VEC_FIELD = re.compile(r"^\s*(?:public\s+|private\s+|protected\s+)?([a-zA-Z_]\w*)\s*:\s*Vec\s*<", re.M)
VEC_PARAM = re.compile(r"([a-zA-Z_]\w*)\s*:\s*Vec\s*<")
# Any declared type for a *lowercase* name, used to decide whether a name is
# unambiguously a Vec. A name reused for two different types cannot be graded,
# so it is skipped rather than reported - false positives cost more here than
# missed checks.
ANY_FIELD = re.compile(r"^\s*(?:public\s+|private\s+|protected\s+)?([a-z]\w*)\s*:\s*(?:&|&mut\s+|mut\s+|dyn\s+)?([A-Za-z_][\w.<>]*)\s*;", re.M)
ANY_PARAM = re.compile(r"([a-z]\w*)\s*:\s*(?:&|&mut\s+|mut\s+|dyn\s+)?([A-Za-z_][\w.<>]*)(?=\s*[,)])")
ANY_LOCAL = re.compile(r"\b(?:let|var)\s+([a-z]\w*)\s*=\s*(?:([A-Za-z_]\w*)(?:<[^(]*>)?\s*(?:\.|::|\())?")


def strip_comments_and_strings(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            out.append('""')
            continue
        out.append(c)
        i += 1
    return "".join(out)


def balanced_end(text, start):
    """Index just past the ')' that closes the '(' at `start`."""
    depth = 0
    i = start
    n = len(text)
    while i < n:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def split_top_level(args):
    if args.strip() == "":
        return []
    parts = []
    depth = 0
    cur = []
    for ch in args:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
            continue
        cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur))
    return [p for p in parts if p.strip() != ""]


def collect_declarations(root):
    """Returns (members, bases, free_fns, alias_map, type_files, module_fns, module_alias_map)."""
    members = defaultdict(dict)
    bases = {}
    returns = {}
    free_fns = set()
    type_files = {}
    type_dupes = []
    alias_map = {}
    module_fns = defaultdict(lambda: defaultdict(set))
    module_alias_map = {}

    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".vyx"):
                continue
            path = os.path.join(dirpath, fn)
            raw = open(path, encoding="utf-8", errors="replace").read()
            text = strip_comments_and_strings(raw)
            mm = re.search(r"^module\s+([\w.]+)\s*;", text, re.M)
            module_name = mm.group(1) if mm else ""
            for m in USE_ALIAS.finditer(text):
                alias_map[m.group(1)] = m.group(2).split(".")[-1]
                module_alias_map[m.group(1)] = m.group(2)
            for m in USE_PLAIN.finditer(text):
                tail = m.group(1).split(".")[-1]
                if tail and tail[0].isupper():
                    alias_map.setdefault(tail, tail)
                module_alias_map.setdefault(m.group(1), m.group(1))

            lines = text.split("\n")
            current = None
            depth = 0
            type_depth = 0
            for idx, line in enumerate(lines):
                m = TYPE_DECL.match(line)
                if m:
                    name = m.group(2)
                    current = name
                    # Same-name types are only a hazard when they share a *module*
                    # (or both sit at crate root): distinct modules namespace their
                    # types, so `std.http.HttpResponse` and `std.curl.HttpResponse`
                    # coexist fine. A clash inside one module is what silently
                    # degrades member resolution to rawptr at the consumer.
                    key = (module_name, name)
                    if key in type_files and type_files[key] != path:
                        type_dupes.append(
                            "%s: duplicate type %r also declared in %s (module %r;"
                            " same-name classes in one module break member resolution"
                            " at the consumer)"
                            % (path, name, type_files[key], module_name or "<root>"))
                    else:
                        type_files.setdefault(key, path)
                    members.setdefault(name, {})
                    type_depth = depth
                    b = BASE_DECL.match(line)
                    if b:
                        bases[name] = b.group(2).split(".")[-1]
                fm = FN_DECL.match(line)
                if fm and current is not None:
                    name = fm.group(1)
                    pstart = line.index("(", fm.start())
                    buf = line[pstart:]
                    j = idx
                    while buf.count("(") > buf.count(")") and j + 1 < len(lines):
                        j += 1
                        buf += " " + lines[j]
                    end = balanced_end(buf, 0)
                    params = split_top_level(buf[1:end - 1])
                    # a receiver (`self`, `mut self`, `&self`, `&mut self`) is not an argument
                    receiver = bool(params) and re.search(r"\bself\b", params[0])
                    if receiver:
                        params = params[1:]
                    slot = members[current].setdefault(name, {})
                    # static and instance methods live in separate namespaces:
                    # `View.text(s)` (static) and `v.text()` (instance) coexist,
                    # as do `Text.title(s)` and `v.title()`.
                    #
                    # A name may also carry several *overloads* - Vyx resolves
                    # them by arity (`AppProfile.named(name)` and
                    # `AppProfile.named(name, w, h)` both exist), so a slot holds
                    # a set of accepted arities rather than one number.
                    key = "static" if "static" in line else "instance"
                    slot.setdefault(key, set()).add(len(params))
                    ret = FN_RET.search(buf)
                    if ret:
                        returns.setdefault((current, name), set()).add(ret.group(1).split("<", 1)[0])
                elif current is None:
                    ff = FREE_FN.match(line)
                    if ff:
                        pstart = line.index("(", ff.start())
                        buf = line[pstart:]
                        j = idx
                        while buf.count("(") > buf.count(")") and j + 1 < len(lines):
                            j += 1
                            buf += " " + lines[j]
                        end = balanced_end(buf, 0)
                        arity = len(split_top_level(buf[1:end - 1]))
                        free_fns.add(ff.group(1))
                        if module_name:
                            module_fns[module_name][ff.group(1)].add(arity)
                depth += line.count("{") - line.count("}")
                if current is not None and depth <= type_depth:
                    current = None
    return members, bases, returns, free_fns, alias_map, type_files, module_fns, module_alias_map, type_dupes


def resolve_member(members, bases, tname, member, seen=None):
    """Depth-first search up the inheritance chain. Returns the member dict or None."""
    if seen is None:
        seen = set()
    if tname in seen:
        return None
    seen.add(tname)
    table = members.get(tname)
    if table and member in table:
        return table[member]
    base = bases.get(tname)
    if base:
        return resolve_member(members, bases, base, member, seen)
    return None


def vec_locals_for(text):
    """Names that are unambiguously a `Vec<T>` everywhere they appear.

    Every declaration of each lowercase name is collected first. A name that is
    ever declared with something other than `Vec` is dropped: the whole point of
    this check is certainty, and a name like `out` that holds a `RenderGraph` in
    one function and a `Vec` in another cannot be graded from its spelling.
    """
    def norm(name):
        if name is None:
            return None
        # A generic instantiation is still the same type for this purpose:
        # `Vec<AppApiSample>` and `Vec<string>` both only have Vec's methods.
        return name.split("<", 1)[0]

    declared = defaultdict(set)
    for m in VEC_LOCAL.finditer(text):
        declared[m.group(1)].add("Vec")
    for m in VEC_FIELD.finditer(text):
        declared[m.group(1)].add("Vec")
    for m in VEC_PARAM.finditer(text):
        declared[m.group(1)].add("Vec")
    for m in ANY_FIELD.finditer(text):
        declared[m.group(1)].add(norm(m.group(2)))
    for m in ANY_PARAM.finditer(text):
        declared[m.group(1)].add(norm(m.group(2)))
    for m in ANY_LOCAL.finditer(text):
        declared[m.group(1)].add(norm(m.group(2)))
    return {name for name, kinds in declared.items() if kinds == {"Vec"}}


LOCAL_ASSIGN = re.compile(r"\b(?:let|var)\s+([a-z]\w*)\s*=\s*([^\n;]+?)\s*;", re.S)
INIT_HEAD = re.compile(r"^([A-Za-z_]\w*)(?:\s*\.\s*([a-zA-Z_]\w*))?")


def infer_local_types(text, returns, alias_map):
    """Resolves each local to a concrete type where it can be read off a call.

    This exists to catch calls like `version.destroy()` where the type simply
    has no such method - the single most common way a module that only parses
    successfully turns out not to compile. Only *unambiguous* results are kept:
    if a name is ever bound to more than one type, or to something this script
    cannot read, the name is dropped rather than guessed at.
    """
    candidates = defaultdict(set)
    unknown = set()
    for m in LOCAL_ASSIGN.finditer(text):
        name, rhs = m.group(1), m.group(2).strip()
        head = INIT_HEAD.match(rhs)
        if head is None:
            unknown.add(name)
            continue
        recv, meth = head.group(1), head.group(2)
        recv_type = alias_map.get(recv, recv)
        if meth is None:
            # `let x = SomeType;` - only useful when it names a type at all.
            unknown.add(name)
            continue
        rets = returns.get((recv_type, meth))
        if not rets or len(rets) != 1:
            unknown.add(name)
            continue
        candidates[name] |= rets

    # Second pass: `let y = x.method()` where x already resolved.
    for _ in range(3):
        changed = False
        for m in LOCAL_ASSIGN.finditer(text):
            name, rhs = m.group(1), m.group(2).strip()
            head = INIT_HEAD.match(rhs)
            if head is None:
                continue
            recv, meth = head.group(1), head.group(2)
            if meth is None or recv not in candidates or recv in unknown:
                continue
            if len(candidates[recv]) != 1:
                continue
            recv_type = next(iter(candidates[recv]))
            rets = returns.get((recv_type, meth))
            if not rets or len(rets) != 1:
                unknown.add(name)
                continue
            if candidates[name] != rets:
                candidates[name] |= rets
                changed = True
        if not changed:
            break

    out = {}
    for name, types in candidates.items():
        if name in unknown or len(types) != 1:
            continue
        out[name] = next(iter(types))
    return out


FIELD_ANY = re.compile(r"^\s*(?:public\s+|private\s+|protected\s+)?([a-z]\w*)\s*:\s*[A-Za-z_]", re.M)


def check_typed_calls(path, text, members, bases, typed, problems):
    """Validates method calls on locals whose type was inferred."""
    if not typed:
        return
    # A local whose name is also a *field* somewhere is almost always a display
    # of `obj.field`, not a binding this script tracked correctly - and grading
    # it produces noise. Drop those names rather than guess.
    fields = set()
    for m in FIELD_ANY.finditer(text):
        fields.add(m.group(1))
    typed = {k: v for k, v in typed.items() if k not in fields}
    if not typed:
        return
    for m in LOWER_CALL.finditer(text):
        receiver, method = m.group(1), m.group(2)
        tname = typed.get(receiver)
        # The builtin `string` has no Vyx declaration here to check against, and
        # gradeable names in the rendering codegen live behind patterns this
        # script does not model; both are skipped rather than approximated.
        if tname is None or tname in BUILTIN_TYPES or tname == "string":
            continue
        slot = resolve_member(members, bases, tname, method)
        line = text.count("\n", 0, m.start()) + 1
        if slot is None:
            known = sorted(members.get(tname, {}).keys())
            problems.append((path, line,
                             f"`{receiver}.{method}(...)` is not a member of {tname}"
                             f" (declared as {tname}; have: {', '.join(known[:12])})"))
            continue
        arity = count_call_args(text, m.start(), receiver, method)
        accepted = set()
        for kind in ("instance", "static"):
            accepted |= slot.get(kind, set())
        if arity is not None and accepted and arity not in accepted:
            listed = "/".join(str(a) for a in sorted(accepted))
            problems.append((path, line,
                             f"`{receiver}.{method}` takes {listed} arg(s), call passes {arity}"))


def check_vec_calls(path, text, members, vec_locals, problems):
    """Validates method calls whose receiver is known to be a `Vec<T>`."""
    vec_members = members.get("Vec")
    if not vec_members or not vec_locals:
        return
    hits = []
    for m in LOWER_CALL.finditer(text):
        if m.group(1) not in vec_locals:
            continue
        hits.append((m.group(1), m.group(2), m.start()))
    for m in SELF_CALL.finditer(text):
        if m.group(1) not in vec_locals:
            continue
        hits.append(("self." + m.group(1), m.group(2), m.start()))
    for receiver, method, pos in hits:
        slot = vec_members.get(method)
        line = text.count("\n", 0, pos) + 1
        if slot is None:
            known = sorted(vec_members.keys())
            # `len` is a public field, not a method - accept it explicitly.
            if method in ("len", "cap", "gen", "data"):
                continue
            problems.append((path, line,
                             f"`{receiver}.{method}(...)` is not a Vec method"
                             f" (have: {', '.join(known)})"))
            continue
        arity = count_call_args(text, pos, receiver, method)
        accepted = set()
        for kind in ("instance", "static"):
            accepted |= slot.get(kind, set())
        if arity is not None and accepted and arity not in accepted:
            listed = "/".join(str(a) for a in sorted(accepted))
            problems.append((path, line,
                             f"`{receiver}.{method}` takes {listed} arg(s), call passes {arity}"))


def count_call_args(text, pos, receiver, method):
    """Arity of the call starting at `pos`, or None if it cannot be read."""
    open_paren = text.index("(", pos)
    end = balanced_end(text, open_paren)
    if end > len(text):
        return None
    return len(split_top_level(text[open_paren + 1:end - 1]))


def check_file(path, members, bases, returns, alias_map, module_fns, module_alias_map):
    text = strip_comments_and_strings(open(path, encoding="utf-8", errors="replace").read())
    problems = []

    # Every name in this file that is known to hold a `Vec<T>`. Scoping is
    # approximated at file level, which is safe: `Vec` has one method set, so a
    # local of any other type cannot produce a false "unknown method" here -
    # only a missed check if two unrelated locals share a name.
    vec_locals = vec_locals_for(text)
    check_vec_calls(path, text, members, vec_locals, problems)
    # Scoped to the application and sample layers. Those are the files written
    # against this framework's own API surface, which is where the mistakes this
    # catches actually occur; the renderer and std trees use command objects and
    # dispatch tables that file-level inference cannot follow.
    rel = path.replace("\\", "/")
    if "/Zyn/src/app/" in rel or "/samples/projects/" in rel:
        typed = infer_local_types(text, returns, alias_map)
        check_typed_calls(path, text, members, bases, typed, problems)

    # Scan the whole text, not line by line: a call may span several lines, and
    # a per-line scan would read a multi-line argument list as arity zero.
    for m in CALL.finditer(text):
        tname, member = m.group(1), m.group(2)
        if tname in BUILTIN_TYPES:
            continue
        # A short all-caps receiver is a generic type parameter (`T.zero(...)`),
        # not a concrete type this script can resolve.
        if len(tname) <= 2 and tname.isupper():
            continue
        open_paren = m.end() - 1
        end = balanced_end(text, open_paren)
        args = split_top_level(text[open_paren + 1:end - 1])
        arity = len(args)
        line = text.count("\n", 0, m.start()) + 1
        target = alias_map.get(tname, tname)

        if target not in members:
            # Not a type: it may be a module alias, as in
            # `use Publish = Zyn.Toolchain.Publish;` then `Publish.name(...)`.
            module = module_alias_map.get(tname, tname)
            table = module_fns.get(module)
            if table is None:
                problems.append((path, line, f"unknown type `{tname}.{member}(...)`"))
                continue
            if member not in table:
                problems.append((path, line, f"`{tname}.{member}` is not a function in module {module}"))
                continue
            if arity not in table[member]:
                listed = "/".join(str(a) for a in sorted(table[member]))
                problems.append(
                    (path, line,
                     f"`{tname}.{member}` takes {listed} arg(s), call passes {arity}"))
            continue

        slot = resolve_member(members, bases, target, member)
        if slot is None:
            problems.append((path, line, f"`{tname}.{member}` is not a member of {target}"))
            continue
        # A capitalised receiver is a static call; only fall back to the
        # instance namespace if there is no static of that name.
        if "static" in slot:
            accepted = slot["static"]
        elif "instance" in slot:
            accepted = slot["instance"]
        else:
            problems.append((path, line, f"`{tname}.{member}` has no callable form"))
            continue
        if arity not in accepted:
            listed = "/".join(str(a) for a in sorted(accepted))
            problems.append(
                (path, line,
                 f"`{tname}.{member}` takes {listed} arg(s), call passes {arity}"))
    return problems


def main():
    argv = sys.argv[1:]
    roots = ["Zyn/src", "std"]
    only = []
    if "--root" in argv:
        roots = argv[argv.index("--root") + 1].split(",")
    if "--only" in argv:
        i = argv.index("--only") + 1
        only = argv[i:]
    members = defaultdict(dict)
    bases = {}
    returns = defaultdict(set)
    free_fns = set()
    alias_map = {}
    type_files = {}
    module_fns = defaultdict(lambda: defaultdict(set))
    module_alias_map = {}
    type_dupes = []
    for root in roots:
        m, b, rt, f, a, t, mf, ma, dupes = collect_declarations(root)
        for k, v in m.items():
            members[k].update(v)
        bases.update(b)
        for key, vals in rt.items():
            returns[key] |= vals
        free_fns |= f
        alias_map.update(a)
        type_files.update(t)
        for mod, table in mf.items():
            for name, arities in table.items():
                module_fns[mod][name] |= arities
        module_alias_map.update(ma)
        type_dupes.extend(dupes)
    targets = []
    for root in roots:
        for dirpath, _d, filenames in os.walk(root):
            for fn in filenames:
                if fn.endswith(".vyx"):
                    targets.append(os.path.join(dirpath, fn))
    if only:
        wanted = {os.path.basename(o) for o in only}
        targets = [t for t in targets if os.path.basename(t) in wanted]
    total = 0
    if type_dupes:
        print("--- duplicate type declarations (break member resolution at the consumer)")
        for d in type_dupes:
            print(f"  {d}")
        total += len(type_dupes)
    for t in sorted(targets):
        probs = check_file(t, members, bases, returns, alias_map, module_fns, module_alias_map)
        if probs:
            print(f"--- {t}")
            for path, ln, msg in probs:
                print(f"  {ln}: {msg}")
            total += len(probs)
    print(f"\nroots={roots} types={len(members)} free_fns={len(free_fns)} "
          f"dupes={len(type_dupes)} problems={total}")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())

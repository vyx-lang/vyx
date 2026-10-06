"""Contract-to-Vyx Converter. This module never invokes a producer compiler.

Declarations retain their measured entity identities. The Consumer validates
edited definitions against the original contract at compilation time.
"""
from __future__ import annotations

from collections import defaultdict
import json
import os
from pathlib import Path
import re

class ConversionError(ValueError):
    pass


VYX_RESERVED = set("fn if as i8 u8 is in let var use for mut new asm i16 i32 i64 u16 u32 u64 f32 f64 true enum impl elif else case null self task fail type when bool char void false class error while match break yield async await const defer bench trait where macro isize usize struct import module export extern return public static unsafe rawptr default private newtype concept foreach continue internal override comptime protocol requires volatile interface protected static_assert".split())



def type_shape(ty: dict) -> str:
    if ty is None:
        return "void"  # The symbol schema uses null for a void return.
    if "type" in ty and "name" not in ty:
        return type_shape(ty["type"])
    reference = ty.get("reference", "value")
    if ty.get("kind") == "function":
        if reference != "pointer" or ty.get("calling_convention") != "cdecl" or ty.get("noexcept"):
            raise ConversionError("function pointer ABI has no matching Vyx cfn syntax")
        signature = ty["signature"]
        if signature.get("variadic"):
            raise ConversionError("variadic callbacks are unsupported")
        return "cfn(" + ",".join(type_shape(p) for p in signature["params"]) + ")->" + type_shape(signature["return"])
    name = ty["name"].replace("::", ".")
    if ty.get("kind") == "opaque" or ty.get("unsupported_reason"):
        raise ConversionError(f"unsupported DCI type: {name}")
    if reference in ("pointer", "pointer_const", "pointer_mut", "reference"):
        inner = type_shape(ty["pointee"]) if "pointee" in ty else name
        if reference == "reference":
            if ty.get("reference_kind", "lvalue") != "lvalue":
                raise ConversionError("rvalue references are unsupported")
            return ("&" if ty.get("pointee_const", False) else "&mut ") + inner
        return "rawptr" if inner == "void" else "*" + inner
    return name



def available_type(ty, owners):
    try:
        shape = type_shape(ty)
    except ConversionError:
        return False
    inner = ty.get("type", ty) if ty else {"kind": "primitive"}
    if "pointee" in inner:
        return available_type(inner["pointee"], owners)
    if inner.get("kind") == "function":
        signature = inner["signature"]
        return all(available_type(p, owners) for p in signature["params"]) and available_type(signature["return"], owners)
    if inner.get("kind") == "class" and inner.get("name") not in owners:
        return False
    return bool(re.fullmatch(r"[A-Za-z_][\w.*&, <>():-]*", shape.lstrip("*& "))) and "..." not in shape



def path_literal(value):
    if not isinstance(value, str) or any(c in value for c in '\"\n\r\0'):
        raise ConversionError(f"invalid import path: {value!r}")
    return value.replace("\\", "/")


def default_literal(param):
    """Translate measured literal defaults, with type/range checks."""
    fact = param.get("default")
    if fact is None:
        return None
    if fact.get("kind") != "constant":
        raise ConversionError(fact.get("reason", "default argument requires producer evaluation"))
    ty, value = param["type"], fact.get("value")
    if ty.get("reference") in {"pointer", "pointer_const", "pointer_mut"} and value is None:
        return "null"
    if ty.get("kind") == "primitive" and ty.get("reference", "value") == "value":
        name = ty.get("name", "")
        if name == "bool" and type(value) is bool:
            return "true" if value else "false"
        match = re.fullmatch(r"([iu])(8|16|32|64)", name)
        if match and type(value) is int:
            bits, unsigned = int(match[2]), match[1] == "u"
            low, high = (0, (1 << bits) - 1) if unsigned else (-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
            if low <= value <= high:
                return str(value)
    raise ConversionError("default constant does not match its measured parameter type/range")


def write_definition(path, source, force=False):
    """An explicit conversion must not overwrite an authored definition."""
    path = Path(path)
    data = source.encode("utf-8")
    if path.exists():
        if path.read_bytes() == data:
            return
        if not force:
            raise ConversionError(f"definition already exists: {path}; choose another output or explicitly use --force")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb" if force else "xb") as stream:
        stream.write(data)


def emit_import(document, module, contract, headers=(), *, editable=True):
    if not re.fullmatch(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*", module) or any(part in VYX_RESERVED for part in module.split(".")):
        raise ConversionError(f"invalid Vyx module: {module!r}")
    contract = path_literal(contract)
    headers = [path_literal(header) for header in headers]
    layouts = {s["type_name"]: s for s in document["exports"]["layouts"]
               if re.fullmatch(r"[A-Za-z_]\w*", s["type_name"])}
    rejected = [{"selector": s["type_name"], "reason": "consumer import generator requires a global C++ class name"}
                for s in document["exports"]["layouts"] if s["type_name"] not in layouts]
    # A class must not advertise an inheritance edge to an omitted declaration.
    # Close this before admitting signatures containing those class identities.
    while True:
        missing = [name for name, layout in layouts.items()
                   if any(b["visibility"] == "public" and b["type_name"] not in layouts
                          for b in layout.get("bases", []))]
        if not missing:
            break
        for name in missing:
            rejected.append({"selector": name, "reason": "class requires unavailable consumer base declarations"})
            del layouts[name]
    symbols = document["exports"]["symbols"]
    groups = defaultdict(list)
    for s in symbols:
        owner = s.get("owner", "")
        if owner and owner not in layouts:
            rejected.append({"selector": s["name"], "reason": "method owner has no representable consumer declaration"})
            continue
        if s["visibility"] != "public" and not (s["visibility"] == "protected" and s.get("is_virtual")):
            continue
        name = s.get("member_name") or s["name"]
        if s["kind"] not in {"constructor", "destructor"} and not re.fullmatch(r"[A-Za-z_]\w*", name):
            rejected.append({"selector": s["name"], "reason": "native name has no supported consumer declaration syntax"})
            continue
        if name in VYX_RESERVED:
            rejected.append({"selector": s["name"], "reason": "native member name is reserved by the consumer syntax"})
            continue
        if not all(available_type(p["type"], layouts) for p in s["params"]) or not available_type(s["return"], layouts):
            rejected.append({"selector": s["name"], "reason": "type is outside the representable consumer import surface"})
            continue
        shape = tuple(type_shape(p["type"]) for p in s["params"])
        groups[(owner, s["kind"], name, shape)].append(s)
    by_owner = defaultdict(list)
    for identity, overloads in groups.items():
        if len(overloads) > 1:
            rejected.append({"selector": repr(identity), "reason": "distinct native overloads have the same consumer parameter identity"})
            continue
        by_owner[identity[0]].append(overloads[0])
    comment = ("// Converted from a measured DCI contract; editable, checked by the Consumer."
               if editable else "// Generated DCI cache definition; regenerate through the Adapter/Converter pipeline.")
    out = [comment, f"module {module};", "", f'@[dci_import("{contract}")]']
    out += [f'@[cpp_include("{header}")]' for header in headers]
    out.append('extern "dci" {')
    def declaration(s):
        used, names = set(), []
        defaults = []
        # Vyx requires optional parameters to form a trailing suffix. Retain
        # the full native signature when an earlier default is unsupported.
        for p in s["params"]:
            try:
                defaults.append(default_literal(p))
            except ConversionError as error:
                defaults.append(None)
                rejected.append({"selector": s["name"], "parameter": p["name"],
                                 "reason": str(error), "scope": "default_argument"})
        first_optional = len(defaults)
        while first_optional and defaults[first_optional - 1] is not None:
            first_optional -= 1
        for i, p in enumerate(s["params"]):
            name = p.get("name", "")
            if not re.fullmatch(r"[A-Za-z_]\w*", name) or name in VYX_RESERVED or name in used:
                name = f"p{i}"
                while name in used:
                    name = "_" + name
            used.add(name)
            names.append(f'{name}: {type_shape(p["type"])}' +
                         (f" = {defaults[i]}" if i >= first_optional else ""))
        params = ", ".join(names)
        if s["kind"] == "constructor":
            return f'{s["owner"]}({params});'
        if s["kind"] == "destructor":
            return f'~{s["owner"]}();'
        result = type_shape(s["return"])
        return ("public " if s.get("owner") else "") + ("static " if s.get("is_static") else "") + ("virtual " if s.get("is_virtual") else "") + f'fn {s.get("member_name") or s["name"]}({params})' + (" const" if s.get("is_const") else "") + (f" -> {result}" if result != "void" else "") + ";"
    # Stable topological order also detects malformed or cyclic hierarchy facts.
    pending, done = dict(layouts), set()
    while pending:
        ready = [name for name, l in pending.items() if all(b["type_name"] in done for b in l.get("bases", []) if b["type_name"] in layouts)]
        if not ready:
            raise ConversionError("cyclic measured C++ class hierarchy")
        for name in sorted(ready):
            layout = pending.pop(name)
            bases = [b for b in layout.get("bases", []) if b["visibility"] == "public"]
            base = " : " + ", ".join(b["type_name"] for b in bases) if bases else ""
            out.append(f"    class {name}{base} {{")
            for s in sorted(by_owner[name], key=lambda s: (s["member_name"], json.dumps(s["params"], sort_keys=True))):
                out.append("        " + declaration(s))
            out.append("    };")
            done.add(name)
    for s in sorted(by_owner[""], key=lambda s: (s["name"], s["link_name"])):
        out.append("    " + declaration(s))
    out += ["}", ""]
    return "\n".join(out), rejected

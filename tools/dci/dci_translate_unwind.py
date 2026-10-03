"""Generic translate_unwind stub emission for C++ Adapter.

Original may_unwind symbols stay Direct-rejected.  A side translator is a
no_unwind extern "C" function compiled by the producer toolchain; it catches
any current exception into dci.Failure.  Names are derived from the original
link identity -- nothing is hardcoded to a fixture symbol.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import tempfile
from typing import Any

RUNTIME_DIR = Path(__file__).resolve().parent / "runtime"
FAILURE_RECORD = "dci::Failure"
FAILURE_DESTROY = "dci_failure_destroy"
FAILURE_COPY = "dci_failure_copy"
FAILURE_CLEAR = "dci_failure_clear"
FAILURE_SIZEOF = "dci_failure_sizeof"


def sanitize_link_token(text: str) -> str:
    token = re.sub(r"[^A-Za-z0-9_]", "_", text)
    token = re.sub(r"_+", "_", token).strip("_")
    if not token:
        token = "sym"
    if token[0].isdigit():
        token = "x_" + token
    return token[:180]


def translator_link_name(link_name: str, used: set[str]) -> str:
    base = "dci_tr_" + sanitize_link_token(link_name)
    candidate = base
    serial = 2
    while candidate in used:
        candidate = f"{base}_{serial}"
        serial += 1
    used.add(candidate)
    return candidate


def translated_record_name(link_name: str) -> str:
    return "dci::Translated_" + sanitize_link_token(link_name)


def cpp_name(text: str) -> str:
    return (text or "").replace(".", "::")


def class_leaf_name(owner: str) -> str:
    leaf = cpp_name(owner).split("::")[-1]
    return leaf.split("<", 1)[0]


def msvc_free_function_cpp_name(mangled: str, fallback: str) -> str:
    if not mangled.startswith("?") or mangled.startswith("??"):
        return cpp_name(fallback)
    body = mangled[1:]
    marker = body.find("@@")
    if marker < 0:
        return cpp_name(fallback)
    parts = [part for part in body[:marker].split("@") if part]
    if not parts:
        return cpp_name(fallback)
    name = parts[0]
    namespaces = list(reversed(parts[1:]))
    return "::".join(namespaces + [name])


def _itanium_length_ident(mangled: str, index: int) -> tuple[str, int]:
    if index >= len(mangled) or not mangled[index].isdigit():
        return "", index
    length = 0
    while index < len(mangled) and mangled[index].isdigit():
        length = length * 10 + int(mangled[index])
        index += 1
    ident = mangled[index : index + length]
    if len(ident) != length:
        return "", index
    return ident, index + length


def itanium_free_function_cpp_name(mangled: str, fallback: str) -> str:
    if not mangled.startswith("_Z"):
        return cpp_name(fallback)
    index = 2
    parts: list[str] = []
    if index < len(mangled) and mangled[index] == "N":
        index += 1
        while index < len(mangled) and mangled[index] in "KVR":
            index += 1
        while index < len(mangled) and mangled[index] != "E":
            if mangled.startswith("St", index):
                parts.append("std")
                index += 2
                continue
            ident, index = _itanium_length_ident(mangled, index)
            if not ident:
                return cpp_name(fallback)
            parts.append(ident)
        if not parts:
            return cpp_name(fallback)
        return "::".join(parts)
    ident, _ = _itanium_length_ident(mangled, index)
    if ident:
        return ident
    return cpp_name(fallback)


def free_function_cpp_name(mangled: str, fallback: str) -> str:
    if mangled.startswith("?"):
        return msvc_free_function_cpp_name(mangled, fallback)
    if mangled.startswith("_Z"):
        return itanium_free_function_cpp_name(mangled, fallback)
    return cpp_name(fallback)


def cpp_spelling_of_type(type_info: dict[str, Any] | None) -> str:
    if not isinstance(type_info, dict):
        return "void"
    raw = (type_info.get("cpp_type") or "").strip()
    reference = type_info.get("reference") or "value"
    name = cpp_name(str(type_info.get("name") or "void"))
    primitives = {
        "i8": "int8_t",
        "i16": "int16_t",
        "i32": "int32_t",
        "i64": "int64_t",
        "u8": "uint8_t",
        "u16": "uint16_t",
        "u32": "uint32_t",
        "u64": "uint64_t",
        "f32": "float",
        "f64": "double",
        "bool": "bool",
        "void": "void",
    }
    kind = type_info.get("kind") or ""
    if kind in {"class", "record", "struct", "union", "enum"} or (
        name not in primitives and "::" in name
    ):
        const = ""
        head = raw.split("*")[0].split("&")[0]
        if type_info.get("pointee_const") or re.search(r"\bconst\b", head):
            const = "const "
        if reference == "pointer":
            return f"{const}{name}*"
        if reference == "reference":
            rvalue = (type_info.get("reference_kind") == "rvalue") or ("&&" in raw)
            return f"{const}{name}" + ("&&" if rvalue else "&")
        return name
    if raw:
        return raw
    spelled = primitives.get(name, name)
    if reference == "pointer":
        return spelled + "*"
    if reference == "reference":
        kind_ref = type_info.get("reference_kind") or "lvalue"
        return spelled + ("&&" if kind_ref == "rvalue" else "&")
    return spelled


def type_is_void(type_info: dict[str, Any] | None) -> bool:
    if not isinstance(type_info, dict):
        return True
    return (type_info.get("name") or "void") == "void" and (
        type_info.get("reference") or "value"
    ) == "value"


def type_is_translatable(type_info: dict[str, Any] | None) -> bool:
    if type_info is None:
        return True
    if not isinstance(type_info, dict):
        return False
    kind = type_info.get("kind") or ""
    if kind in {"opaque", "vector", "function"} and (
        type_info.get("reference") or "value"
    ) == "value":
        return False
    if type_info.get("unsupported_reason"):
        return False
    return True


def _find_record(adapter: Any, records: list[Any], type_info: dict[str, Any] | None) -> Any:
    if adapter is None or not isinstance(type_info, dict):
        return None
    name = type_info.get("name") or ""
    if not name:
        return None
    return next(
        (record for record in records if adapter.names_match(record.type_name, name)),
        None,
    )


def return_ok_supported(symbol: Any, adapter: Any = None, records: list[Any] | None = None) -> bool:
    if symbol.kind in {"constructor", "destructor"}:
        return True
    if symbol.ret is None:
        return True
    type_info = symbol.ret.get("type")
    if type_is_void(type_info):
        return True
    if not type_is_translatable(type_info):
        return False
    reference = (type_info or {}).get("reference") or "value"
    if reference in {"pointer", "reference"}:
        return True
    kind = (type_info or {}).get("kind") or ""
    if kind not in {"class", "record", "struct", "union"}:
        return True
    # Class-by-value ok needs a default constructor so the translator can
    # value-initialize the tagged record.  Non-default-constructible RAII
    # returns stay on the original may_unwind symbol.
    record = _find_record(adapter, records or [], type_info)
    if record is None or adapter is None:
        return False
    return adapter.lifecycle_operation_is_executable(record, "default_construct")


def symbol_can_translate(
    symbol: Any, adapter: Any = None, records: list[Any] | None = None
) -> bool:
    if symbol.unwind != "may_unwind":
        return False
    if symbol.visibility and symbol.visibility != "public":
        return False
    if symbol.kind not in {"function", "method", "constructor", "destructor"}:
        return False
    for param in symbol.params:
        if not type_is_translatable(param.get("type")):
            return False
    if not return_ok_supported(symbol, adapter, records):
        return False
    return True


def trivial_lifecycle(available: bool = True, trivial: bool = True, link_name: str = "") -> dict[str, Any]:
    return {
        "available": available,
        "accessible": True,
        "deleted": False,
        "trivial": trivial if available else None,
        "declared": "user" if link_name else "implicit",
        "execution": "direct" if link_name else ("trivial" if trivial and available else "unavailable"),
        "link_name": link_name,
        "unwind": "no_unwind",
    }


def failure_lifecycle() -> dict[str, Any]:
    return {
        "default_construct": trivial_lifecycle(),
        "copy_construct": trivial_lifecycle(trivial=False, link_name=FAILURE_COPY),
        "move_construct": trivial_lifecycle(),
        "copy_assign": trivial_lifecycle(trivial=False, link_name=FAILURE_COPY),
        "move_assign": trivial_lifecycle(),
        "destroy": trivial_lifecycle(trivial=False, link_name=FAILURE_DESTROY),
    }


def param_identifier(index: int, param: dict[str, Any]) -> str:
    name = param.get("name") or f"p{index}"
    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", str(name)):
        return f"p{index}"
    if name in {"self", "this", "try", "catch", "new", "delete"}:
        return f"p{index}"
    return str(name)


def translator_parameters(symbol: Any) -> list[tuple[str, str, dict[str, Any]]]:
    """Return (c++ type, ident, original type_info) including explicit this."""
    out: list[tuple[str, str, dict[str, Any]]] = []
    if symbol.owner and not symbol.is_static:
        owner = cpp_name(symbol.owner)
        cv = "const " if symbol.is_const and symbol.kind == "method" else ""
        this_type = {
            "name": symbol.owner,
            "kind": "class",
            "reference": "pointer",
            "cpp_type": f"{cv}{owner}*",
        }
        out.append((f"{cv}{owner}*", "self", this_type))
    for index, param in enumerate(symbol.params):
        type_info = param.get("type") or {}
        out.append((cpp_spelling_of_type(type_info), param_identifier(index, param), type_info))
    return out


def original_call_expr(symbol: Any, args: list[str]) -> str:
    joined = ", ".join(args)
    owner = cpp_name(symbol.owner)
    if symbol.kind == "constructor":
        return f"::new (static_cast<void*>(self)) {owner}({joined})"
    if symbol.kind == "destructor":
        leaf = class_leaf_name(symbol.owner)
        return f"self->~{leaf}()"
    if symbol.kind == "method" and not symbol.is_static:
        method = symbol.member_name
        if method.startswith("operator"):
            return f"(self->{method}({joined}))"
        return f"self->{method}({joined})"
    if symbol.is_static and owner:
        return f"{owner}::{symbol.member_name}({joined})"
    qualified = free_function_cpp_name(symbol.mangled, symbol.name)
    return f"{qualified}({joined})"


def ok_field_cpp_type(symbol: Any) -> str | None:
    if symbol.kind in {"constructor", "destructor"}:
        return None
    if symbol.ret is None:
        return None
    type_info = symbol.ret.get("type")
    if type_is_void(type_info):
        return None
    spelled = cpp_spelling_of_type(type_info)
    if (type_info or {}).get("reference") == "reference":
        pointee = cpp_spelling_of_type((type_info or {}).get("pointee") or type_info)
        return pointee.rstrip("&").strip() + "*"
    return spelled


def translated_struct_source(record_cpp: str, ok_cpp: str | None) -> str:
    leaf = record_cpp.replace("::", "_")
    if ok_cpp:
        return (
            f"struct {leaf} {{\n"
            f"    std::int8_t tag;\n"
            f"    {ok_cpp} ok;\n"
            f"    DciFailure err;\n"
            f"}};\n"
        )
    return (
        f"struct {leaf} {{\n"
        f"    std::int8_t tag;\n"
        f"    DciFailure err;\n"
        f"}};\n"
    )


ABI_CAPTURE = {
    "msvc": {
        "header": "dci_msvc_failure.hpp",
        "function": "dci_msvc_capture_current",
        "probe_flags": ["-fms-compatibility"],
    },
    "itanium": {
        "header": "dci_itanium_failure.hpp",
        "function": "dci_itanium_capture_current",
        "probe_flags": ["-fno-ms-compatibility"],
    },
}

FAILURE_KNOWN_LAYOUT = {
    "size": 64,
    "align": 8,
    "fields": {
        "type_identity": 0,
        "type_identity_len": 8,
        "message": 16,
        "message_len": 24,
        "payload": 32,
        "payload_size": 40,
        "payload_align": 48,
        "producer_tag": 56,
    },
}


def translator_function_source(
    symbol: Any,
    link_name: str,
    record_cpp: str,
    params: list[tuple[str, str, dict[str, Any]]],
    ok_cpp: str | None,
    capture_function: str = "dci_msvc_capture_current",
) -> str:
    leaf = record_cpp.replace("::", "_")
    param_list = ", ".join(f"{ctype} {ident}" for ctype, ident, _ in params)
    if not param_list:
        param_list = "void"
    args = [ident for _, ident, _ in params]
    if symbol.owner and not symbol.is_static:
        args = args[1:]
    call = original_call_expr(symbol, args)
    lines = [
        f"extern \"C\" {leaf} {link_name}({param_list}) noexcept {{",
        f"    {leaf} out{{}};",
        "    try {",
    ]
    if ok_cpp:
        ret_info = (symbol.ret or {}).get("type") or {}
        if ret_info.get("reference") == "reference":
            lines.append(f"        out.ok = &({call});")
        else:
            lines.append(f"        out.ok = {call};")
    else:
        lines.append(f"        {call};")
    lines.extend(
        [
            "        out.tag = 0;",
            "        return out;",
            "    } catch (...) {",
            "        out.tag = 1;",
            f"        {capture_function}(&out.err);",
            "        return out;",
            "    }",
            "}",
            "",
        ]
    )
    return "\n".join(lines)


def destroy_function_source(record_cpp: str, destroy_link: str, ok_cpp: str | None) -> str:
    leaf = record_cpp.replace("::", "_")
    lines = [
        f"extern \"C\" void {destroy_link}({leaf}* value) noexcept {{",
        "    if (value == nullptr) {",
        "        return;",
        "    }",
    ]
    if ok_cpp and not ok_cpp.endswith("*"):
        # Non-pointer class ok fields need an explicit destructor when they are
        # not trivially destructible.  Only destroy a constructed ok (tag==0).
        if re.search(r"[A-Za-z_]", ok_cpp) and ok_cpp not in {
            "int8_t",
            "int16_t",
            "int32_t",
            "int64_t",
            "uint8_t",
            "uint16_t",
            "uint32_t",
            "uint64_t",
            "float",
            "double",
            "bool",
            "char",
            "int",
            "long",
            "short",
            "unsigned",
        }:
            lines.append("    if (value->tag == 0) {")
            lines.append(f"        value->ok.~{class_leaf_name(ok_cpp)}();")
            lines.append("    }")
    lines.extend(
        [
            "    dci_failure_destroy(&value->err);",
            "}",
            "",
        ]
    )
    return "\n".join(lines)


def header_includes(headers: list[str], capture_header: str = "dci_msvc_failure.hpp") -> str:
    lines = [
        "#include <cstdint>",
        "#include <cstring>",
        "#include <new>",
        f'#include "{capture_header}"',
    ]
    for header in headers:
        posix = Path(header).as_posix()
        lines.append(f"#include \"{posix}\"")
    return "\n".join(lines) + "\n\n"


def parse_layout_probe(output: str) -> dict[str, dict[str, Any]]:
    layouts: dict[str, dict[str, Any]] = {}
    for line in output.splitlines():
        parts = line.strip().split()
        if len(parts) < 3:
            continue
        if parts[0] == "STRUCT" and len(parts) >= 4:
            layouts[parts[1]] = {
                "size": int(parts[2]),
                "align": int(parts[3]),
                "fields": {},
            }
        elif parts[0] == "FIELD" and len(parts) >= 4:
            layouts.setdefault(parts[1], {"size": 0, "align": 1, "fields": {}})
            layouts[parts[1]]["fields"][parts[2]] = int(parts[3])
    return layouts


def run_layout_probe(
    executable: str,
    target: str,
    source: str,
    include_dirs: list[str] | None = None,
    extra_flags: list[str] | None = None,
) -> dict[str, dict[str, Any]]:
    with tempfile.TemporaryDirectory(prefix="vyx_dci_tw_") as temp_dir:
        probe = Path(temp_dir) / "layout_probe.cpp"
        binary = Path(temp_dir) / "layout_probe.exe"
        probe.write_text(source, encoding="utf-8")
        command = [
            executable,
            "-std=c++17",
            "-O0",
            "-fexceptions",
            "-fcxx-exceptions",
            "-target",
            target,
            "-I",
            str(RUNTIME_DIR),
        ]
        command.extend(extra_flags or [])
        for directory in include_dirs or []:
            command.extend(["-I", directory])
        command.extend(
            [
                str(probe),
                "-o",
                str(binary),
            ]
        )
        proc = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        if proc.returncode != 0 or not binary.is_file():
            raise RuntimeError(proc.stdout)
        run = subprocess.run(
            [str(binary)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        if run.returncode != 0:
            raise RuntimeError(run.stdout)
        return parse_layout_probe(run.stdout)


def failure_record_layout(adapter: Any, measured: dict[str, Any]) -> Any:
    fields = []
    order = [
        ("type_identity", "const char*"),
        ("type_identity_len", "uint64_t"),
        ("message", "const char*"),
        ("message_len", "uint64_t"),
        ("payload", "const uint8_t*"),
        ("payload_size", "uint64_t"),
        ("payload_align", "uint64_t"),
        ("producer_tag", "uint32_t"),
    ]
    offsets = measured.get("fields") or {}
    for name, cpp_type in order:
        if name not in offsets:
            raise RuntimeError(f"layout probe missing dci.Failure field {name}")
        fields.append(adapter.FieldLayout(name, cpp_type, offsets[name], "public"))
    return adapter.RecordLayout(
        FAILURE_RECORD,
        size=int(measured["size"]),
        alignment=int(measured["align"]),
        fields=fields,
        traits={
            "pod": False,
            "trivial": False,
            "trivially_copyable": False,
            "can_pass_in_registers": False,
            "standard_layout": True,
            "aggregate": True,
            "polymorphic": False,
            "abstract": False,
            "final": False,
            "has_virtual_bases": False,
        },
        lifecycle=failure_lifecycle(),
    )


def translated_record_layout(
    adapter: Any,
    record_name: str,
    ok_cpp: str | None,
    ok_type: dict[str, Any] | None,
    destroy_link: str,
    measured: dict[str, Any],
) -> Any:
    offsets = measured.get("fields") or {}
    fields = [adapter.FieldLayout("tag", "int8_t", offsets["tag"], "public")]
    if ok_cpp:
        if "ok" not in offsets:
            raise RuntimeError(f"layout probe missing ok for {record_name}")
        cpp_type = ok_cpp
        fields.append(adapter.FieldLayout("ok", cpp_type, offsets["ok"], "public"))
    if "err" not in offsets:
        raise RuntimeError(f"layout probe missing err for {record_name}")
    fields.append(adapter.FieldLayout("err", "dci::Failure", offsets["err"], "public"))
    lifecycle = {
        "default_construct": trivial_lifecycle(),
        "copy_construct": trivial_lifecycle(trivial=False),
        "move_construct": trivial_lifecycle(),
        "copy_assign": trivial_lifecycle(trivial=False),
        "move_assign": trivial_lifecycle(),
        "destroy": trivial_lifecycle(trivial=False, link_name=destroy_link),
    }
    return adapter.RecordLayout(
        record_name,
        size=int(measured["size"]),
        alignment=int(measured["align"]),
        fields=fields,
        traits={
            "pod": False,
            "trivial": False,
            "trivially_copyable": False,
            "can_pass_in_registers": False,
            "standard_layout": True,
            "aggregate": True,
            "polymorphic": False,
            "abstract": False,
            "final": False,
            "has_virtual_bases": False,
            "closed_tagged_return": True,
        },
        lifecycle=lifecycle,
    )


def align_up(value: int, alignment: int) -> int:
    if alignment <= 1:
        return value
    return (value + alignment - 1) // alignment * alignment


def cxx_struct_layout(fields: list[tuple[str, int, int]]) -> dict[str, Any]:
    offset = 0
    rec_align = 1
    offsets: dict[str, int] = {}
    for name, size, alignment in fields:
        align = alignment if alignment > 0 else 1
        offset = align_up(offset, align)
        offsets[name] = offset
        offset += size
        rec_align = max(rec_align, align)
    return {"size": align_up(offset, rec_align), "align": rec_align, "fields": offsets}


def primitive_ok_layout(spelling: str, pointer_width: int, data_model: str) -> tuple[int, int] | None:
    width = max(pointer_width // 8, 4)
    text = (spelling or "").strip()
    if text.endswith("*") or text.endswith("&"):
        return width, width
    table = {
        "int8_t": (1, 1),
        "uint8_t": (1, 1),
        "bool": (1, 1),
        "char": (1, 1),
        "int16_t": (2, 2),
        "uint16_t": (2, 2),
        "short": (2, 2),
        "int32_t": (4, 4),
        "uint32_t": (4, 4),
        "int": (4, 4),
        "float": (4, 4),
        "int64_t": (8, 8),
        "uint64_t": (8, 8),
        "double": (8, 8),
        "long long": (8, 8),
        "unsigned long long": (8, 8),
    }
    if text in table:
        return table[text]
    if text in {"long", "unsigned long"}:
        long_size = 8 if pointer_width == 64 and data_model == "lp64" else 4
        return long_size, long_size
    return None


def ok_field_layout(
    ok_cpp: str | None,
    adapter: Any,
    records: list[Any],
    pointer_width: int,
    data_model: str,
) -> tuple[int, int] | None:
    if not ok_cpp:
        return None
    primitive = primitive_ok_layout(ok_cpp, pointer_width, data_model)
    if primitive is not None:
        return primitive
    record = next(
        (item for item in records if adapter.names_match(item.type_name, ok_cpp.replace("::", "."))),
        None,
    )
    if record is None:
        record = next(
            (item for item in records if adapter.names_match(item.type_name, ok_cpp)),
            None,
        )
    if record is None or record.size <= 0 or record.alignment <= 0:
        return None
    return int(record.size), int(record.alignment)


def compute_translated_layouts(
    plans: list[dict[str, Any]],
    adapter: Any,
    records: list[Any],
    pointer_width: int,
    data_model: str,
) -> dict[str, dict[str, Any]]:
    measured: dict[str, dict[str, Any]] = {"dci::Failure": dict(FAILURE_KNOWN_LAYOUT)}
    measured["dci::Failure"]["fields"] = dict(FAILURE_KNOWN_LAYOUT["fields"])
    failure_size = int(FAILURE_KNOWN_LAYOUT["size"])
    failure_align = int(FAILURE_KNOWN_LAYOUT["align"])
    for plan in plans:
        fields: list[tuple[str, int, int]] = [("tag", 1, 1)]
        if plan["ok_cpp"]:
            ok_layout = ok_field_layout(
                plan["ok_cpp"], adapter, records, pointer_width, data_model
            )
            if ok_layout is None:
                continue
            fields.append(("ok", ok_layout[0], ok_layout[1]))
        fields.append(("err", failure_size, failure_align))
        measured[plan["record_name"]] = cxx_struct_layout(fields)
    return measured


def c_function_symbol(
    adapter: Any,
    name: str,
    params: list[dict[str, Any]],
    ret: dict[str, Any] | None,
) -> Any:
    return adapter.Symbol(
        name=name,
        owner="",
        member_name=name,
        mangled=name,
        kind="function",
        calling_convention="c",
        params=params,
        ret=ret,
        visibility="public",
        native_calling_convention="cdecl",
        unwind="no_unwind",
    )


def attach_translate_unwind(
    adapter: Any,
    *,
    exported_symbols: list[dict[str, Any]],
    records: list[Any],
    original_symbols: list[Any],
    headers: list[str],
    target: str,
    toolchain: Any,
    machine_signatures: dict[str, dict[str, Any]] | None,
) -> tuple[list[dict[str, Any]], list[Any], list[dict[str, Any]], str]:
    """Append translator symbols/layouts and return stub source."""
    empty: tuple[list[dict[str, Any]], list[Any], list[dict[str, Any]], str] = (
        [],
        [],
        [],
        "",
    )
    descriptor = adapter.target_contract(target)
    abi_family = descriptor.get("abi_family") or ""
    capture = ABI_CAPTURE.get(abi_family)
    if capture is None:
        return empty
    exported_links = {
        symbol.get("link_name")
        for symbol in exported_symbols
        if symbol.get("link_name")
    }
    candidates = [
        symbol
        for symbol in original_symbols
        if symbol_can_translate(symbol, adapter, records) and symbol.mangled in exported_links
    ]
    if not candidates:
        return empty
    executable = getattr(toolchain, "executable", "") or ""

    used_links = set(exported_links)
    plans: list[dict[str, Any]] = []
    struct_sources: list[str] = []
    for symbol in candidates:
        tr_link = translator_link_name(symbol.mangled or symbol.name, used_links)
        record_name = translated_record_name(tr_link)
        ok_cpp = ok_field_cpp_type(symbol)
        params = translator_parameters(symbol)
        destroy_link = translator_link_name(tr_link + "_destroy", used_links)
        plans.append(
            {
                "symbol": symbol,
                "tr_link": tr_link,
                "record_name": record_name,
                "record_cpp": record_name,
                "ok_cpp": ok_cpp,
                "params": params,
                "destroy_link": destroy_link,
            }
        )
        struct_sources.append(translated_struct_source(record_name, ok_cpp))

    probe_source = (
        "#include <cstddef>\n#include <cstdio>\n"
        + header_includes(headers, capture["header"])
        + "".join(struct_sources)
        + "int main() {\n"
        + '    std::printf("STRUCT %s %zu %zu\\n", "dci::Failure", sizeof(DciFailure), alignof(DciFailure));\n'
        + '    std::printf("FIELD %s %s %zu\\n", "dci::Failure", "type_identity", offsetof(DciFailure, type_identity));\n'
        + '    std::printf("FIELD %s %s %zu\\n", "dci::Failure", "type_identity_len", offsetof(DciFailure, type_identity_len));\n'
        + '    std::printf("FIELD %s %s %zu\\n", "dci::Failure", "message", offsetof(DciFailure, message));\n'
        + '    std::printf("FIELD %s %s %zu\\n", "dci::Failure", "message_len", offsetof(DciFailure, message_len));\n'
        + '    std::printf("FIELD %s %s %zu\\n", "dci::Failure", "payload", offsetof(DciFailure, payload));\n'
        + '    std::printf("FIELD %s %s %zu\\n", "dci::Failure", "payload_size", offsetof(DciFailure, payload_size));\n'
        + '    std::printf("FIELD %s %s %zu\\n", "dci::Failure", "payload_align", offsetof(DciFailure, payload_align));\n'
        + '    std::printf("FIELD %s %s %zu\\n", "dci::Failure", "producer_tag", offsetof(DciFailure, producer_tag));\n'
    )
    for plan in plans:
        leaf = plan["record_name"].replace("::", "_")
        probe_source += (
            f'    std::printf("STRUCT %s %zu %zu\\n", "{plan["record_name"]}", '
            f"sizeof({leaf}), alignof({leaf}));\n"
            f'    std::printf("FIELD %s %s %zu\\n", "{plan["record_name"]}", "tag", offsetof({leaf}, tag));\n'
        )
        if plan["ok_cpp"]:
            probe_source += (
                f'    std::printf("FIELD %s %s %zu\\n", "{plan["record_name"]}", "ok", offsetof({leaf}, ok));\n'
            )
        probe_source += (
            f'    std::printf("FIELD %s %s %zu\\n", "{plan["record_name"]}", "err", offsetof({leaf}, err));\n'
        )
    probe_source += "    return 0;\n}\n"

    pointer_width = int(descriptor.get("pointer_width") or 64)
    data_model = str(descriptor.get("data_model") or "")
    measured: dict[str, dict[str, Any]] = {}
    if executable:
        try:
            include_dirs = sorted({str(Path(header).parent) for header in headers if header})
            measured = run_layout_probe(
                executable,
                target,
                probe_source,
                include_dirs,
                extra_flags=list(capture["probe_flags"]),
            )
        except (OSError, RuntimeError):
            measured = {}
    if "dci::Failure" not in measured:
        measured = compute_translated_layouts(
            plans, adapter, records, pointer_width, data_model
        )
    if "dci::Failure" not in measured:
        return empty

    extra_records = [failure_record_layout(adapter, measured["dci::Failure"])]
    extra_symbols: list[Any] = [
        c_function_symbol(
            adapter,
            FAILURE_DESTROY,
            [
                {
                    "name": "failure",
                    "type": {
                        "name": FAILURE_RECORD,
                        "kind": "class",
                        "reference": "pointer",
                        "cpp_type": "DciFailure*",
                    },
                    "location": "abi",
                }
            ],
            None,
        ),
        c_function_symbol(
            adapter,
            FAILURE_COPY,
            [
                {
                    "name": "src",
                    "type": {
                        "name": FAILURE_RECORD,
                        "kind": "class",
                        "reference": "pointer",
                        "cpp_type": "const DciFailure*",
                    },
                    "location": "abi",
                },
                {
                    "name": "dst",
                    "type": {
                        "name": FAILURE_RECORD,
                        "kind": "class",
                        "reference": "pointer",
                        "cpp_type": "DciFailure*",
                    },
                    "location": "abi",
                },
            ],
            {"type": adapter.dci_type("int"), "location": "abi"},
        ),
    ]
    extra_symbols[0].parameter_ownerships[0] = "borrow_mut"
    extra_symbols[1].parameter_ownerships[0] = "borrow"
    extra_symbols[1].parameter_ownerships[1] = "borrow_mut"
    stub_parts = [header_includes(headers, capture["header"])]
    stub_requests: list[dict[str, Any]] = []
    translator_json: list[dict[str, Any]] = []

    for plan in plans:
        measured_record = measured.get(plan["record_name"])
        if not measured_record:
            continue
        record = translated_record_layout(
            adapter,
            plan["record_name"],
            plan["ok_cpp"],
            None,
            plan["destroy_link"],
            measured_record,
        )
        extra_records.append(record)
        extra_symbols.append(
            c_function_symbol(
                adapter,
                plan["destroy_link"],
                [
                    {
                        "name": "value",
                        "type": {
                            "name": plan["record_name"],
                            "kind": "class",
                            "reference": "pointer",
                            "cpp_type": plan["record_name"].replace("::", "_") + "*",
                        },
                        "location": "abi",
                    }
                ],
                None,
            )
        )
        extra_symbols[-1].parameter_ownerships[0] = "borrow_mut"
        tr_params = [
            {"name": ident, "type": type_info, "location": "abi"}
            for _, ident, type_info in plan["params"]
        ]
        translator = adapter.Symbol(
            name=plan["symbol"].name + "_translated",
            owner="",
            member_name=plan["tr_link"],
            mangled=plan["tr_link"],
            kind="function",
            calling_convention="c",
            params=tr_params,
            ret={
                "type": {
                    "name": plan["record_name"],
                    "kind": "class",
                    "reference": "value",
                    "cpp_type": plan["record_name"].replace("::", "_"),
                },
                "location": "abi",
            },
            visibility="public",
            native_calling_convention="cdecl",
            unwind="no_unwind",
        )
        ownerships: dict[int, str] = {}
        offset = 0
        original = plan["symbol"]
        if original.owner and not original.is_static:
            ownerships[0] = "borrow" if original.is_const else "borrow_mut"
            offset = 1
        for index, value in (original.parameter_ownerships or {}).items():
            ownerships[int(index) + offset] = value
        translator.parameter_ownerships = ownerships
        extra_symbols.append(translator)
        stub_parts.append(translated_struct_source(plan["record_name"], plan["ok_cpp"]))
        stub_parts.append(
            translator_function_source(
                plan["symbol"],
                plan["tr_link"],
                plan["record_name"],
                plan["params"],
                plan["ok_cpp"],
                capture["function"],
            )
        )
        stub_parts.append(
            destroy_function_source(plan["record_name"], plan["destroy_link"], plan["ok_cpp"])
        )
        stub_requests.append(
            {
                "id": f"translate.{plan['symbol'].mangled or plan['symbol'].name}",
                "kind": "operation_wrapper",
                "target": plan["symbol"].mangled,
                "synthesis": {
                    "strategy": "translate_unwind",
                    "target": plan["symbol"].mangled,
                },
                "wrapper": {"link_name": plan["tr_link"]},
            }
        )

    all_records = list(records) + extra_records
    for extra in extra_symbols:
        try:
            translator_json.append(
                adapter.symbol_json(extra, all_records, target, machine_signatures)
            )
        except adapter.AdapterContractError:
            continue
    # Mark translators as translated leaves that are themselves no_unwind.
    by_link = {item["link_name"]: item for item in translator_json if item.get("link_name")}
    for request in stub_requests:
        wrapper = request["wrapper"]["link_name"]
        item = by_link.get(wrapper)
        if item is None:
            continue
        item["control_flow"] = {
            "default_boundary": "no_unwind",
            "boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
            "unwind": "no_unwind",
            "boundary_action": "direct",
        }

    stub_source = "".join(stub_parts)
    return translator_json, extra_records, stub_requests, stub_source

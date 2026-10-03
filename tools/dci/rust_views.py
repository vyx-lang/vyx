"""rustc-measured, explicitly named consumer views of native Rust borrows."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import tempfile


def measure_slice_views(root, crate, values, types, rustc, target, edition, rustc_args):
    from dci_adapter_rust import copy_crate_tree, probe_environment, run_checked, RustAdapterError

    unique = {}
    for value in values:
        if (value.kind == "reference" and value.fat and value.element is not None
                and value.element.kind in {"slice", "str"}):
            key = "str" if value.element.kind == "str" else types.active_name(value.element.element)
            unique.setdefault(key, value)
    if not unique:
        return {}, []
    if target.triple != run_checked([rustc, "-vV"]).stdout.split("host: ", 1)[1].splitlines()[0]:
        raise RustAdapterError("native borrow views require a runnable producer target for layout measurement")
    lines = ["", "fn main() {"]
    for key, value in unique.items():
        if value.element.kind == "str":
            expression = 'let view: &str = "";'
            pointer = "view.as_ptr() as usize"
        else:
            element = value.element.element
            spelling = types.rust_spelling(element)
            if spelling is None:
                raise RustAdapterError(f"cannot name native slice element {key!r} for measurement")
            expression = (f"let pointer = core::mem::align_of::<{spelling}>() as *const {spelling}; "
                          f"let view: &[{spelling}] = unsafe {{ core::slice::from_raw_parts(pointer, 0) }};")
            pointer = "view.as_ptr() as usize"
        lines.extend([
            "{", expression,
            "assert_eq!(core::mem::size_of_val(&view), 2 * core::mem::size_of::<usize>());",
            "let words: [usize; 2] = unsafe { core::mem::transmute_copy(&view) };",
            f"let data_offset = words.iter().position(|word| *word == {pointer}).unwrap() * core::mem::size_of::<usize>();",
            "let len_offset = words.iter().position(|word| *word == view.len()).unwrap() * core::mem::size_of::<usize>();",
            "assert_ne!(data_offset, len_offset);",
            f'println!("{{}} {{}} {{}} {{}} {{}}", {json.dumps(key)}, core::mem::size_of_val(&view), core::mem::align_of_val(&view), data_offset, len_offset);',
            "}",
        ])
    lines.append("}")
    with tempfile.TemporaryDirectory(prefix="vyx-rust-view-") as directory:
        temp = Path(directory)
        copied = copy_crate_tree(root, temp)
        copied.write_text(copied.read_text(encoding="utf-8") + "\n" + "\n".join(lines), encoding="utf-8")
        executable = temp / ("view.exe" if os.name == "nt" else "view")
        run_checked([rustc, str(copied), "--crate-name", "vyx_dci_view", "--crate-type", "bin",
                     "--edition", edition, "--target", target.triple, "-C", "panic=abort",
                     "-o", str(executable), *rustc_args],
                    env=probe_environment(root, temp, rustc_args), cwd=copied.parent, timeout=180)
        output = run_checked([str(executable)], timeout=30).stdout
    layouts = []
    bindings = {}
    measured = {parts[0]: [int(p) for p in parts[1:]]
                for line in output.splitlines() if len(parts := line.split()) == 5}
    for key, value in unique.items():
        if key not in measured:
            raise RustAdapterError(f"rustc omitted the native borrow layout for {key}")
        size, align, data_offset, len_offset = measured[key]
        width = target.pointer_width // 8
        if (size, align, data_offset, len_offset) != (2 * width, width, 0, width):
            raise RustAdapterError(f"producer borrow layout cannot use the current split ABI: {key}: {measured[key]}")
        name = "dci.RustStr" if key == "str" else "dci.RustSlice_" + re.sub(r"[^A-Za-z0-9_]", "_", key)
        data_type = {"name": "u8" if key == "str" else key, "kind": "primitive" if key == "str" else types.type_json(value.element.element)["kind"],
                     "reference": "pointer", "nullable": False}
        fields = [{"name": "data", "offset": data_offset, "type": data_type, "visibility": "public"},
                  {"name": "len", "offset": len_offset,
                   "type": {"name": "usize", "kind": "primitive", "reference": "value", "nullable": False},
                   "visibility": "public"}]
        layouts.append({"type_name": name, "size": size, "alignment": align,
                        "representation": "stable", "has_vtable": False, "is_pod": True,
                        "is_trivially_destructible": True, "bases": [], "fields": fields,
                        "lifecycle": {"ownership_model": "value", "copy_semantics": "trivial",
                                      "move_semantics": "trivial", "destruction": "trivial",
                                      "moved_from_state": "valid", "operations": {}},
                        "rust": {"view_of": value.spelling, "metadata_kind": "len",
                                 "measured_by": "rustc-native-borrow-view"}})
        for original in values:
            if original.kind == "reference" and original.fat and original.element is not None:
                original_key = "str" if original.element.kind == "str" else (
                    types.active_name(original.element.element) if original.element.kind == "slice" else None)
                if original_key == key:
                    bindings[original.spelling] = name
    return bindings, layouts

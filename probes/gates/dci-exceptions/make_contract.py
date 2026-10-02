"""Native fixture ABI (static_assert in native.cpp) and versioned C++ unwind."""
import copy
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/dci"))
import dcib

base = json.loads((ROOT / "tests/checks/dci/consumer/dci/base.dci").read_text())
base["profile"]["id"] = "dci.shared-unwind.regression"
base["control_flow"] = {
    "default_boundary": "shared_abi",
    "propagation": {"mode": "shared_abi", "abi": "dci.eh.msvc-cxx.v1"},
}
if len(sys.argv) > 1 and sys.argv[1] == "linux":
    base["target"].update(triple="x86_64-unknown-linux-gnu", object_format="elf", environment="linux", abi_family="sysv")
    base["control_flow"]["propagation"]["abi"] = "dci.eh.itanium-cxx.v1"
cc = "win64" if base["target"]["environment"] == "windows" else "x86_64_sysv"
guard_ptr = {"kind": "class", "name": "eh::Guard", "reference": "pointer"}
i32 = {"name": "i32", "kind": "primitive", "nullable": False, "reference": "value"}
i64 = dict(i32, name="i64")
symbols = []
for kind, member, link in (("constructor", "constructor", "guard_construct"),
                           ("destructor", "~Guard", "guard_destroy")):
    ctor = kind == "constructor"
    params = [{"name": "id", "type": i32, "ownership": "copy", "location": "abi"}] if ctor else []
    abi_params = [{"index": 0, "name": "id", "type": i32, "passing": "direct", "size": 4}] if ctor else []
    symbols.append({
        "kind": kind, "name": "eh::Guard::" + member, "member_name": member,
        "owner": "eh::Guard", "link_name": link, "mangled": link, "visibility": "public",
        "calling_convention": cc, "params": params, "return": None,
        "control_flow": dict(copy.deepcopy(base["control_flow"]), unwind="may_unwind"),
        "abi": {"calling_convention": cc, "parameters": abi_params,
                "receiver": {"type": guard_ptr, "ownership": "borrow_mut", "passing": "direct", "this_adjust": 0},
                "return": {"passing": "direct", "type": guard_ptr if ctor else {"kind": "primitive", "name": "void"}},
                "unwind": "may_unwind"},
    })
probe = copy.deepcopy(base["exports"]["symbols"][0])
probe["name"] = probe["link_name"] = "contract_throw"
symbols.append(probe)
layout = {"type_name": "eh::Guard", "size": 32, "alignment": 8, "representation": "native",
          "is_pod": False, "is_trivially_destructible": False, "bases": [], "has_vtable": False,
          "fields": [{"name": name, "offset": n * 8, "type": i64} for n, name in enumerate(("id", "b", "c", "d"))]}
base["exports"] = {"symbols": symbols, "layouts": [layout]}
out = Path(__file__).with_name("shared.dci")
out.write_text(json.dumps(base, indent=2), encoding="utf-8")
out.with_suffix(".dcib").write_bytes(dcib.encode(base))

# A separate automatic-lifecycle contract exercises inline stack storage.
record = {"name": "eh.Inline", "kind": "record", "nullable": False, "reference": "value"}
pointer = dict(record, reference="pointer")
def inline_symbol(name, parameter, returned):
    return {
        "name": name, "link_name": name, "kind": "function", "calling_convention": "c",
        "params": [{"name": "value", "type": parameter, "ownership": "copy" if returned else "move", "location": "abi"}],
        "return": {"type": record, "ownership": "owned", "location": "abi"} if returned else None,
        "control_flow": {"default_boundary": "no_unwind", "propagation": {"mode": "forbidden"}, "unwind": "no_unwind"},
        "abi": {"calling_convention": "c", "variadic": False,
                "parameters": [{"index": 0, "passing": "direct", "size": 8}],
                "return": {"passing": "direct", "size": 8} if returned else {"passing": "ignore"}},
    }
inline = copy.deepcopy(base)
inline["profile"]["lifecycle_binding"] = "automatic"
inline["exports"] = {
    "symbols": [inline_symbol("inline_create", i64, True), inline_symbol("inline_destroy", pointer, False), probe],
    "layouts": [{"type_name": "eh.Inline", "size": 8, "alignment": 8, "representation": "stable",
                 "is_pod": True, "is_trivially_destructible": False,
                 "fields": [{"name": "slot", "offset": 0, "type": i64}],
                 "lifecycle": {"ownership_model": "unique", "copy_semantics": "forbidden",
                               "move_semantics": "forbidden", "destruction": "operation",
                               "moved_from_state": "valid", "allocator_domain": "inline",
                               "operations": {"create": {"symbol": "inline_create", "availability": "required", "no_unwind": True},
                                              "destroy": {"symbol": "inline_destroy", "availability": "required", "no_unwind": True}}}}],
}
out.with_name("inline.dci").write_text(json.dumps(inline, indent=2), encoding="utf-8")
out.with_name("inline.dcib").write_bytes(dcib.encode(inline))

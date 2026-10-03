"""Check provenance and discovered native std::vector facts for this gate."""
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))
from tools.dci.dcib import decode


def canon(name):
    return name.replace("::", ".").replace(" ", "").replace(".<", "<")


base = decode((HERE / "vector.dcib").read_bytes())
assert all(not value for value in base["exports"].values()), "base contract pre-exported a specialization"
assert (HERE / "vector.hpp").read_text().strip() == "#pragma once\n#include <vector>", "producer must be the original template header"
assert base["control_flow"]["default_boundary"] == "shared_abi"

closed = decode((HERE / ".cache/dci_closed_facts_dci_vector.dcib").read_bytes())
owners = {"std.vector<i32>", "std.vector<f64>"}
layouts = {canon(item["type_name"]): item for item in closed["exports"]["layouts"]}
for owner in owners:
    assert owner in layouts, f"no producer-measured layout for {owner}"
    assert layouts[owner]["size"] > 0 and layouts[owner]["alignment"] > 0
    symbols = [s for s in closed["exports"]["symbols"] if canon(s.get("owner", "")) == owner]
    for name in ("push_back", "at", "data", "size", "capacity", "reserve", "clear", "pop_back"):
        selected = [s for s in symbols if s.get("member_name") == name]
        assert selected, f"no original {owner}::{name} ABI"
        assert all(s.get("cpp_materialization", {}).get("kind") == "native_member" for s in selected)
    assert any(s["kind"] == "destructor" and s.get("cpp_materialization", {}).get("kind") == "native_member" for s in symbols)
    constructors = [s for s in symbols if s.get("cpp_materialization", {}).get("kind") == "list_constructor"]
    assert constructors, f"no producer-checked brace constructor for {owner}"
    assert any(len(s["params"]) == (5 if owner.endswith("i32>") else 3) for s in constructors)

requests = (HERE / ".cache/dci_open_requests.txt").read_text()
assert "list-init\t" in requests, "consumer did not discover brace constructors"
for type_name in ("i32", "f64"):
    assert re.search(r"std\.vector(?:::)?<" + type_name + r">", requests), "missing consumer instance request"
print("open original std::vector<T> -> discovered i32/f64 native ABI OK")

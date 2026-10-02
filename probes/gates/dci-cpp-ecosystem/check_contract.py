"""Verify this run measures the real ICU object and admits original symbols."""
import json
import sys
from pathlib import Path

contract = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
exports = contract["exports"]
layouts = {r["type_name"]: r for r in exports["layouts"]}
record = layouts["UnicodeString"]
assert record["size"] == 64 and record["alignment"] == 8, record
assert record["lifecycle"]["operations"]["destroy"]["availability"] == "required"
assert contract["control_flow"]["default_boundary"] == "shared_abi"
symbols = exports["symbols"]
required = {
    "constructor", "~UnicodeString", "length", "getCapacity", "charAt",
    "char32At", "countChar32", "moveIndex32", "caseCompare",
    "compareCodePointOrder", "append", "toUpper", "toLower",
}
assert required <= {s["member_name"] for s in symbols}, required
for symbol in symbols:
    if symbol["member_name"] not in required:
        continue
    assert "UnicodeString@icu_78@@" in symbol["link_name"], symbol
    assert "cpp_materialization" not in symbol, symbol
    if symbol["member_name"] in {"append", "toUpper", "toLower"}:
        assert symbol["return"]["ownership"] == "borrow", symbol
print("ICU original layouts and symbols OK")

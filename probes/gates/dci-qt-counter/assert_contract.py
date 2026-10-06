"""Check the exact Qt surface consumed by the counter, including pointer layers."""
import argparse
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("tools", type=Path)
parser.add_argument("contract", type=Path)
parser.add_argument("--unwind", action="store_true", help="validate the separate exception test contract")
args = parser.parse_args()
sys.path.insert(0, str(args.tools))
import dcib


def shape(ty):
    reference = ty.get("reference", "value")
    if reference in ("pointer", "pointer_const", "pointer_mut", "reference"):
        prefix = "&" if reference == "reference" else "*"
        return prefix + (shape(ty["pointee"]) if "pointee" in ty else ty["name"])
    return ty["name"]


doc = dcib.decode(args.contract.read_bytes())
flow = doc["control_flow"]
assert flow["default_boundary"] == "shared_abi", flow
assert flow["propagation"] == {"mode": "shared_abi", "abi": "dci.eh.msvc-cxx.v1"}, flow
assert not any(request.get("synthesis", {}).get("strategy") == "translate_unwind"
               for request in doc["exports"].get("stub_requests", []))
symbols = doc["exports"]["symbols"]
assert not any(symbol.get("link_name", "").startswith("dci_tr_") for symbol in symbols)
helpers = ("qt_counter_watch", "qt_counter_throw", "qt_counter_call_visible")
if args.unwind:
    thrower = [symbol for symbol in symbols if symbol.get("name") == "qt_counter_throw"]
    assert len(thrower) == 1 and thrower[0]["abi"]["unwind"] == "may_unwind", thrower
    for helper in ("qt_counter_watch", "qt_counter_call_visible"):
        matches = [symbol for symbol in symbols if symbol.get("name") == helper]
        assert len(matches) == 1 and matches[0]["params"][0]["ownership"] == "borrow_mut", matches
else:
    assert not any(symbol.get("name") in helpers for symbol in symbols), "test helpers leaked into application contract"
required = [
    ("QApplication", "constructor", ["&i32", "**char", "i32"]),
    ("QString", "constructor", ["*char"]),
    ("QPushButton", "constructor", ["&QString", "*QWidget"]),
    ("QLCDNumber", "constructor", ["*QWidget"]),
    ("QLCDNumber", "intValue", []),
    ("QAbstractButton", "setDown", ["bool"]),
    ("QCoreApplication", "exit", ["i32"]),
    ("QObject", "timerEvent", ["*QTimerEvent"]),
    ("QWidget", "mouseReleaseEvent", ["*QMouseEvent"]),
    ("QApplication", "~QApplication", []),
    ("QString", "~QString", []),
    ("QPushButton", "~QPushButton", []),
    ("QLCDNumber", "~QLCDNumber", []),
]
for owner, member, params in required:
    matches = [s for s in symbols if s.get("owner") == owner
               and s.get("member_name") == member
               and [shape(p["type"]) for p in s.get("params", [])] == params]
    if len(matches) != 1:
        raise AssertionError(f"expected one {owner}::{member}{params}, found {len(matches)}")
    assert matches[0].get("link_name"), f"missing native symbol for {owner}::{member}"
print(f"Qt contract OK: {len(required)} exact signatures, {len(symbols)} symbols")

import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
import dcib

doc = dcib.decode(Path(sys.argv[2]).read_bytes())
syms = doc["exports"].get("symbols") or []
wanted = ["QApplication", "QLCDNumber", "QPushButton", "QWidget", "QAbstractButton"]
names = {}
for s in syms:
    owner = s.get("owner") or ""
    name = s.get("name") or ""
    for w in wanted:
        if owner == w or name == w or name.startswith(w + "::"):
            names.setdefault(w, []).append(s.get("member_name") or name)
print("symbols", len(syms), "rejected", len(doc["exports"].get("rejected_symbols") or []))
for w in wanted:
    ms = names.get(w) or []
    print(w, "count", len(ms), "sample", ms[:12])
missing = [w for w in wanted if w not in names]
if missing:
    print("FAILED missing", missing, file=sys.stderr)
    sys.exit(1)
qapp = [
    s
    for s in syms
    if (s.get("owner") or "") == "QApplication"
    and (s.get("member_name") or "") == "constructor"
]
print("QApplication constructors", len(qapp))
if not qapp:
    print("FAILED QApplication constructor not exported", file=sys.stderr)
    sys.exit(1)
qstr = [
    s
    for s in syms
    if (s.get("owner") or "") == "QString"
    and (s.get("member_name") or "") == "constructor"
    and any(((p.get("type") or {}).get("cpp_type") == "const char *") for p in (s.get("params") or []))
]
print("QString(const char*) constructors", len(qstr))
if not qstr:
    print("FAILED QString(const char*) constructor not exported", file=sys.stderr)
    sys.exit(1)
print("assert OK")

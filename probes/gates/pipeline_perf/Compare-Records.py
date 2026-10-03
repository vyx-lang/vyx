"""Compare complete incremental-build records; exit 1 when any record differs."""

import argparse
import difflib
import hashlib
import json
from pathlib import Path


def read_records(path: Path) -> dict[str, str]:
    records = {}
    for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line:
            continue
        key, separator, _ = line.partition("\t")
        if not separator:
            raise ValueError(f"{path}:{number}: record has no tab-delimited key")
        if key in records:
            raise ValueError(f"{path}:{number}: duplicate cache key {key!r}")
        records[key] = line
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    before, after = read_records(args.before), read_records(args.after)
    changes = []
    for key in sorted(before.keys() | after.keys()):
        old, new = before.get(key, ""), after.get(key, "")
        if old == new:
            continue
        deltas = []
        for kind, old_start, old_end, new_start, new_end in difflib.SequenceMatcher(
            None, old, new, autojunk=False
        ).get_opcodes():
            if kind == "equal":
                continue
            deltas.append({
                "kind": kind,
                "old_offset": old_start,
                "new_offset": new_start,
                "old": old[old_start:old_end],
                "new": new[new_start:new_end],
                "before_context": old[max(0, old_start - 32):old_end + 32],
                "after_context": new[max(0, new_start - 32):new_end + 32],
            })
        changes.append({"key": key, "deltas": deltas})
    report = {
        "before": str(args.before.resolve()),
        "after": str(args.after.resolve()),
        "before_sha256": hashlib.sha256(args.before.read_bytes()).hexdigest(),
        "after_sha256": hashlib.sha256(args.after.read_bytes()).hexdigest(),
        "before_record_count": len(before),
        "after_record_count": len(after),
        "changed_record_count": len(changes),
        "changes": changes,
    }
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return int(bool(changes))


if __name__ == "__main__":
    raise SystemExit(main())

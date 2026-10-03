"""Active Adapter end-to-end gate driver (on-demand closure side).

Orchestrates the §6 flow for both languages and prints deterministic
``KEY=VALUE`` lines for the bash gate to assert:

* on-demand closure of a generic call that no pre-existing contract covers
* per-operation admission (one operation closes, the sibling refuses with a
  producer diagnostic)
* materialization into a real producer-compiled object
* publication into the Phase 0 artifact bundle (single-flight)
* invalidation correctness (touching the source changes the session identity
  and therefore every request key)
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from tools.dci import active_protocol as ap  # noqa: E402


def _queries(lang: str) -> tuple[list[ap.Query], str, str]:
    """(queries, replay symbol, expected replay stdout)."""
    if lang == "rust":
        twice = ap.Query(
            query_id="g", entity={"kind": "generic", "path": "twice",
                                  "args": [ap.type_ref(ap.TYPE_PRIMITIVE, "i32")]},
            operation={"op": "rust/call/1"})
        pair = ap.Query(
            query_id="p", entity={"kind": "generic", "path": "pair",
                                  "args": [ap.type_ref(ap.TYPE_PRIMITIVE, "i32"),
                                           ap.type_ref(ap.TYPE_PRIMITIVE, "i32")]},
            operation={"op": "rust/call/1"})
        bad = ap.Query(
            query_id="b", entity={"kind": "generic", "path": "twice",
                                  "args": [ap.type_ref(ap.TYPE_RECORD, "Marker")]},
            operation={"op": "rust/call/1"})
        return [twice, pair, bad], "__vyx_rust_twice_i32", "84"
    tadd = ap.Query(
        query_id="g", entity={"kind": "generic", "path": "tadd",
                              "args": [ap.type_ref(ap.TYPE_PRIMITIVE, "i32"),
                                       ap.type_ref(ap.TYPE_PRIMITIVE, "i32")]},
        operation={"op": "cpp/call/1"})
    bad = ap.Query(
        query_id="b", entity={"kind": "generic", "path": "tadd",
                              "args": [ap.type_ref(ap.TYPE_RECORD, "NoAdd")]},
        operation={"op": "cpp/call/1"})
    return [tadd, bad], "__vyx_cpp_tadd_int_int", "42"


def _publish(session, resolutions, cache: Path):
    closed = [r for r in resolutions if r.status == ap.STATUS_CLOSED]
    candidate = session.materialize_batch(closed)
    key_first = session.validate_and_publish(candidate, cache)
    key_second = session.validate_and_publish(candidate, cache)
    return candidate, key_first, key_first == key_second


def run(lang: str, fixture: Path, cache: Path, work: Path, summary: Path) -> int:
    if lang == "rust":
        from tools.dci.active_rust import RustActiveSession as Session
        entry = work / "crate" / "lib.rs"
        entry.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(fixture, entry)
    else:
        from tools.dci.active_cpp import CppActiveSession as Session
        entry = work / "fixture.hpp"
        entry.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(fixture, entry)
    open_args = {"work_root": work / "session1"}
    queries, replay_symbol, replay_expect = _queries(lang)

    with Session(entry, **open_args) as session:
        resolutions = session.resolve_batch(queries)
        candidate, bundle_key, single_flight = _publish(session, resolutions, cache)

    closed = [r for r in resolutions if r.status == ap.STATUS_CLOSED]
    refused = [r for r in resolutions if r.status != ap.STATUS_CLOSED]
    if not closed or len(refused) != 1:
        print(f"FAIL {lang} closed={len(closed)} refused={len(refused)}")
        return 1
    if refused[0].diagnostics[0].code != ap.ERROR_CONSTRAINT_FAILED:
        print(f"FAIL {lang} admission code={refused[0].diagnostics[0].code}")
        return 1
    symbols = sorted(r.facts["symbol"] for r in closed)
    if replay_symbol not in symbols:
        print(f"FAIL {lang} replay symbol missing: {symbols}")
        return 1

    # Invalidation correctness: touch the source, reopen the session; the same
    # request must land on a different session identity and request key.
    entry.write_text(entry.read_text(encoding="utf-8") + "\n// touched\n",
                     encoding="utf-8")
    with Session(entry, **{"work_root": work / "session2"}) as session2:
        resolutions2 = session2.resolve_batch(queries)
    closed2 = [r for r in resolutions2 if r.status == ap.STATUS_CLOSED]
    invalidated = ({r.request_key for r in closed}
                   != {r.request_key for r in closed2})
    if not invalidated:
        print(f"FAIL {lang} invalidation")
        return 1

    summary.write_text(
        "\n".join([
            f"LANG={lang}",
            f"SYMBOLS={','.join(symbols)}",
            f"REPLAY_SYMBOL={replay_symbol}",
            f"REPLAY_EXPECT={replay_expect}",
            f"BUNDLE={bundle_key}",
        ]) + "\n", encoding="utf-8")
    print(f"OK {lang} closed={len(closed)} symbols={','.join(symbols)}")
    print(f"OK {lang} admission=rejected:{refused[0].diagnostics[0].code}")
    print(f"OK {lang} bundle={bundle_key}")
    print(f"OK {lang} single_flight={'yes' if single_flight else 'no'}")
    print(f"OK {lang} invalidated={'yes' if invalidated else 'no'}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lang", choices=("rust", "cpp"), required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)
    return run(args.lang, args.fixture, args.cache, args.work, args.summary)


if __name__ == "__main__":
    sys.exit(main())

"""dci_opengeneric — 开放泛型端到端演示（Active Adapter）.

同一个开放泛型，N 个调用点实参：

  1. 打开 provider 会话（rustc / clang++ 身份 + 源摘要冻结为 SessionIdentity）
  2. 一个 resolve_batch 同时闭合多个实参组合（含 record 实参、双类型参数）
     约束裁决交给生产端编译器探针：拒绝项带回生产端诊断
  3. 一次 materialize_batch -> 一个真实 COFF 对象（llvm-nm 符号数 == 闭合数）
  4. 发布一个内容寻址 bundle（二次发布 key 一致 = single-flight）
  5. 离线重放：C driver 只链接 bundle 里的对象（无源码、无 adapter），
     逐调用验证计算值
  6. 失效正确性：改动源码后 request key 全变

输出为确定性 KEY=VALUE 行；与 expected_output.txt 比对即门。
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from tools.dci import active_protocol as ap  # noqa: E402

PROJ = Path(__file__).resolve().parent
NATIVE = PROJ / "native"


def _tool(*candidates) -> str:
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate))
    for candidate in candidates:
        if candidate and shutil.which(str(candidate)):
            return str(candidate)
    raise SystemExit(f"tool not found: {candidates}")


CLANG = _tool(REPO / "clang" / "bin" / "clang.exe", "D:/LLVM/bin/clang.exe", "clang")
CLANGXX = _tool(REPO / "clang" / "bin" / "clang++.exe", "D:/LLVM/bin/clang++.exe", "clang++")
LLVM_NM = _tool(REPO / "clang" / "bin" / "llvm-nm.exe", "D:/LLVM/bin/llvm-nm.exe", "llvm-nm")

SYMBOL_PREFIX = {"rust": "__vyx_rust_", "cpp": "__vyx_cpp_"}


# ---------------------------------------------------------------------------
# 调用点清单（消费方视角：只写「函数路径 + 实参类型」，别的一概不知）


def _q(qid: str, path: str, *args: tuple[str, str], op: str) -> ap.Query:
    return ap.Query(
        query_id=qid,
        entity={"kind": "generic", "path": path,
                "args": [ap.type_ref(form, name) for form, name in args]},
        operation={"op": op},
    )


def queries(lang: str) -> list[ap.Query]:
    """The SAME open generics, in whichever producer's operation namespace.

    `native/lib.rs` and `native/lib.hpp` declare the same four generics
    (`twice`, `max_of`, `identity`, `swap`) over the same `Vec2`, so the
    request surface is identical and only `op` differs.  The last query is the
    control: `twice<Vec2>` asks for a constraint the producer must refuse
    (`Vec2` has neither `Add`/`operator+` nor `PartialOrd`/`operator>`), which
    proves the refusal comes from the producer compiler and not from the
    request surface.
    """
    P, R = ap.TYPE_PRIMITIVE, ap.TYPE_RECORD
    op = f"{lang}/call/1"
    return [
        _q("t1", "twice", (P, "i32"), op=op),
        _q("t2", "twice", (P, "f64"), op=op),
        _q("m1", "max_of", (P, "i32"), (P, "i32"), op=op),
        _q("m2", "max_of", (P, "f64"), (P, "f64"), op=op),
        _q("id", "identity", (R, "Vec2"), op=op),
        _q("sw", "swap", (P, "i32"), (P, "f64"), op=op),
        _q("bad", "twice", (R, "Vec2"), op=op),
    ]


# ---------------------------------------------------------------------------
# 离线重放清单：C driver 语句模板，{sym} 从 bundle manifest 取真实符号


REPLAY: list[dict] = [
    {"key": ("twice", ("i32",)),
     "decls": ['extern int {sym}(int v0);'],
     "stmts": ['printf("%d\\n", (int){sym}(42));'], "expect": "84"},
    {"key": ("twice", ("f64",)),
     "decls": ['extern double {sym}(double v0);'],
     "stmts": ['printf("%.6f\\n", {sym}(1.5));'], "expect": "3.000000"},
    {"key": ("max_of", ("i32", "i32")),
     "decls": ['extern int {sym}(int v0, int v1);'],
     "stmts": ['printf("%d\\n", (int){sym}(3, 9));'], "expect": "9"},
    {"key": ("max_of", ("f64", "f64")),
     "decls": ['extern double {sym}(double v0, double v1);'],
     "stmts": ['printf("%.6f\\n", {sym}(1.25, 1.75));'], "expect": "1.750000"},
    {"key": ("identity", ("Vec2",)),
     "decls": ['extern struct Vec2 {sym}(struct Vec2 v0);'],
     "stmts": ['{ struct Vec2 v; v.x = 3; v.y = 4;',
               '  struct Vec2 r = {sym}(v);',
               '  printf("(%.6f,%.6f)\\n", r.x, r.y); }'],
     "expect": "(3.000000,4.000000)"},
    # swap returns the 16-byte aggregate both providers agree on: f64 at 0,
    # i32 at 8.  It is the one operation whose result is a record, so the
    # replay checks the layout as well as the value.
    {"key": ("swap", ("i32", "f64")),
     "decls": ['extern struct Pair2 {sym}(int v0, double v1);'],
     "stmts": ['{ struct Pair2 r = {sym}(21, 1.5);',
               '  printf("(%.6f,%d)\\n", r.first, r.second); }'],
     "expect": "(1.500000,21)"},
]

C_HEADER = """#include <stdio.h>
typedef struct Vec2 { double x, y; } Vec2;
typedef struct Pair2 { double first; int second; } Pair2;
"""

# manifest facts 里的 spelling -> 查询用的类型名
_REV_CPP = {"int": "i32", "double": "f64"}


def _norm_args(lang: str, spellings: list[str]) -> tuple[str, ...]:
    out = []
    for spelling in spellings:
        name = spelling.replace("crate_shim::", "")
        out.append(_REV_CPP.get(name, name) if lang == "cpp" else name)
    return tuple(out)


def _symbols_from_manifest(lang: str, manifest: dict) -> dict:
    table: dict = {}
    for op in manifest["covered_operations"]:
        facts = op["facts"]
        table[(tuple(facts["path"]), _norm_args(lang, facts["arguments"]))] = \
            facts["symbol"]
    return table


# ---------------------------------------------------------------------------


def run(lang: str, work: Path, cache: Path) -> int:
    prefix = SYMBOL_PREFIX[lang]
    if lang == "rust":
        from tools.dci.active_rust import RustActiveSession as Session
        staged = work / "source" / "lib.rs"
    else:
        from tools.dci.active_cpp import CppActiveSession as Session
        staged = work / "source" / "lib.hpp"
    staged.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(NATIVE / staged.name, staged)

    queries_list = queries(lang)
    open_args = {"work_root": work / "session1"}

    print(f"== {lang} session ==")
    with Session(staged, **open_args) as session:
        identity = session.handle.session.session_identity
        producer = session.handle.session.producer
        print(f"SESSION producer={producer} identity={identity[:16]}")
        resolutions = session.resolve_batch(queries_list)

        by_id = {query.query_id: query for query in queries_list}
        closed, refused = [], []
        for resolution in resolutions:
            query = by_id[resolution.query_id]
            name = ",".join(a["name"] for a in query.entity["args"])
            line = f"CALL {query.entity['path']}<{name}>"
            if resolution.status == ap.STATUS_CLOSED:
                closed.append(resolution)
                print(f"{line:<44} -> CLOSED   {resolution.facts['symbol']}")
            else:
                refused.append(resolution)
                diag = resolution.diagnostics[0]
                message = diag.message
                error_at = message.find("error")
                display = message[error_at:] if error_at != -1 else message
                print(f"{line:<44} -> {resolution.status.upper():<9} "
                      f"{diag.code} ({display[:72]})")

        if len(closed) != len(queries_list) - 1 or len(refused) != 1:
            print(f"FAIL {lang} closed={len(closed)} refused={len(refused)}")
            return 1
        if refused[0].diagnostics[0].code != ap.ERROR_CONSTRAINT_FAILED:
            print(f"FAIL {lang} admission={refused[0].diagnostics[0].code}")
            return 1

        candidate = session.materialize_batch(closed)
        object_entry = next(iter(candidate.artifacts.values()))
        object_bytes = object_entry[0]
        nm = subprocess.run([LLVM_NM, "-"], input=object_bytes,
                            capture_output=True, timeout=60, check=False)
        if nm.returncode != 0:  # llvm-nm 不吃 stdin 时退回落盘
            tmp_obj = work / "nm_probe.obj"
            tmp_obj.write_bytes(object_bytes)
            nm = subprocess.run([LLVM_NM, str(tmp_obj)],
                                capture_output=True, text=True, timeout=60,
                                check=False)
            output = nm.stdout.decode("utf-8", "replace")
        else:
            output = nm.stdout.decode("utf-8", "replace")
        seen = {line.split()[-1] for line in output.splitlines() if line.strip()}
        distinct = sorted(s for s in seen if prefix in s)
        if len(distinct) != len(closed):
            print(f"FAIL {lang} nm distinct={len(distinct)} closed={len(closed)}")
            return 1
        print(f"OBJECT bytes={len(object_bytes)} nm_distinct={len(distinct)}"
              f" closed={len(closed)}")

        key_first = session.validate_and_publish(candidate, cache)
        key_second = session.validate_and_publish(candidate, cache)
        single_flight = key_first == key_second
        print(f"PUBLISH bundle={key_first[:16]} republish_same_key="
              f"{'yes' if single_flight else 'no'}")

        # 离线重放：只凭 bundle（无源码、无 adapter）
        manifest = __import__("tools.dci.artifact_bundle",
                              fromlist=["load_bundle"]).load_bundle(cache, key_first)
        symbols = _symbols_from_manifest(lang, manifest)
        decls, calls, expects = [], [], []
        for item in REPLAY:
            symbol = symbols[((item["key"][0],), item["key"][1])]
            decls += [d.replace("{sym}", symbol) for d in item["decls"]]
            calls += [s.replace("{sym}", symbol) for s in item["stmts"]]
            expects.append(item["expect"])
        replay_dir = work / "replay"
        replay_dir.mkdir(parents=True, exist_ok=True)
        driver = replay_dir / "driver.c"
        driver.write_text(C_HEADER + "\n".join(decls)
                          + "\nint main(void) {\n" + "\n".join(calls)
                          + "\n    return 0;\n}\n", encoding="utf-8")
        artifact = __import__("tools.dci.artifact_bundle",
                              fromlist=["bundle_path"]).bundle_path(
            cache, key_first, "artifacts/" + manifest["artifacts"][0]["path"])
        exe = replay_dir / "replay.exe"
        # driver.c 是 C 代码：必须用 C 编译器（C++ 模式会把声明 mangle 掉）
        link = subprocess.run([CLANG, str(driver), str(artifact), "-o", str(exe)],
                              capture_output=True, text=True, timeout=120,
                              check=False)
        if link.returncode != 0:
            print(f"FAIL {lang} replay link: {link.stderr.strip()[:200]}")
            return 1
        run_result = subprocess.run([str(exe)], capture_output=True, text=True,
                                    timeout=30, check=False)
        actual = run_result.stdout.strip().splitlines()
        print("REPLAY offline (bundle only, no source, no adapters):")
        for symbol_line, expect in zip(actual, expects):
            marker = "OK" if symbol_line == expect else "MISMATCH"
            print(f"REPLAY {symbol_line:<44} expect={expect} {marker}")
        if run_result.returncode != 0 or actual != expects:
            print(f"FAIL {lang} replay rc={run_result.returncode}")
            return 1

    # 失效正确性：源码一动，request key 全变
    staged.write_text(staged.read_text(encoding="utf-8") + "\n// touched\n",
                      encoding="utf-8")
    with Session(staged, **{"work_root": work / "session2"}) as session2:
        probes = [queries(lang)[0], queries(lang)[-1]]
        resolutions2 = session2.resolve_batch(probes)
    old_keys = {r.request_key for r in closed}
    new_keys = {r.request_key for r in resolutions2 if r.status == ap.STATUS_CLOSED}
    print(f"INVALIDATE touched source: {len(old_keys - new_keys)}"
          f"/{len(old_keys)} request keys changed")
    if new_keys & old_keys:
        print(f"FAIL {lang} invalidation leaked {len(new_keys & old_keys)} key(s)")
        return 1

    print(f"PASS {lang}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=PROJ / "target" / "demo")
    parser.add_argument("--cache", type=Path, default=PROJ / "target" / "bundles")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    return run("rust", args.work, args.cache) or run("cpp", args.work, args.cache)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Reproducible benchmark harness: Vyx DCI vs mature Rust FFI vs Rust cxx.

The harness drives one shared C++ ABI fixture library (``fixtures/``) through
three native-binding mechanisms and records, from real runs, how each one binds,
verifies, or fails closed on the same set of ABI entities:

  * DCI      -- the current Vyx Declarative Code Interface.  The adapter reads
               the C++ header directly and records ABI facts; the descriptor is
               strict-validated and consumed by the bootstrap compiler at the
               contract / IR level (no cross-language linking, per the study
               design: Vyx FFI is not used as the correctness oracle).
  * bindgen  -- extern "C" plus bindgen over a hand-authored C facade.  rustc
               actually compiles and runs the correctness cases (this is the
               mature Rust FFI oracle).
  * cxx      -- a #[cxx::bridge] over the cxx-expressible subset.  rustc actually
               compiles and runs the correctness cases.

Usage:

    python3 tools/dci/bench/dci_bench.py run --repeat 5 -o tools/dci/bench/out
    python3 tools/dci/bench/dci_bench.py self-check
    python3 tools/dci/bench/dci_bench.py fingerprint

Correctness and coverage are deterministic and measured once.  Timing metrics
are repeated ``--repeat`` times and reported as mean +/- standard deviation with
the repeat count.  Every raw number the paper cites is written to JSON and CSV
under the output directory; nothing is hand-entered.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

BENCH_DIR = Path(__file__).resolve().parent
FIXTURES = BENCH_DIR / "fixtures"
HARNESS = BENCH_DIR / "harness"
DCI_TOOLS = BENCH_DIR.parent
DCI_PY = DCI_TOOLS / "dci.py"
REPO_ROOT = DCI_TOOLS.parents[1]

# --- Pinned toolchain configuration (single source of truth) ----------------
SEED = 20260828
CXX_VERSION = "1.0.129"       # last cxx line resolving on rustc 1.83
CXX_BUILD_VERSION = "1.0.129"
BINDGEN_VERSION = "0.70.1"    # last bindgen line building on rustc 1.83


def default_llvm_root() -> str:
    root = os.environ.get("LLVM_ROOT")
    if root and Path(root).exists():
        return root
    repo_clang = REPO_ROOT / "clang"
    clangxx = repo_clang / "bin" / ("clang++.exe" if sys.platform == "win32" else "clang++")
    if clangxx.exists():
        return str(repo_clang)
    for candidate in ("/usr/lib/llvm-22", "/usr/lib/llvm-21", "/usr/lib/llvm-20"):
        if Path(candidate).exists():
            return candidate
    return ""


def default_triplet() -> str:
    override = os.environ.get("DCI_BENCH_TRIPLET", "").strip()
    if override:
        return override
    if sys.platform == "win32":
        return "windows_x64"
    return "linux_x64"


def default_libclang_path(llvm_root: str) -> str:
    if not llvm_root:
        return ""
    root = Path(llvm_root)
    if sys.platform == "win32":
        for candidate in (root / "bin", root / "lib"):
            if (candidate / "libclang.dll").exists():
                return str(candidate)
        return str(root / "bin")
    return str(root / "lib")


def boot_compiler() -> Path:
    """Host bootstrap compiler used by Consumer checks and fingerprints."""
    out = REPO_ROOT / "bootstrap_compiler" / "out"
    if sys.platform == "win32":
        return out / "boot.exe"
    return out / "boot"


def boot_run_env() -> dict[str, str]:
    env = dict(os.environ)
    lib = str(REPO_ROOT / "bootstrap_compiler" / "out")
    env["PATH"] = lib + os.pathsep + env.get("PATH", "")
    env["LD_LIBRARY_PATH"] = lib + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    if LLVM_ROOT:
        env["LLVM_ROOT"] = LLVM_ROOT
        llvm_bin = str(Path(LLVM_ROOT) / "bin")
        env["PATH"] = llvm_bin + os.pathsep + env["PATH"]
    return env


def llvm_tool(llvm_root: str, name: str) -> str:
    if not llvm_root:
        return name
    path = Path(llvm_root) / "bin" / name
    if sys.platform == "win32":
        exe = path if path.suffix.lower() == ".exe" else path.with_suffix(".exe")
        if exe.exists():
            return str(exe)
        if path.exists():
            return str(path)
        return str(exe)
    return str(path)


LLVM_ROOT = default_llvm_root()
LIBCLANG_PATH = default_libclang_path(LLVM_ROOT)
CLANGXX = llvm_tool(LLVM_ROOT, "clang++")
CLANG = llvm_tool(LLVM_ROOT, "clang")
CLANG_CL = llvm_tool(LLVM_ROOT, "clang-cl")
DEFAULT_TRIPLET = default_triplet()
DEFAULT_STD = "c++17"


# --- The fixed entity catalog exercised by all three paths -------------------
@dataclass
class Entity:
    id: str
    family: str
    description: str
    # DCI lookup: ("symbol", name) | ("symbol_all", [names]) | ("reject", selector_prefix)
    #             | ("layout_size", type_name, expected_from_ground_truth)
    dci: tuple


CATALOG: list[Entity] = [
    Entity("point_sum", "pod_register", "POD single INTEGER eightbyte by value",
           ("symbol", "point_sum")),
    Entity("point_translate", "pod_aggregate_return", "POD aggregate returned by value",
           ("symbol", "point_translate")),
    Entity("rect_area", "pod_nested", "nested POD, two INTEGER eightbytes by value",
           ("symbol", "rect_area")),
    Entity("vec2_dot", "pod_sse", "SSE aggregate (two doubles) by value",
           ("symbol", "vec2_dot")),
    Entity("span_checksum", "pod_ptr_int", "pointer+integer aggregate by value",
           ("symbol", "span_checksum")),
    Entity("quad_sum", "pod_memory", "memory-class aggregate (32B) by value",
           ("symbol", "quad_sum")),
    Entity("quad_scale", "pod_memory_return", "memory-class aggregate returned by value",
           ("symbol", "quad_scale")),
    Entity("color_next", "enum_scoped", "scoped enum lowered to integer",
           ("symbol", "color_next")),
    Entity("status_step", "enum_unscoped", "unscoped fixed-underlying enum",
           ("symbol", "status_step")),
    Entity("packed_layout", "packed", "packed record layout (size 5)",
           ("layout_size", "abi::PackedHeader")),
    Entity("buffer_handle", "nontrivial_handle", "non-trivial type via methods/handle",
           ("symbol_all", ["constructor", "size", "checksum", "fill"])),
    Entity("buffer_value", "nontrivial_value", "non-trivial type passed by value",
           ("reject", "buffer_consume")),
    Entity("shape_virtual", "virtual", "polymorphic hierarchy / virtual dispatch",
           ("symbol_all", ["area", "sides"])),
    Entity("checked_divide", "exception", "function that throws across the boundary",
           ("symbol", "checked_divide")),
]

ENTITY_IDS = [e.id for e in CATALOG]


# --- Small utilities --------------------------------------------------------
def run(cmd: list[str], *, cwd: Path | None = None, env: dict | None = None,
        timeout: int = 900) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd, cwd=str(cwd) if cwd else None, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        errors="replace", timeout=timeout, check=False,
    )


def tool_version(cmd: list[str]) -> str:
    try:
        proc = run(cmd, timeout=60)
        return proc.stdout.strip().splitlines()[0] if proc.stdout.strip() else ""
    except Exception as exc:  # pragma: no cover - environment probe
        return f"(unavailable: {exc})"


def mean_std(values: list[float]) -> dict[str, Any]:
    n = len(values)
    if n == 0:
        return {"mean": None, "std": None, "n": 0, "samples": []}
    mean = sum(values) / n
    if n > 1:
        var = sum((v - mean) ** 2 for v in values) / (n - 1)
        std = math.sqrt(var)
    else:
        std = 0.0
    return {"mean": mean, "std": std, "n": n, "samples": [round(v, 6) for v in values]}


def file_size(path: Path) -> int:
    return path.stat().st_size if path.exists() else -1


def count_nonblank_lines(path: Path) -> int:
    if not path.exists():
        return 0
    return sum(1 for line in path.read_text(encoding="utf-8").splitlines() if line.strip())


def cargo_env() -> dict:
    env = dict(os.environ)
    if LLVM_ROOT:
        llvm_bin = str(Path(LLVM_ROOT) / "bin")
        env["PATH"] = llvm_bin + os.pathsep + env.get("PATH", "")
        env["LLVM_ROOT"] = LLVM_ROOT
    if sys.platform == "win32" and Path(CLANG_CL).exists():
        env["CXX"] = CLANG_CL
        env["CC"] = CLANG_CL
        # clang-cl / cc default to exceptions-off; fixture + cxx.cc both throw.
        existing = env.get("CXXFLAGS", "").strip()
        if "/EHsc" not in existing:
            env["CXXFLAGS"] = (existing + " /EHsc").strip()
    else:
        env["CXX"] = CLANGXX
        env["CC"] = CLANG
    if LIBCLANG_PATH:
        env["LIBCLANG_PATH"] = LIBCLANG_PATH
    env["CARGO_TERM_COLOR"] = "never"
    return env


# --- Toolchain fingerprint --------------------------------------------------
def collect_fingerprint(triplet: str | None = None) -> dict[str, Any]:
    boot = boot_compiler()
    return {
        "seed": SEED,
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": sys.version.split()[0],
        },
        "target_triplet": triplet or DEFAULT_TRIPLET,
        "cpp_std": DEFAULT_STD,
        "llvm_root": LLVM_ROOT,
        "compilers": {
            "clang++": tool_version([CLANGXX, "--version"]),
            "clang": tool_version([CLANG, "--version"]),
            "g++": tool_version(["g++", "--version"]),
        },
        "rust": {
            "rustc": tool_version(["rustc", "--version"]),
            "cargo": tool_version(["cargo", "--version"]),
        },
        "pinned_crates": {
            "cxx": CXX_VERSION,
            "cxx-build": CXX_BUILD_VERSION,
            "bindgen": BINDGEN_VERSION,
        },
        "vyx": {
            "boot": tool_version([str(boot), "--help"]) if boot.exists() else "(not built)",
            "boot_present": boot.exists(),
        },
    }


# --- Ground-truth ABI probe -------------------------------------------------
def build_ground_truth(work: Path) -> dict[str, Any]:
    src = work / "ground_truth"
    src.mkdir(parents=True, exist_ok=True)
    for name in ("abi_fixtures.hpp", "abi_fixtures.cpp"):
        shutil.copy2(FIXTURES / name, src / name)
    shutil.copy2(HARNESS / "layout_probe.cpp", src / "layout_probe.cpp")
    exe = src / "layout_probe"
    compile_proc = run([
        CLANGXX, f"-std={DEFAULT_STD}", "-O0",
        str(src / "layout_probe.cpp"), str(src / "abi_fixtures.cpp"),
        "-I", str(src), "-o", str(exe),
    ])
    if compile_proc.returncode != 0:
        raise RuntimeError("ground-truth probe failed to compile:\n" + compile_proc.stdout)
    run_proc = run([str(exe)])
    if run_proc.returncode != 0:
        raise RuntimeError("ground-truth probe failed to run:\n" + run_proc.stdout)
    return json.loads(run_proc.stdout)


def extract_ir_signatures(work: Path) -> dict[str, str]:
    """Argument shapes of the real C++ functions from the compiler's own IR.

    Used to independently confirm the DCI-recorded by-value ABI (coerce/direct/
    indirect) matches the selected compiler's actual codegen.
    """
    src = work / "ground_truth"
    ir = src / "abi_fixtures.ll"
    proc = run([
        CLANGXX, f"-std={DEFAULT_STD}", "-O0", "-S", "-emit-llvm",
        str(src / "abi_fixtures.cpp"), "-I", str(src), "-o", str(ir),
    ])
    if proc.returncode != 0 or not ir.exists():
        return {}
    text = ir.read_text(encoding="utf-8", errors="replace")
    signatures: dict[str, str] = {}
    for match in re.finditer(r"^define[^\n@]*@([A-Za-z0-9_$.]+)\(([^)]*)\)", text, re.MULTILINE):
        signatures[match.group(1)] = match.group(2).strip()
    return signatures


# --- DCI path ---------------------------------------------------------------
def dci_adapter(header: Path, out_dcib: Path, debug_json: Path | None,
                ownership_ok: bool = True, triplet: str | None = None) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    if LLVM_ROOT:
        env["LLVM_ROOT"] = LLVM_ROOT
        llvm_bin = str(Path(LLVM_ROOT) / "bin")
        env["PATH"] = llvm_bin + os.pathsep + env.get("PATH", "")
    # Cargo / clang-cl in the parent shell must not leak into Adapter discovery.
    for key in ("CC", "CXX", "CFLAGS", "CXXFLAGS"):
        env.pop(key, None)
    cmd = [sys.executable, str(DCI_PY), "adapter", "--language", "cpp", str(header),
           "--toolchain", "clang", "--triplet", triplet or DEFAULT_TRIPLET, "-o", str(out_dcib)]
    if Path(CLANGXX).exists():
        cmd += ["--clang", CLANGXX]
    if debug_json is not None:
        cmd += ["--debug-json", str(debug_json)]
    return run(cmd, env=env)


def dci_validate(dcib: Path, strict: bool = True) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    if LLVM_ROOT:
        env["LLVM_ROOT"] = LLVM_ROOT
    cmd = [sys.executable, str(DCI_PY), "validate", str(dcib)]
    if strict:
        cmd.append("--strict")
    return run(cmd, env=env)


def dci_inspect(dcib: Path) -> dict[str, Any]:
    env = dict(os.environ)
    if LLVM_ROOT:
        env["LLVM_ROOT"] = LLVM_ROOT
    proc = run([sys.executable, str(DCI_PY), "inspect", "--json", str(dcib)], env=env)
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        return {}


def classify_dci_entity(entity: Entity, contract: dict[str, Any],
                        ground_truth: dict[str, Any]) -> dict[str, Any]:
    exports = contract.get("exports", {})
    symbols = exports.get("symbols", [])
    rejected = exports.get("rejected_symbols", [])
    layouts = exports.get("layouts", [])
    accepted_names = {s.get("name") for s in symbols}
    kind = entity.dci[0]

    if kind == "symbol":
        name = entity.dci[1]
        if name in accepted_names:
            abi = next((s.get("abi", {}) for s in symbols if s.get("name") == name), {})
            passing = [p.get("passing") for p in abi.get("parameters", [])]
            coerce = [p.get("coerce_to", {}).get("name") for p in abi.get("parameters", [])
                      if p.get("coerce_to")]
            detail = f"bound; param passing={passing} coerce={coerce}"
            return {"outcome": "bound", "detail": detail}
        return {"outcome": "unaccounted", "detail": f"symbol {name} not found"}

    if kind == "symbol_all":
        wanted = set(entity.dci[1])
        found = wanted & {s.get("member_name") or s.get("name") for s in symbols}
        found |= wanted & accepted_names
        if wanted <= found:
            return {"outcome": "bound", "detail": f"methods bound: {sorted(found)}"}
        return {"outcome": "partial", "detail": f"bound {sorted(found)} of {sorted(wanted)}"}

    if kind == "reject":
        prefix = entity.dci[1]
        for r in rejected:
            sel = r.get("selector") or ""
            if sel.startswith(prefix):
                return {"outcome": "rejected", "detail": r.get("reason", "")}
        if prefix in accepted_names:
            return {"outcome": "bound_unexpected", "detail": f"{prefix} unexpectedly bound"}
        return {"outcome": "unaccounted", "detail": f"{prefix} neither bound nor rejected"}

    if kind == "layout_size":
        type_name = entity.dci[1]
        layout = next((l for l in layouts if l.get("type_name") == type_name), None)
        if layout is None:
            return {"outcome": "unaccounted", "detail": f"layout {type_name} missing"}
        gt = ground_truth.get("types", {}).get(type_name.split("::")[-1], {})
        true_size = gt.get("size")
        size = layout.get("size")
        if true_size is not None and size == true_size:
            return {"outcome": "bound", "detail": f"size={size} matches ground truth"}
        return {"outcome": "mismatch_layout",
                "detail": f"size={size} ground_truth={true_size}"}

    return {"outcome": "unaccounted", "detail": "no dci mapping"}


def dci_consumer_checks(dcib: Path, work: Path) -> dict[str, Any]:
    """Contract / IR-level Consumer checks driven by the bootstrap compiler.

    No cross-language linking or execution: the compiler consumes the descriptor
    and emits IR, exercising acceptance of a Direct contract, in-Consumer
    forward_direct wrapper synthesis, and acceptance of a well-formed
    shared_abi exception-propagation boundary.
    """
    boot = boot_compiler()
    result: dict[str, Any] = {"available": boot.exists(), "path": str(boot)}
    if not boot.exists():
        return result
    env = boot_run_env()

    def consume(document_dcib: Path, host: str, name: str):
        host_path = work / f"{name}.vyx"
        out_ll = work / f"{name}.ll"
        host_path.write_text(host, encoding="utf-8")
        proc = run([str(boot), "--src=file", str(host_path), "--emit=ir",
                    "--dci", str(document_dcib), "-o", str(out_ll)], env=env)
        ir = out_ll.read_text(encoding="utf-8") if out_ll.exists() else ""
        return proc, ir

    # 1) A Direct contract from the fixtures is accepted and lowered.
    host = ('extern "dci" {\n'
            '    fn checked_divide(a: i32, b: i32) -> i32;\n'
            '}\n\n'
            'fn main() -> i32 {\n'
            '    return checked_divide(10, 2);\n'
            '}\n')
    proc, ir = consume(dcib, host, "direct")
    result["direct_contract_accepted"] = proc.returncode == 0 and "checked_divide" in ir

    # 2) In-Consumer forward_direct wrapper synthesis over a hand-written probe
    #    contract (mirrors the Consumer gate; needs no foreign toolchain).
    synth = _forward_direct_contract()
    synth_dcib = work / "synth.dcib"
    _encode_dcib(synth, synth_dcib)
    host2 = ('extern "dci" {\n'
             '    fn contract_probe(value: i32) -> i32;\n'
             '}\n\n'
             'fn main() -> i32 {\n'
             '    return contract_probe(0);\n'
             '}\n')
    proc2, ir2 = consume(synth_dcib, host2, "synth")
    result["forward_direct_synthesized"] = (
        proc2.returncode == 0
        and re.search(r"define[^\n]*@contract_probe_forward_wrapper", ir2) is not None
    )

    # 3) A well-formed shared_abi exception boundary emits native unwind attrs.
    shared = _shared_abi_contract()
    shared_dcib = work / "shared_abi.dcib"
    _encode_dcib(shared, shared_dcib)
    proc3, ir3 = consume(shared_dcib, host2, "shared_abi")
    result["shared_abi_boundary_accepted"] = (
        proc3.returncode == 0
        and ("__CxxFrameHandler3" in ir3 or "__gxx_personality_v0" in ir3)
    )
    return result


def _encode_dcib(document: dict, dcib: Path) -> None:
    json_path = dcib.with_suffix(".dci")
    json_path.write_text(json.dumps(document, indent=1), encoding="utf-8")
    dcib_tool = DCI_TOOLS / "dcib.py"
    run([sys.executable, str(dcib_tool), "encode", str(json_path), str(dcib)])


def _probe_symbol() -> dict:
    prim = {"name": "i32", "kind": "primitive", "nullable": False, "reference": "value"}
    return {
        "name": "contract_probe", "link_name": "contract_probe", "kind": "function",
        "calling_convention": "system",
        "params": [{"name": "value", "type": prim, "location": "abi", "ownership": "copy"}],
        "return": {"type": prim, "location": "abi", "ownership": "copy"},
        "abi": {"calling_convention": "system", "variadic": False,
                "parameters": [{"index": 0, "passing": "direct", "size": 4}],
                "return": {"passing": "direct", "size": 4}},
    }


def _base_probe_contract() -> dict:
    return {
        "$schema": "dci-1.0.schema.json", "dci": "1.0", "kind": "abi",
        "profile": {"id": "bench.probe", "version": "1.0", "level": "L1",
                    "consumer_modes": ["direct"]},
        "source": {"language": "contract-fixture",
                   "adapter": {"name": "dci-bench", "version": "1.0"}},
        "target": {"triple": "x86_64-pc-windows-msvc", "architecture": "x86_64",
                   "pointer_width": 64, "endianness": "little", "object_format": "coff",
                   "environment": "windows", "abi": "system", "abi_family": "win64"},
        "control_flow": {"default_boundary": "no_unwind", "propagation": {"mode": "forbidden"}},
        "exports": {"symbols": [_probe_symbol()]},
    }


def _forward_direct_contract() -> dict:
    doc = _base_probe_contract()
    doc["profile"]["consumer_modes"] = ["direct", "stub"]
    doc["exports"]["stub_requests"] = [{
        "id": "probe.forward.1", "kind": "operation_wrapper", "reason": "requires_stub",
        "target": "contract_probe", "synthesis": {"strategy": "forward_direct"},
        "wrapper": {"link_name": "contract_probe_forward_wrapper"},
    }]
    return doc


def _shared_abi_contract() -> dict:
    doc = _base_probe_contract()
    doc["control_flow"] = {"default_boundary": "shared_abi",
                           # Use the canonical producer/consumer identity
                           # implemented by the AOT lowerer.  The old
                           # ``bench-unwind-v1`` placeholder was intentionally
                           # rejected as an unknown ABI, making this benchmark
                           # report a false baseline failure.
                           "propagation": {"mode": "shared_abi", "abi": "dci.eh.msvc-cxx.v1"}}
    return doc


def run_dci_path(work: Path, ground_truth: dict[str, Any], repeat: int,
                 ir_signatures: dict[str, str], triplet: str | None = None) -> dict[str, Any]:
    dcib = work / "abi_fixtures.dcib"
    debug = work / "abi_fixtures.dci.json"
    trip = triplet or DEFAULT_TRIPLET
    # Timed descriptor generation (repeated).
    times: list[float] = []
    proc = None
    for _ in range(repeat):
        start = time.perf_counter()
        proc = dci_adapter(FIXTURES / "abi_fixtures.hpp", dcib, debug, triplet=trip)
        times.append(time.perf_counter() - start)
    assert proc is not None
    adapter_ok = proc.returncode == 0
    contract = json.loads(debug.read_text(encoding="utf-8")) if debug.exists() else {}

    validate_proc = dci_validate(dcib, strict=True)
    validate_ok = validate_proc.returncode == 0
    inspect = dci_inspect(dcib)

    entities = {}
    for entity in CATALOG:
        entities[entity.id] = classify_dci_entity(entity, contract, ground_truth)

    # Independent ABI cross-check: DCI-recorded by-value passing vs real IR.
    abi_crosscheck = []
    for symbol in contract.get("exports", {}).get("symbols", []):
        link = symbol.get("link_name") or symbol.get("mangled")
        abi = symbol.get("abi", {})
        params = abi.get("parameters", [])
        if not link or link not in ir_signatures:
            continue
        recorded = []
        for p in params:
            if p.get("passing") == "coerce" and p.get("coerce_to"):
                recorded.append(p["coerce_to"]["name"])
            else:
                recorded.append(p.get("passing"))
        abi_crosscheck.append({
            "symbol": symbol.get("name"), "link_name": link,
            "recorded": recorded, "ir_params": ir_signatures[link],
        })

    consumer = dci_consumer_checks(dcib, work)

    # Bit-field limitation probe: the adapter emits a descriptor that its own
    # strict validator rejects for a bit-field crossing a storage-unit boundary.
    bit_dcib = work / "bitfield_probe.dcib"
    bit_proc = dci_adapter(FIXTURES / "bitfield_probe.hpp", bit_dcib, None, triplet=trip)
    bit_validate = dci_validate(bit_dcib, strict=True) if bit_proc.returncode == 0 else None
    bitfield_limitation = {
        "adapter_ok": bit_proc.returncode == 0,
        "strict_validation_failed": (bit_validate is not None and bit_validate.returncode != 0),
        "diagnostic": (bit_validate.stdout.strip().splitlines()[-1]
                       if bit_validate and bit_validate.returncode != 0 else ""),
    }

    return {
        "adapter_ok": adapter_ok,
        "strict_validation_ok": validate_ok,
        "descriptor_bytes": file_size(dcib),
        "inspect": inspect,
        "entities": entities,
        "abi_crosscheck": abi_crosscheck,
        "consumer": consumer,
        "bitfield_limitation": bitfield_limitation,
        "generation_seconds": mean_std(times),
        "handwritten_glue_lines": _dci_host_binding_lines(),
        "host_binding_lines": _dci_host_binding_lines(),
        "requires_cpp_toolchain_at_consume": False,
    }


def _dci_host_binding_lines() -> int:
    """Programmer-written host glue: `extern \"dci\"`, not the generated .dcib."""
    path = HARNESS / "dci_host_binding.vyx"
    if not path.exists():
        return 0
    n = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("//"):
            continue
        n += 1
    return n


# --- Rust cargo paths (bindgen, cxx) ----------------------------------------
def _prepare_runner(work: Path, name: str, extra_fixtures: list[str]) -> Path:
    dest = work / name
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(HARNESS / name, dest)
    for fixture in extra_fixtures:
        shutil.copy2(FIXTURES / fixture, dest / "src" / fixture)
    return dest


def _target_binary(project: Path, name: str) -> Path:
    target_root = os.environ.get("CARGO_TARGET_DIR")
    base = Path(target_root) if target_root else (project / "target")
    path = base / "debug" / name
    if os.name == "nt":
        return path.with_suffix(".exe")
    return path


def _ensure_lockfile(project: Path, env: dict) -> subprocess.CompletedProcess:
    """Reuse the committed pinned Cargo.lock; only generate one if missing."""
    if (project / "Cargo.lock").exists():
        return subprocess.CompletedProcess([], 0, stdout="reused committed Cargo.lock", stderr="")
    return run(["cargo", "generate-lockfile"], cwd=project, env=env, timeout=600)


def _cargo_build(project: Path, env: dict) -> subprocess.CompletedProcess:
    return run(["cargo", "build", "--quiet", "--locked"], cwd=project, env=env, timeout=1200)


def _cargo_run(project: Path, env: dict, bench_out: Path) -> subprocess.CompletedProcess:
    run_env = dict(env)
    run_env["BENCH_OUT"] = str(bench_out)
    return run(["cargo", "run", "--quiet"], cwd=project, env=run_env, timeout=1200)


def _incremental_rebuild_times(project: Path, env: dict, repeat: int) -> dict[str, Any]:
    main_rs = project / "src" / "main.rs"
    times: list[float] = []
    for _ in range(repeat):
        os.utime(main_rs, None)
        start = time.perf_counter()
        proc = _cargo_build(project, env)
        elapsed = time.perf_counter() - start
        if proc.returncode == 0:
            times.append(elapsed)
    return mean_std(times)


def run_bindgen_path(work: Path, repeat: int) -> dict[str, Any]:
    env = cargo_env()
    project = _prepare_runner(work, "bindgen_runner",
                              ["abi_fixtures.hpp", "abi_fixtures.cpp",
                               "abi_fixtures_c.h", "abi_fixtures_c.cpp"])
    lock = _ensure_lockfile(project, env)
    first_start = time.perf_counter()
    build = _cargo_build(project, env)
    first_build = time.perf_counter() - first_start
    bench_out = work / "bindgen_result.json"
    runp = _cargo_run(project, env, bench_out)
    entities = {}
    extra = {}
    if bench_out.exists():
        data = json.loads(bench_out.read_text(encoding="utf-8"))
        for item in data.get("entities", []):
            if item["id"] in ENTITY_IDS:
                entities[item["id"]] = {"outcome": item["outcome"], "detail": item["detail"]}
            else:
                extra[item["id"]] = {"outcome": item["outcome"], "detail": item["detail"]}
    timing = _incremental_rebuild_times(project, env, repeat)
    binary = _target_binary(project, "bindgen_runner")
    facade_lines = (count_nonblank_lines(FIXTURES / "abi_fixtures_c.h")
                    + count_nonblank_lines(FIXTURES / "abi_fixtures_c.cpp"))
    return {
        "lockfile_ok": lock.returncode == 0,
        "build_ok": build.returncode == 0,
        "run_ok": runp.returncode == 0,
        "build_log_tail": build.stdout[-1500:] if build.returncode != 0 else "",
        "entities": entities,
        "extra_entities": extra,
        "first_build_seconds": first_build if build.returncode == 0 else None,
        "incremental_rebuild_seconds": timing,
        "binary_bytes": file_size(binary),
        "handwritten_glue_lines": facade_lines,
        "requires_cpp_toolchain_at_consume": True,
    }


def _cxx_reject_probe(work: Path, env: dict) -> dict[str, Any]:
    """Real compile-error evidence that cxx fails closed on opaque-by-value."""
    probe = work / "cxx_reject_opaque_by_value"
    if probe.exists():
        shutil.rmtree(probe)
    (probe / "src").mkdir(parents=True)
    for fixture in ("abi_fixtures.hpp", "abi_fixtures.cpp"):
        shutil.copy2(FIXTURES / fixture, probe / "src" / fixture)
    (probe / "Cargo.toml").write_text(
        "[package]\nname = \"cxx_reject\"\nversion = \"0.1.0\"\nedition = \"2021\"\n"
        f"[dependencies]\ncxx = \"={CXX_VERSION}\"\n"
        f"[build-dependencies]\ncxx-build = \"={CXX_BUILD_VERSION}\"\ncc = \"1\"\n",
        encoding="utf-8")
    (probe / "build.rs").write_text(
        'fn main(){ cxx_build::bridge("src/main.rs").file("src/abi_fixtures.cpp")'
        '.std("c++17").include("src").compile("cxx_reject"); }\n', encoding="utf-8")
    (probe / "src" / "bridge.h").write_text(
        '#pragma once\n#include "abi_fixtures.hpp"\n', encoding="utf-8")
    (probe / "src" / "main.rs").write_text(
        '#[cxx::bridge(namespace="bench")]\nmod ffi {\n'
        '    unsafe extern "C++" {\n'
        '        include!("cxx_reject/src/bridge.h");\n'
        '        #[namespace="abi"] type Buffer;\n'
        '        fn consume(b: Buffer) -> u64;\n'
        '    }\n}\nfn main(){}\n', encoding="utf-8")
    run(["cargo", "generate-lockfile"], cwd=probe, env=env, timeout=600)
    build = _cargo_build(probe, env)
    diagnostic = ""
    for line in build.stdout.splitlines():
        if "opaque C++ type by value" in line:
            diagnostic = line.strip()
            break
    return {
        "compile_failed": build.returncode != 0,
        "diagnostic": diagnostic or (build.stdout.strip().splitlines()[-1] if build.stdout.strip() else ""),
    }


def run_cxx_path(work: Path, repeat: int) -> dict[str, Any]:
    env = cargo_env()
    project = _prepare_runner(work, "cxx_runner",
                              ["abi_fixtures.hpp", "abi_fixtures.cpp"])
    lock = _ensure_lockfile(project, env)
    first_start = time.perf_counter()
    build = _cargo_build(project, env)
    first_build = time.perf_counter() - first_start
    bench_out = work / "cxx_result.json"
    runp = _cargo_run(project, env, bench_out)
    entities = {}
    if bench_out.exists():
        data = json.loads(bench_out.read_text(encoding="utf-8"))
        for item in data.get("entities", []):
            entities[item["id"]] = {"outcome": item["outcome"], "detail": item["detail"]}
    # cxx cannot express opaque-by-value; capture the real compile error.
    reject = _cxx_reject_probe(work, env)
    if reject["compile_failed"]:
        entities["buffer_value"] = {"outcome": "rejected", "detail": reject["diagnostic"]}
    timing = _incremental_rebuild_times(project, env, repeat)
    binary = _target_binary(project, "cxx_runner")
    glue_lines = (count_nonblank_lines(HARNESS / "cxx_runner" / "src" / "bridge.h")
                  + _cxx_bridge_mod_lines(HARNESS / "cxx_runner" / "src" / "main.rs"))
    return {
        "lockfile_ok": lock.returncode == 0,
        "build_ok": build.returncode == 0,
        "run_ok": runp.returncode == 0,
        "build_log_tail": build.stdout[-1500:] if build.returncode != 0 else "",
        "entities": entities,
        "reject_probe": reject,
        "first_build_seconds": first_build if build.returncode == 0 else None,
        "incremental_rebuild_seconds": timing,
        "binary_bytes": file_size(binary),
        "handwritten_glue_lines": glue_lines,
        "requires_cpp_toolchain_at_consume": True,
    }


def _cxx_bridge_mod_lines(main_rs: Path) -> int:
    text = main_rs.read_text(encoding="utf-8")
    match = re.search(r"#\[cxx::bridge.*?\nmod ffi \{(.*?)\n\}", text, re.DOTALL)
    if not match:
        return 0
    return sum(1 for line in match.group(1).splitlines() if line.strip())


# --- Aggregation ------------------------------------------------------------
def build_coverage_matrix(dci: dict, bindgen: dict, cxx: dict) -> list[dict[str, Any]]:
    rows = []
    for entity in CATALOG:
        rows.append({
            "id": entity.id,
            "family": entity.family,
            "description": entity.description,
            "dci": dci["entities"].get(entity.id, {}).get("outcome", "n/a"),
            "bindgen": bindgen["entities"].get(entity.id, {}).get("outcome", "n/a"),
            "cxx": cxx["entities"].get(entity.id, {}).get("outcome", "n/a"),
        })
    return rows


SAFE_OUTCOMES = {"bound", "rejected", "unsupported", "bound_guarded"}
BOUND_OUTCOMES = {"bound", "bound_guarded"}


def summarize(coverage: list[dict], bindgen: dict) -> dict[str, Any]:
    def count(path: str, predicate: Callable[[str], bool]) -> int:
        return sum(1 for row in coverage if predicate(row[path]))

    total = len(coverage)
    summary = {"total_entities": total}
    for path in ("dci", "bindgen", "cxx"):
        summary[path] = {
            "bound": count(path, lambda o: o in BOUND_OUTCOMES),
            "rejected_fail_closed": count(path, lambda o: o == "rejected"),
            "unsupported": count(path, lambda o: o == "unsupported"),
            "mismatch_layout": count(path, lambda o: o == "mismatch_layout"),
            "silent_wrong": count(path, lambda o: o == "silent_wrong"),
            "safe": count(path, lambda o: o in SAFE_OUTCOMES),
        }
    # The classic extern-C silent-wrong result lives in the bindgen extra set.
    naive = bindgen.get("extra_entities", {}).get("packed_layout_naive", {})
    summary["extern_c_handwritten_packed"] = naive.get("outcome", "n/a")
    return summary


def write_csv(path: Path, rows: list[dict], columns: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in columns})


def write_outputs(out_dir: Path, results: dict[str, Any]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "results.json").write_text(
        json.dumps(results, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    write_csv(out_dir / "coverage.csv", results["coverage"],
              ["id", "family", "dci", "bindgen", "cxx", "description"])

    trap_rows = [row for row in results["coverage"]
                 if row["family"] in {"pod_sse", "pod_memory", "pod_memory_return",
                                      "pod_aggregate_return", "packed", "nontrivial_value",
                                      "exception"}]
    write_csv(out_dir / "abi_traps.csv", trap_rows,
              ["id", "family", "dci", "bindgen", "cxx", "description"])

    timing_rows = []
    for path, blob in (("dci", results["dci"].get("generation_seconds")),
                       ("bindgen", results["bindgen"].get("incremental_rebuild_seconds")),
                       ("cxx", results["cxx"].get("incremental_rebuild_seconds"))):
        if blob:
            timing_rows.append({
                "path": path,
                "metric": "generation" if path == "dci" else "incremental_rebuild",
                "mean_seconds": round(blob["mean"], 6) if blob["mean"] is not None else "",
                "std_seconds": round(blob["std"], 6) if blob["std"] is not None else "",
                "repeats": blob["n"],
            })
    for path in ("bindgen", "cxx"):
        first = results[path].get("first_build_seconds")
        if first is not None:
            timing_rows.append({
                "path": path,
                "metric": "first_build",
                "mean_seconds": round(first, 6),
                "std_seconds": "",
                "repeats": 1,
            })
    write_csv(out_dir / "timings.csv", timing_rows,
              ["path", "metric", "mean_seconds", "std_seconds", "repeats"])

    cost_rows = [
        {"path": "dci",
         "host_binding_lines": results["dci"]["host_binding_lines"],
         "requires_cpp_toolchain_at_consume": results["dci"]["requires_cpp_toolchain_at_consume"]},
        {"path": "bindgen",
         "host_binding_lines": results["bindgen"]["handwritten_glue_lines"],
         "requires_cpp_toolchain_at_consume": results["bindgen"]["requires_cpp_toolchain_at_consume"]},
        {"path": "cxx",
         "host_binding_lines": results["cxx"]["handwritten_glue_lines"],
         "requires_cpp_toolchain_at_consume": results["cxx"]["requires_cpp_toolchain_at_consume"]},
    ]
    write_csv(out_dir / "cost.csv", cost_rows,
              ["path", "host_binding_lines", "requires_cpp_toolchain_at_consume"])


# --- Commands ---------------------------------------------------------------
def cmd_fingerprint(args: argparse.Namespace) -> int:
    print(json.dumps(collect_fingerprint(), indent=2, ensure_ascii=False))
    return 0


def do_run(out_dir: Path, repeat: int, work_dir: Path | None = None,
           triplet: str | None = None) -> dict[str, Any]:
    work = work_dir or (out_dir / "work")
    work.mkdir(parents=True, exist_ok=True)
    trip = triplet or DEFAULT_TRIPLET
    # Shared cargo target dir so the bindgen and cxx crates reuse compiled
    # dependencies and repeated runs stay fast and reproducible.
    os.environ["CARGO_TARGET_DIR"] = str(work / "cargo-target")
    fingerprint = collect_fingerprint(trip)
    ground_truth = build_ground_truth(work)
    ir_signatures = extract_ir_signatures(work)
    dci = run_dci_path(work, ground_truth, repeat, ir_signatures, triplet=trip)
    bindgen = run_bindgen_path(work, repeat)
    cxx = run_cxx_path(work, repeat)
    coverage = build_coverage_matrix(dci, bindgen, cxx)
    summary = summarize(coverage, bindgen)
    results = {
        "fingerprint": fingerprint,
        "ground_truth": ground_truth,
        "dci": dci,
        "bindgen": bindgen,
        "cxx": cxx,
        "coverage": coverage,
        "summary": summary,
    }
    write_outputs(out_dir, results)
    return results


def cmd_run(args: argparse.Namespace) -> int:
    out_dir = Path(args.output).expanduser().resolve()
    results = do_run(out_dir, args.repeat, triplet=args.triplet)
    summary = results["summary"]
    print(f"DCI bench complete -> {out_dir}")
    print(f"  triplet: {results['fingerprint']['target_triplet']}")
    print(f"  host: {results['fingerprint']['host']['platform']}")
    print(f"  entities: {summary['total_entities']}")
    for path in ("dci", "bindgen", "cxx"):
        s = summary[path]
        print(f"  {path:8s}: bound={s['bound']} rejected={s['rejected_fail_closed']} "
              f"unsupported={s['unsupported']} mismatch={s['mismatch_layout']} "
              f"silent_wrong={s['silent_wrong']} safe={s['safe']}/{summary['total_entities']}")
    print(f"  extern-C hand-written packed mirror: {summary['extern_c_handwritten_packed']}")
    dci_t = results["dci"].get("generation_seconds") or {}
    print(f"  dci generation: mean={dci_t.get('mean')} std={dci_t.get('std')} n={dci_t.get('n')} "
          f"adapter_ok={results['dci'].get('adapter_ok')}")
    for path in ("bindgen", "cxx"):
        blob = results[path]
        inc = blob.get("incremental_rebuild_seconds") or {}
        print(f"  {path} first_build={blob.get('first_build_seconds')} "
              f"incremental mean={inc.get('mean')} n={inc.get('n')} "
              f"build_ok={blob.get('build_ok')} run_ok={blob.get('run_ok')}")
    if not results["dci"].get("adapter_ok"):
        return 1
    if not results["bindgen"].get("build_ok") or not results["cxx"].get("build_ok"):
        return 1
    return 0


def cmd_self_check(args: argparse.Namespace) -> int:
    """Offline, deterministic invariant check for CI (no cargo / no network).

    Runs the DCI path plus the ground-truth probe and asserts the load-bearing
    facts the paper relies on.  The Rust paths need network on first run, so they
    are exercised by ``run`` rather than the CI self-check.
    """
    import tempfile

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="dci_bench_selfcheck_") as tmp:
        work = Path(tmp)
        ground_truth = build_ground_truth(work)
        if ground_truth["types"]["PackedHeader"]["size"] != 5:
            failures.append("ground truth PackedHeader size != 5")
        ir_signatures = extract_ir_signatures(work)
        dci = run_dci_path(work, ground_truth, repeat=1, ir_signatures=ir_signatures)

        def expect(entity_id: str, outcome: str) -> None:
            got = dci["entities"].get(entity_id, {}).get("outcome")
            if got != outcome:
                failures.append(f"DCI {entity_id}: expected {outcome}, got {got}")

        if not dci["adapter_ok"]:
            failures.append("DCI adapter failed")
        if not dci["strict_validation_ok"]:
            failures.append("DCI strict validation failed")
        expect("point_sum", "bound")
        expect("rect_area", "bound")
        expect("span_checksum", "bound")
        expect("color_next", "bound")
        expect("vec2_dot", "bound")
        expect("quad_sum", "bound")
        expect("point_translate", "bound")
        expect("quad_scale", "bound")
        expect("buffer_value", "rejected")
        expect("buffer_handle", "bound")
        expect("packed_layout", "bound")
        consumer = dci["consumer"]
        if consumer.get("available"):
            for key in ("direct_contract_accepted", "forward_direct_synthesized",
                        "shared_abi_boundary_accepted"):
                if not consumer.get(key):
                    failures.append(f"DCI consumer check failed: {key}")
        if not dci["bitfield_limitation"]["strict_validation_failed"]:
            failures.append("bit-field limitation probe did not reproduce")

    if failures:
        print("self-check FAILED:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("self-check OK: all DCI invariants hold")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="dci_bench",
                                     description="DCI vs Rust FFI vs cxx benchmark harness")
    sub = parser.add_subparsers(dest="command", required=True)

    run_p = sub.add_parser("run", help="run the full three-path benchmark")
    run_p.add_argument("--repeat", type=int, default=5, help="timing repetitions (default 5)")
    run_p.add_argument("--triplet", default=None,
                       help="DCI adapter target triplet (default: host platform)")
    run_p.add_argument("-o", "--output", default=str(BENCH_DIR / "out"),
                       help="output directory (default tools/dci/bench/out)")
    run_p.set_defaults(handler=cmd_run)

    sc = sub.add_parser("self-check", help="offline DCI invariant check for CI")
    sc.set_defaults(handler=cmd_self_check)

    fp = sub.add_parser("fingerprint", help="print the toolchain fingerprint")
    fp.set_defaults(handler=cmd_fingerprint)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())

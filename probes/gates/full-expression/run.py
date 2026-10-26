#!/usr/bin/env python3
"""AOT full-expression owners, branch evaluation, moves and borrowed views."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    output = gate / ".cache"
    output.mkdir(exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    records = []

    def invoke(name, command, expected_exit=0, stdout=None, diagnostic=""):
        try:
            result = subprocess.run([str(x) for x in command], cwd=repo, env=env,
                                    capture_output=True, timeout=90)
            normalized = result.stdout.replace(b"\r\n", b"\n")
            diagnostics = (result.stdout + result.stderr).decode(errors="replace")
            passed = result.returncode == expected_exit
            if stdout is not None:
                passed = passed and normalized == stdout
            if diagnostic:
                required = (diagnostic,) if isinstance(diagnostic, str) else diagnostic
                passed = passed and all(fragment in diagnostics for fragment in required)
            record = {"name": name, "command": [str(x) for x in command],
                      "exit": result.returncode, "stdout_hex": result.stdout.hex(),
                      "stderr": result.stderr.decode(errors="replace"), "pass": passed}
        except subprocess.TimeoutExpired:
            passed = False
            record = {"name": name, "pass": False, "timeout": True}
        records.append(record)
        print(name, "PASS" if passed else "FAIL", flush=True)
        if not passed:
            preview = dict(record)
            if "stdout_hex" in preview:
                preview["stdout_bytes"] = len(preview["stdout_hex"]) // 2
                preview["stdout_hex"] = preview["stdout_hex"][:1024]
            if "stderr" in preview:
                preview["stderr_bytes"] = len(preview["stderr"].encode())
                preview["stderr"] = preview["stderr"][:2048]
            print(json.dumps(preview, indent=2), flush=True)
        return passed

    heap_line = b"0123456789012345678901234567890123\n"
    branch_line = b"01234567890123456789012345678901\n"
    interfaces = output / "interfaces"
    (interfaces / "support").mkdir(parents=True, exist_ok=True)
    invoke("generic-scope-interface", [compiler, "--src=file", gate / "support/scoped_library.vyx",
           "--emit=vyi", "--vyi-shallow", "-o", interfaces / "support/scoped_library.vyi"])
    expectations = {"owners": b"owners=ok\n", "strings": heap_line + b"short\n" + heap_line * 2,
                    "borrowed_view": heap_line * 2, "generic_owners": b"generic-owners=ok\n",
                    "task_owners": b"mir_storage_lifetime OK\n",
                    "branch_views": branch_line + b"short\n",
                    "conditional_owners": b"conditional-owners=ok\n",
                    "stdlib_owners": b"",
                    "borrowed_payloads": b"borrowed-payloads=ok\n",
                    "clone_reference_values": b"clone-reference-values=ok\n",
                    "generic_forwarding": b"generic-forwarding=ok\n",
                    "vec_owning_mutations": b"vec-owning-mutations=ok\n",
                    "string_drop_identity": b"string-drop-identity=ok\n",
                    "generic_call_scope": b"generic-call-scope=ok\n",
                    "string_control_flow": b"string-control-flow=ok\n",
                    "lookalike_string_calls": b"borrowed class field must remain valid until its owner is dropped\n" * 9
                        + b"lookalike-string-calls=ok\n"}
    for name, expected in expectations.items():
        for optimization in (0, 2):
            executable = output / (f"{name}-O{optimization}" + (".exe" if os.name == "nt" else ""))
            executable.unlink(missing_ok=True)
            shared_sources = {"task_owners": "mir_storage_lifetime.vyx",
                              "stdlib_owners": "_clone_semantics.vyx"}
            source = (repo / "tests/cases" / shared_sources[name]
                      if name in shared_sources else gate / f"{name}.vyx")
            command = [compiler, "--src=file", source, "--emit=exe",
                       f"-O{optimization}", "-o", executable]
            if name == "generic_call_scope":
                command += ["--interface-root", interfaces]
            if invoke(f"{name}-O{optimization}-compile", command):
                invoke(f"{name}-O{optimization}-run", [executable], stdout=expected)

    # Output alone cannot demonstrate String buffer reclamation. Check the
    # unoptimized direct-expression program has a reachable String drop.
    ir = output / "strings-O0.ll"
    ir.unlink(missing_ok=True)
    if invoke("strings-ir-compile", [compiler, "--src=file", gate / "strings.vyx",
                                     "--emit=ir", "-O0", "-o", ir]):
        text = ir.read_text(encoding="utf-8")
        main = re.search(r"^define[^\n]*@main\([^\n]*\)[^\n]*\{\n(.*?)^\}", text, re.M | re.S)
        drop_calls = re.findall(r"\b(?:call|invoke)\b[^\n]*@([^ (]*_N_drop_[^ (]*)\(",
                                main.group(0) if main else "")
        passed = len(drop_calls) >= 5
        records.append({"name": "strings-ir-cleanup", "pass": passed, "drop_calls": drop_calls})
        print("strings-ir-cleanup", "PASS" if passed else "FAIL", flush=True)

    # Observe the stored allocations, not only the rendered string contents.
    # Runtime/native frees keep their normal ABI; only IR frees are redirected
    # to a counting harness that delegates to the same allocator.
    clang = repo / "clang/bin/clang++.exe" if os.name == "nt" else shutil.which("clang++")
    runtime = compiler.parent
    if not (runtime / ("vyx_runtime.lib" if os.name == "nt" else "libvyx_runtime.a")).exists():
        runtime = compiler.parent.parent / "lib"
    for fixture in ("stored_strings", "local_string_results"):
        for optimization in (0, 2):
            label = f"{fixture}-O{optimization}"
            ir = output / f"{label}.ll"
            tracked = output / f"{label}.tracked.ll"
            executable = output / (label + (".exe" if os.name == "nt" else ""))
            executable.unlink(missing_ok=True)
            if not invoke(label + "-compile", [compiler, "--src=file", gate / f"{fixture}.vyx",
                                               "--emit=ir", f"-O{optimization}", "-o", ir]):
                continue
            text = ir.read_text(encoding="utf-8")
            if fixture == "stored_strings":
                text = re.sub(r"@(vyx_runtime_free|free)(?=[(\s])", "@tracked_free", text)
            else:
                hooks = ("vyx_string_alloc_abi", "vyx_string_clone_len_abi", "vyx_runtime_malloc",
                         "vyx_string_to_upper_len_abi", "vyx_string_to_lower_len_abi",
                         "vyx_string_replace", "vyx_string_trim", "vyx_runtime_free", "vyx_free_pooled_string",
                         "vyx_free_runtime_string")
                text = re.sub(r"@(" + "|".join(hooks) + r")(?=[(\s])", r"@track_\1", text)
            tracked.write_text(text, encoding="utf-8")
            if not clang or not Path(clang).exists():
                records.append({"name": label + "-link", "pass": False, "error": "clang++ unavailable"})
                continue
            command = [clang, tracked, gate / f"{fixture}.cpp", f"-L{runtime}", "-lvyx_runtime",
                       "-ltbb12", "-fuse-ld=lld", "-o", executable]
            if os.name == "nt":
                command.extend(["-lsynchronization", "-lws2_32", "-fms-runtime-lib=static"])
            if invoke(label + "-link", command):
                expected = b"stored-strings=ok\n" if fixture == "stored_strings" else b"local-string-results=ok\n"
                invoke(label + "-run", [executable], stdout=expected)

    for name in ("returned_view", "returned_bound_view", "returned_payload", "returned_string_slices"):
        diagnostic = "returned view borrows an owning temporary"
        if name == "returned_string_slices":
            diagnostic = (diagnostic, "return owner.substring(", "return owner.trim()")
        rejected = output / (name + (".exe" if os.name == "nt" else ""))
        rejected.unlink(missing_ok=True)
        invoke(f"{name}-rejected", [compiler, "--src=file", gate / f"{name}.vyx",
                                    "--emit=exe", "-o", rejected], expected_exit=1,
               diagnostic=diagnostic)
        records.append({"name": f"{name}-no-artifact", "pass": not rejected.exists()})
        invoke(f"{name}-mir-rejected", [compiler, "--src=file", gate / f"{name}.vyx",
                                       "--dump-mir2"], expected_exit=1,
               diagnostic=diagnostic)
    rejected = output / ("unclonable_payload" + (".exe" if os.name == "nt" else ""))
    rejected.unlink(missing_ok=True)
    invoke("unclonable_payload-rejected", [compiler, "--src=file", gate / "unclonable_payload.vyx",
                                           "--emit=exe", "-o", rejected], expected_exit=1,
           diagnostic="E1100: trait bound not satisfied")
    records.append({"name": "unclonable_payload-no-artifact", "pass": not rejected.exists()})
    rejected = output / ("generic_call_no_overload" + (".exe" if os.name == "nt" else ""))
    rejected.unlink(missing_ok=True)
    invoke("generic-call-no-overload", [compiler, "--src=file", gate / "generic_call_no_overload.vyx",
           "--interface-root", interfaces, "--emit=exe", "-o", rejected], expected_exit=1,
           diagnostic="error: E2200:")
    records.append({"name": "generic-call-no-overload-no-artifact", "pass": not rejected.exists()})
    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "records": records, "passed": sum(r["pass"] for r in records), "checks": len(records)}
    (output / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f'{report["passed"]}/{report["checks"]} PASS', flush=True)
    return 0 if all(r["pass"] for r in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())

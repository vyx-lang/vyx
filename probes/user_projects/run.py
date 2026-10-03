#!/usr/bin/env python3
"""Overfit probe runner.

Compiles each probe under probes/user_projects/ with a *given* compiler binary
and records what happened: compile exit, run exit, stdout, and a one-line MIR
note. It rebuilds nothing large; it only drives the compiler you point it at.

Usage:
  run.py --cc SDK=/usr/local/bin/vyxc --cc S1=/workspace/bootstrap_compiler/out/boot \
         [--ldpath /workspace/bootstrap_compiler/out] [--json results.json]

Each --cc is LABEL=PATH. The same LD_LIBRARY_PATH (if any) is used for every
compiler; the self-host boot needs it to find libvyx_compiler_backend.so.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LLVM_ROOT = os.environ.get("LLVM_ROOT", "/usr/lib/llvm-22")

COMPILE_TIMEOUT = 120
RUN_TIMEOUT = 30

# Probe catalog. kind: "file" (single source), "project" (Vyx.toml dir),
# "opt" (compile at O0 and O2), "error" (expected to fail compilation).
PROBES = [
    {"id": "p1_generic_infer", "kind": "file",
     "src": "p1_generic_infer/generic_infer.vyx", "expect_run": 0,
     "expect_out": "generic_infer ok"},
    {"id": "p1_generic_infer_nested", "kind": "file",
     "src": "p1_generic_infer/generic_infer_nested.vyx", "expect_run": 0,
     "expect_out": "generic_infer_nested ok"},
    {"id": "p2_class_value", "kind": "file",
     "src": "p2_class_value/class_value.vyx", "expect_run": 0,
     "expect_out": "class_value ok"},
    {"id": "p2_class_value_collection", "kind": "file",
     "src": "p2_class_value/class_value_collection.vyx", "expect_run": 0,
     "expect_out": "class_value_collection ok"},
    {"id": "p3_nested_writeback", "kind": "file",
     "src": "p3_nested_mut/nested_writeback.vyx", "expect_run": 0,
     "expect_out": "nested_writeback ok"},
    {"id": "p3_nested_direct", "kind": "file",
     "src": "p3_nested_mut/nested_direct.vyx", "expect_run": 0,
     "expect_out": "nested_direct ok"},
    {"id": "p4_chained", "kind": "file",
     "src": "p4_dispatch/chained.vyx", "expect_run": 0,
     "expect_out": "chained ok"},
    {"id": "p4_overload", "kind": "file",
     "src": "p4_dispatch/overload.vyx", "expect_run": 0,
     "expect_out": "overload ok"},
    {"id": "p5_control_flow", "kind": "file",
     "src": "p5_control_flow/control_flow.vyx", "expect_run": 0,
     "expect_out": "control_flow ok"},
    {"id": "p6_mini_app", "kind": "project",
     "dir": "p6_mini_app", "target": "mini_app", "expect_run": 0,
     "expect_out": "mini_app ok"},
    {"id": "p7_opt_mismatch", "kind": "opt",
     "src": "p7_opt_mismatch/opt_probe.vyx", "expect_run": 0,
     "expect_out": "result=4991"},
    {"id": "p8_bad_type", "kind": "error",
     "src": "p8_error_quality/bad_type.vyx"},
    {"id": "p8_unknown_call", "kind": "error",
     "src": "p8_error_quality/unknown_call.vyx"},
]


def make_env(ldpath):
    env = dict(os.environ)
    env["LLVM_ROOT"] = LLVM_ROOT
    if ldpath:
        prev = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = ldpath + (":" + prev if prev else "")
    return env


def run(cmd, env, timeout, cwd=None):
    try:
        p = subprocess.run(cmd, env=env, cwd=cwd, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode("utf-8", "replace") if e.stdout else ""
        return "TIMEOUT", out


def signal_note(code):
    if isinstance(code, int) and code < 0:
        return f"signal {-code}" + (" (SIGSEGV)" if code == -11 else "")
    return None


def mir_note(cc, src, env, extra=None):
    """One-line MIR summary via --verify-mir2 (last mir2-verify line)."""
    cmd = [cc, "--src=file", src, "--verify-mir2"]
    if extra:
        cmd += extra
    code, out = run(cmd, env, COMPILE_TIMEOUT)
    line = ""
    for ln in out.splitlines():
        if "[mir2-verify]" in ln:
            line = ln.strip()
    if not line:
        # surface first error line if MIR never built
        for ln in out.splitlines():
            if "error" in ln.lower():
                return "no-mir: " + ln.strip()[:120]
        return "no-mir"
    return line


def do_file(cc, probe, env, tmp):
    src = os.path.join(HERE, probe["src"])
    exe = os.path.join(tmp, probe["id"] + ".exe")
    row = {"kind": "file"}
    c_code, c_out = run([cc, "--src=file", src, "--emit=exe", "-o", exe],
                        env, COMPILE_TIMEOUT)
    row["compile_exit"] = c_code
    row["compile_ok"] = (c_code == 0 and os.path.exists(exe))
    if not row["compile_ok"]:
        row["compile_err"] = first_error(c_out)
        row["verdict"] = "COMPILE_FAIL"
        return row
    r_code, r_out = run([exe], make_env(None), RUN_TIMEOUT)
    row["run_exit"] = r_code
    row["stdout"] = r_out.strip()
    row["signal"] = signal_note(r_code)
    row["mir"] = mir_note(cc, src, env)
    row["verdict"] = verdict_run(probe, r_code, r_out)
    return row


def do_project(cc, probe, env, tmp):
    pdir = os.path.join(HERE, probe["dir"])
    row = {"kind": "project"}
    # clean prior target so we measure this compiler
    tgt = os.path.join(pdir, "target")
    c_code, c_out = run([cc, "--src=project", pdir], env, COMPILE_TIMEOUT)
    exe = os.path.join(tgt, probe["target"])
    row["compile_exit"] = c_code
    row["compile_ok"] = (c_code == 0 and os.path.exists(exe))
    if not row["compile_ok"]:
        row["compile_err"] = first_error(c_out)
        row["verdict"] = "COMPILE_FAIL"
        return row
    r_code, r_out = run([exe], make_env(None), RUN_TIMEOUT)
    row["run_exit"] = r_code
    row["stdout"] = r_out.strip()
    row["signal"] = signal_note(r_code)
    row["verdict"] = verdict_run(probe, r_code, r_out)
    return row


def do_opt(cc, probe, env, tmp):
    src = os.path.join(HERE, probe["src"])
    row = {"kind": "opt", "variants": {}}
    results = {}
    for label, flags in (("O0", ["--mir-opt", "0", "--llvm-opt", "0"]),
                         ("O2", ["--mir-opt", "2", "--llvm-opt", "2"])):
        exe = os.path.join(tmp, probe["id"] + "_" + label + ".exe")
        c_code, c_out = run([cc, "--src=file", src, "--emit=exe", "-o", exe] + flags,
                            env, COMPILE_TIMEOUT)
        v = {"compile_exit": c_code, "compile_ok": (c_code == 0 and os.path.exists(exe))}
        if v["compile_ok"]:
            r_code, r_out = run([exe], make_env(None), RUN_TIMEOUT)
            v["run_exit"] = r_code
            v["stdout"] = r_out.strip()
            v["mir"] = mir_note(cc, src, env, extra=flags)
        else:
            v["compile_err"] = first_error(c_out)
        row["variants"][label] = v
        results[label] = v
    o0, o2 = results["O0"], results["O2"]
    same = (o0.get("compile_ok") and o2.get("compile_ok")
            and o0.get("run_exit") == o2.get("run_exit")
            and o0.get("stdout") == o2.get("stdout"))
    row["agree"] = bool(same)
    if not (o0.get("compile_ok") and o2.get("compile_ok")):
        row["verdict"] = "COMPILE_FAIL"
    elif not same:
        row["verdict"] = "O0_O2_DIVERGE"
    elif o0.get("run_exit") != probe["expect_run"] or probe["expect_out"] not in o0.get("stdout", ""):
        row["verdict"] = "WRONG_RESULT"
    else:
        row["verdict"] = "PASS"
    return row


def do_error(cc, probe, env, tmp):
    src = os.path.join(HERE, probe["src"])
    exe = os.path.join(tmp, probe["id"] + ".exe")
    row = {"kind": "error"}
    c_code, c_out = run([cc, "--src=file", src, "--emit=exe", "-o", exe],
                        env, COMPILE_TIMEOUT)
    row["compile_exit"] = c_code
    row["compile_err"] = first_error(c_out)
    produced = os.path.exists(exe)
    has_diag = ("error" in c_out.lower())
    if c_code == "TIMEOUT":
        row["verdict"] = "HANG"
    elif signal_note(c_code):
        row["verdict"] = "COMPILER_CRASH:" + signal_note(c_code)
    elif c_code == 0 or produced:
        row["verdict"] = "ACCEPTED_INVALID"  # bad: compiler said ok
    elif has_diag:
        row["verdict"] = "FAIL_CLOSED_OK"  # good: rejected with a diagnostic
    else:
        row["verdict"] = "FAIL_NO_DIAG"
    return row


def first_error(out):
    for ln in out.splitlines():
        low = ln.lower()
        if "error" in low or "I0100" in ln or "panic" in low:
            return ln.strip()[:200]
    tail = out.strip().splitlines()
    return (tail[-1].strip()[:200] if tail else "")


def verdict_run(probe, r_code, r_out):
    if r_code == "TIMEOUT":
        return "HANG"
    if signal_note(r_code):
        return "CRASH:" + signal_note(r_code)
    if r_code != probe["expect_run"]:
        return f"WRONG_EXIT({r_code})"
    if probe.get("expect_out") and probe["expect_out"] not in r_out:
        return "WRONG_OUTPUT"
    return "PASS"


DISPATCH = {"file": do_file, "project": do_project, "opt": do_opt, "error": do_error}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", action="append", default=[], metavar="LABEL=PATH",
                    help="compiler to test (repeatable)")
    ap.add_argument("--ldpath", default=None, help="LD_LIBRARY_PATH for the compiler")
    ap.add_argument("--json", default=None, help="write raw rows to this file")
    args = ap.parse_args()

    if not args.cc:
        print("need at least one --cc LABEL=PATH", file=sys.stderr)
        return 2

    compilers = []
    for spec in args.cc:
        label, _, path = spec.partition("=")
        compilers.append((label, path))

    env = make_env(args.ldpath)
    all_rows = {}
    for label, cc in compilers:
        print(f"\n===== compiler {label}: {cc} =====")
        rows = {}
        with tempfile.TemporaryDirectory() as tmp:
            for probe in PROBES:
                r = DISPATCH[probe["kind"]](cc, probe, env, tmp)
                r["probe"] = probe["id"]
                rows[probe["id"]] = r
                extra = ""
                if probe["kind"] == "opt":
                    extra = f" agree={r.get('agree')}"
                elif probe["kind"] != "error":
                    extra = f" run_exit={r.get('run_exit')}"
                print(f"  {probe['id']:28s} {r['verdict']:20s}{extra}")
        all_rows[label] = rows

    if args.json:
        with open(args.json, "w") as f:
            json.dump(all_rows, f, indent=2, default=str)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

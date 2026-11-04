"""Exercise the real oneTBB hook while a parent keeps its library loaded."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def worker(specification):
    # Isolate dlclose and any invalidated mappings in this child, so a crash
    # remains an observable failed check rather than killing the gate runner.
    spec = json.loads(specification.read_text())
    library = ctypes.CDLL(spec["loaded"])
    interface = library.TBB_runtime_interface_version
    interface.restype = ctypes.c_int
    before = interface()
    with open(spec["loaded"], "rb") as held:
        original = hashlib.sha256(held.read()).hexdigest()
        commands = []
        for command in spec["commands"]:
            result = subprocess.run(command, cwd=spec["cwd"], capture_output=True, timeout=90)
            commands.append({"command": command, "exit": result.returncode,
                             "stdout": result.stdout.decode(errors="replace"),
                             "stderr": result.stderr.decode(errors="replace")})
            if result.returncode:
                break
        held.seek(0)
        intact = original == hashlib.sha256(held.read()).hexdigest()
        after = interface()
    import _ctypes
    _ctypes.dlclose(library._handle)
    Path(spec["result"]).write_text(json.dumps({"commands": commands,
        "mapped_inode_intact": intact, "interface_before": before, "interface_after": after}, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--hook", type=Path, default=ROOT / "runtime/scripts/prepare_tbb.vyx")
    args = parser.parse_args()
    if sys.platform not in ("linux", "darwin"):
        print("oneTBB mapped-library install gate requires Linux or macOS")
        return 77
    compiler = args.compiler.resolve(strict=True)
    library = args.library.resolve(strict=True)
    hook = args.hook.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, VYX_CODEGEN_UNITS="1")
    payload = library.read_bytes()
    # A valid shared library with changed bytes; it still exports the actual
    # oneTBB ABI, without compiling or substituting a test implementation.
    changed = payload + b"\nVYX oneTBB installation regression\n" * 4096
    leaf = "libtbb.12.dylib" if sys.platform == "darwin" else "libtbb.so"
    names = ["libtbb.12.dylib", "libtbb.dylib"] if sys.platform == "darwin" else ["libtbb.so"]
    records = []
    for opt in ("0", "2"):
        executable = output / ("hook-O" + opt)
        compile_command = [str(compiler), "--src=file", str(hook), "--emit=exe", "-O" + opt, "-o", str(executable)]
        compiled = subprocess.run(compile_command, env=environment, capture_output=True, timeout=120)
        (output / ("compile-O" + opt + ".stdout.log")).write_bytes(compiled.stdout)
        (output / ("compile-O" + opt + ".stderr.log")).write_bytes(compiled.stderr)
        if compiled.returncode:
            raise RuntimeError(f"Cannot compile oneTBB hook at O{opt}: {compiled.stderr.decode(errors='replace')}")
        for scenario in ("identical", "changed", "destination-alias", "source-alias", "missing", "blocked", "compiler-identical", "compiler-changed"):
            name = scenario + "-O" + opt
            logs = output / name
            logs.mkdir()
            # Use the native temporary filesystem, including when this gate
            # is launched from a checkout mounted from another OS under WSL.
            with tempfile.TemporaryDirectory(prefix="vyx-tbb-install-") as temporary:
                fixture = Path(temporary)
                build, source = fixture / "bootstrap_compiler", fixture / "runtime/vendor/oneTBB"
                out = build / "out"
                out.mkdir(parents=True)
                (source / "include/oneapi/tbb").mkdir(parents=True)
                (source / "include/oneapi/tbb/parallel_for.h").touch()
                producer = source / "cmake-build/tbb_out_release" / leaf
                producer.parent.mkdir(parents=True)
                wanted = changed if scenario in ("changed", "destination-alias", "compiler-changed") else payload
                producer.write_bytes(wanted)
                external = fixture / leaf
                external.write_bytes(payload)
                initial = {}
                for target_name in names:
                    target = out / target_name
                    if scenario == "missing":
                        continue
                    if scenario == "blocked":
                        target.mkdir()
                        (target / "keep").write_bytes(payload)
                        continue
                    if scenario == "destination-alias":
                        target.symlink_to(external)
                    else:
                        target.write_bytes(payload)
                    initial[target_name] = (target.stat().st_ino, target.stat().st_mtime_ns)
                if sys.platform == "linux" and scenario != "blocked":
                    (out / "libtbb.so.12").symlink_to(leaf)
                if scenario == "source-alias":
                    producer.unlink()
                    producer.symlink_to(out / leaf)
                command = [str(executable)]
                if scenario.startswith("compiler-"):
                    command = [str(compiler), "--run=aot", "--src=file", str(hook), "-O" + opt]
                spec = logs / "specification.json"
                result_path = logs / "worker.json"
                spec.write_text(json.dumps({"loaded": str(external if scenario in ("blocked", "missing") else out / leaf),
                    "cwd": str(build), "commands": [command, command], "result": str(result_path)}, indent=2) + "\n")
                child_env = dict(environment)
                if sys.platform == "linux":
                    child_env["LD_LIBRARY_PATH"] = str(out) + ":" + child_env.get("LD_LIBRARY_PATH", "")
                # Darwin's worker explicitly dlopens the staged path; it must
                # not redirect the compiler's unrelated LLVM dependencies.
                process = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--worker", str(spec)],
                    env=child_env, capture_output=True, timeout=200)
                (logs / "stdout.log").write_bytes(process.stdout)
                (logs / "stderr.log").write_bytes(process.stderr)
                details = json.loads(result_path.read_text()) if result_path.exists() else {}
                commands = details.get("commands", [])
                expected_exit = 1 if scenario == "blocked" else 0
                passed = (process.returncode == 0 and len(commands) == (1 if expected_exit else 2)
                          and all(row["exit"] == expected_exit for row in commands)
                          and details.get("mapped_inode_intact") is True
                          and details.get("interface_before") == details.get("interface_after"))
                if scenario == "blocked":
                    passed &= all((out / n / "keep").read_bytes() == payload for n in names)
                    passed &= bool(commands) and "error: cannot compare oneTBB files:" in commands[0]["stdout"]
                else:
                    passed &= all((out / n).is_file() and (out / n).read_bytes() == wanted for n in names)
                    if scenario in ("identical", "source-alias", "compiler-identical"):
                        passed &= all((out / n).stat().st_ino == initial[n][0]
                                      and (out / n).stat().st_mtime_ns == initial[n][1] for n in names)
                    if scenario in ("changed", "destination-alias", "compiler-changed"):
                        passed &= all((out / n).stat().st_ino != initial[n][0] for n in names)
                passed &= external.read_bytes() == payload and not list(out.glob("*.vyx-*"))
                row = {"name": name, "worker_exit": process.returncode, "passed": bool(passed), **details}
                records.append(row)
                print(name + (": PASS" if passed else ": FAIL"), flush=True)
    report = {"compiler": str(compiler), "compiler_sha256": digest(compiler),
              "hook": str(hook), "hook_sha256": digest(hook), "library": str(library),
              "library_sha256": digest(library), "records": records,
              "passed": len(records) == 16 and all(row["passed"] for row in records)}
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--worker":
        worker(Path(sys.argv[2]))
    else:
        raise SystemExit(main())

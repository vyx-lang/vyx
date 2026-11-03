"""Windows native runtime ownership: explicit producer versus implicit SDK."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        print("Windows runtime companion gate: skipped on this host")
        return 77
    compiler = args.compiler.resolve(strict=True)
    runtime = args.runtime.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, VYX_RUNTIME_ROOT=str(runtime))
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    for name in ("vyx_runtime.lib", "tbb12.dll", "tbb12.lib"):
        if not (runtime / name).is_file():
            raise RuntimeError(f"Matching SDK runtime input missing: {runtime / name}")
    original = (runtime / "tbb12.dll").read_bytes()
    marked = original + b"\nVYX runtime-companions producer identity\n"
    pe = int.from_bytes(original[0x3c:0x40], "little")
    if original[:2] != b"MZ" or original[pe:pe + 4] != b"PE\0\0" or pe + 12 > len(original):
        raise RuntimeError("oneTBB input is not a complete PE image")
    same_size = bytearray(original)
    same_size[pe + 8] ^= 1  # COFF timestamp: different bytes, unchanged image layout.
    records = []
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryW.argtypes = [ctypes.c_wchar_p]
    kernel.LoadLibraryW.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    kernel.FreeLibrary.restype = ctypes.c_int
    for scenario in ("explicit", "implicit", "same-size", "blocked", "loaded-identical", "loaded-different"):
        explicit = scenario == "explicit"
        directory = output / scenario
        target = directory / "target"
        target.mkdir(parents=True)
        (directory / "main.vyx").write_text('''extern "C" {
    fn LoadLibraryA(path: rawptr) -> rawptr;
    fn FreeLibrary(handle: rawptr) -> i32;
}
fn main() -> i32 {
    unsafe {
        let handle = LoadLibraryA("tbb12.dll".c_str());
        if (handle == null) { return 1; }
        if (FreeLibrary(handle) == 0) { return 2; }
    }
    print("runtime companion ok");
    return 0;
}
''', encoding="utf8")
        manifest = ('[package]\nname="companion_probe"\nversion="0.1.0"\nentry="main.vyx"\n'
                    '[build]\ncache_dir=".cache"\noutput_dir="target"\n'
                    '[target.app]\ntype="executable"\nentry="main.vyx"\nauto_sources=false\nsources=["main.vyx"]\n')
        if explicit:
            provider = directory / "provider"
            provider.mkdir()
            for name in ("vyx_runtime.lib", "tbb12.lib"):
                shutil.copy2(runtime / name, provider / name)
            (target / "tbb12.dll").write_bytes(marked)
            manifest += 'lib_paths=["provider"]\nlibs=["vyx_runtime","tbb12","synchronization","ws2_32"]\n'
        elif scenario == "blocked":
            (target / "tbb12.dll").mkdir()
        elif scenario.startswith("loaded-"):
            (target / "tbb12.dll").write_bytes(original if scenario == "loaded-identical" else same_size)
        elif scenario == "same-size":
            (target / "tbb12.dll").write_bytes(same_size)
        (directory / "Vyx.toml").write_text(manifest, encoding="utf8")
        command = [str(compiler), "--src=project", str(directory), "--target", "app", "-j2"]
        handle = None
        if scenario.startswith("loaded-"):
            handle = kernel.LoadLibraryW(str(target / "tbb12.dll"))
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
        try:
            build = subprocess.run(command, env=environment, cwd=directory, capture_output=True, timeout=120)
        finally:
            if handle and not kernel.FreeLibrary(handle):
                raise ctypes.WinError(ctypes.get_last_error())
        (directory / "build.stdout.log").write_bytes(build.stdout)
        (directory / "build.stderr.log").write_bytes(build.stderr)
        dll, executable = target / "tbb12.dll", target / "app.exe"
        wanted = marked if explicit else original
        identity_ok = dll.is_file() and dll.read_bytes() == wanted
        run = None
        if build.returncode == 0 and executable.is_file():
            process = subprocess.run([str(executable)], env=environment, cwd=directory, capture_output=True, timeout=30)
            (directory / "run.stdout.log").write_bytes(process.stdout)
            (directory / "run.stderr.log").write_bytes(process.stderr)
            run = {"exit": process.returncode, "stdout": process.stdout.decode(errors="replace"),
                   "stderr": process.stderr.decode(errors="replace")}
        passed = (build.returncode == 0 and identity_ok and run is not None and run["exit"] == 0
                  and run["stdout"].replace("\r\n", "\n") == "runtime companion ok\n" and not run["stderr"])
        if scenario in ("blocked", "loaded-different"):
            diagnostics = (build.stdout + build.stderr).decode(errors="replace")
            passed = build.returncode == 1 and "cannot deploy oneTBB runtime:" in diagnostics
        records.append({"name": directory.name, "command": command, "build_exit": build.returncode,
                        "expected_dll_sha256": hashlib.sha256(wanted).hexdigest(),
                        "actual_dll_sha256": digest(dll) if dll.is_file() else None,
                        "identity_matches": identity_ok, "run": run, "passed": passed})
        print(directory.name + (": PASS" if passed else ": FAIL"), flush=True)
    report = {"compiler": str(compiler), "compiler_sha256": digest(compiler),
              "runtime": str(runtime), "inputs": {name: digest(runtime / name) for name in
               ("vyx_runtime.lib", "tbb12.dll", "tbb12.lib")}, "records": records,
              "passed": all(row["passed"] for row in records)}
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Run standard-library project contracts with an identified SDK compiler."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import tomllib
from model import write_project
from semver_model import write_project as write_semver_project

ROOT = Path(__file__).resolve().parents[3]
PACKAGES = ROOT / "bootstrap_compiler/std_packages"
PROJECTS = {
    "std_text_contracts": "std text contracts ok",
    "std_ownership_contracts": "std ownership contracts ok",
    "std_serde_contracts": "std serde contracts ok",
    "std_generic_contracts": "std generic contracts ok",
    "std_scalar_contracts": "std scalar contracts ok",
    "std_semver_contracts": "std semver contracts ok",
    "std_sync_contracts": "std.sync contracts ok",
    "std_owned_collections": "std owning collections contracts ok",
    "std_value_collections": "std value collections ok",
    "std_iterator_contracts": "std iterator contracts ok",
    "std_priority_queue_contracts": "std priority queue contracts ok",
    "std_ordered_contracts": "std ordered contracts ok",
    "std_testing_contracts": "std testing contracts ok",
    "std_fs_contracts": "std.fs contracts ok",
    "std_system_contracts": "std system contracts ok",
    "std_time_faults": "std time failure contracts ok",
    "std_sqlite_contracts": "std sqlite contracts ok",
    "std_collections_package": "std.collections package ok",
    "std_io_package": "std.io package closure ok",
    "std_json_package": "std.json package ok",
    "std_miniz_package": "std.miniz package ok",
    "std_meta_package": "std umbrella package ok",
}
NATIVE_PROJECTS = {
    "std_security_package": "std.security.openssl package ok",
    "std_crypto_faults": "std crypto failure contracts ok",
}
NATIVE_DLLS = {
    "std_sqlite_contracts": ("sqlite3.dll",),
    "std_security_package": ("libcrypto-3-x64.dll", "libssl-3-x64.dll"),
    "std_crypto_faults": (),
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def deploy_native_libraries(project: str, directories: list[Path], destination: Path) -> list[dict]:
    """Select explicit native inputs; a missing required DLL must not fall back to PATH."""
    import shutil
    selected = []
    for name in NATIVE_DLLS[project]:
        source = next((root / name for root in directories if (root / name).is_file()), None)
        if source is None:
            raise RuntimeError(f"Missing explicit --native-bin library: {name}")
        selected.append(source)
    result = []
    for source in selected:
        shutil.copy2(source, destination / source.name)
        result.append({"source": str(source), "sha256": sha256(source)})
    return result


def inventory() -> list[dict]:
    result = []
    sources = set()
    source_owners = {}
    for manifest in sorted(PACKAGES.glob("*/Vyx.toml")):
        data = tomllib.loads(manifest.read_text(encoding="utf-8"))
        targets = []
        for name, target in data.get("target", {}).items():
            declared = [target.get("entry", data["package"].get("entry", ""))]
            declared += target.get("sources", [])
            for relative in declared:
                if not relative:
                    continue
                source = manifest.parent / relative
                if not source.is_file():
                    raise RuntimeError(f"Missing package source: {source}")
                if source.suffix == ".vyx":
                    canonical = source.resolve()
                    owner = source_owners.setdefault(canonical, manifest.parent)
                    if owner != manifest.parent:
                        raise RuntimeError(f"Source owned by two packages: {canonical}: {owner}, {manifest.parent}")
                    sources.add(canonical)
            targets.append(name)
        result.append({"package": manifest.parent.name, "targets": targets,
                       "manifest_sha256": sha256(manifest)})
    registry = { (PACKAGES / line.strip()).resolve()
                 for line in (PACKAGES / "registry").read_text(encoding="utf-8").splitlines()
                 if line.strip() and not line.lstrip().startswith("#") }
    unexpected = registry - sources
    if unexpected:
        raise RuntimeError(f"Registry sources absent from manifests: {sorted(map(str, unexpected))}")
    return result


def input_hashes(projects: list[str]) -> dict[str, str]:
    result = {}
    for directory in [PACKAGES, Path(__file__).parent / "fixtures",
                      *(ROOT / "tests/projects" / name for name in projects)]:
        for path in directory.rglob("*"):
            if not path.is_file() or any(part in (".cache", "out", "target") for part in path.relative_to(directory).parts):
                continue
            if path.suffix in (".vyx", ".toml", ".c", ".h", ".cpp", ".txt") or path.name == "registry":
                result[path.relative_to(ROOT).as_posix()] = sha256(path)
    return result


def execute(command: list[str], directory: Path, environment: dict, timeout: int):
    options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
    process = subprocess.Popen(command, cwd=directory, env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **options)
    job = WindowsJob(process) if os.name == "nt" else None
    try:
        try:
            log, _ = process.communicate(timeout=timeout)
            return process.returncode, log
        except subprocess.TimeoutExpired:
            if job is not None:
                job.terminate()
            else:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            log, _ = process.communicate()
            return "timeout", log
    finally:
        if job is not None:
            job.close()


class WindowsJob:
    """A private, nested job containing only this invocation and its children."""
    def __init__(self, process):
        import ctypes as c
        from ctypes import wintypes as w
        class Basic(c.Structure):
            _fields_ = [("process_time", c.c_int64), ("job_time", c.c_int64), ("flags", w.DWORD),
                        ("min_working_set", c.c_size_t), ("max_working_set", c.c_size_t),
                        ("active_processes", w.DWORD), ("affinity", c.c_size_t),
                        ("priority", w.DWORD), ("scheduling", w.DWORD)]
        class Info(c.Structure):
            _fields_ = [("basic", Basic), ("io", c.c_uint64 * 6),
                        ("process_memory", c.c_size_t), ("job_memory", c.c_size_t),
                        ("peak_process_memory", c.c_size_t), ("peak_job_memory", c.c_size_t)]
        self.api = c.WinDLL("kernel32", use_last_error=True)
        self.api.CreateJobObjectW.argtypes = [c.c_void_p, w.LPCWSTR]
        self.api.CreateJobObjectW.restype = w.HANDLE
        self.api.SetInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
        self.api.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        self.api.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
        self.api.CloseHandle.argtypes = [w.HANDLE]
        self.handle = self.api.CreateJobObjectW(None, None)
        info = Info()
        info.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.handle or not self.api.SetInformationJobObject(self.handle, 9, c.byref(info), c.sizeof(info)) \
                or not self.api.AssignProcessToJobObject(self.handle, int(process._handle)):
            error = c.get_last_error()
            self.close()
            process.kill()
            raise c.WinError(error)

    def terminate(self):
        if not self.api.TerminateJobObject(self.handle, 1):
            import ctypes
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/stdlib-contracts")
    parser.add_argument("--projects", default=",".join(PROJECTS))
    parser.add_argument("--opt", choices=("0", "2", "both"), default="both")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--native-crypto", action="store_true", help="also run real OpenSSL and injected native failure contracts")
    parser.add_argument("--native-bin", type=Path, action="append", default=[], help="explicit native library directory (repeatable)")
    parser.add_argument("--negative", action="store_true", help="also check rejected traits and invalidated/bounds panics")
    parser.add_argument("--model", action="store_true", help="also generate value-model operation traces")
    parser.add_argument("--model-seed", type=int, default=935867)
    parser.add_argument("--model-block-size", type=int, default=16)
    parser.add_argument("--model-only", action="store_true", help="run only generated models")
    parser.add_argument("--model-timeout", type=int, default=600, help="budget for compiling and executing a generated model")
    parser.add_argument("--model-types", default="i32,i64,u64,f64,string")
    parser.add_argument("--model-steps", type=int, default=128)
    args = parser.parse_args()
    if args.timeout <= 0 or args.model_timeout <= 0:
        parser.error("timeout budgets must be positive")
    if not 1 <= args.model_block_size <= 128:
        parser.error("--model-block-size must be between 1 and 128")
    kinds = tuple(args.model_types.split(","))
    if not kinds or len(set(kinds)) != len(kinds) or any(t not in ("i32", "i64", "u64", "f64", "string") for t in kinds):
        parser.error("--model-types must select distinct supported element types")
    if not 1 <= args.model_steps <= 4096:
        parser.error("--model-steps must be between 1 and 4096")
    compiler = args.compiler.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "runner.py.snapshot").write_bytes(Path(__file__).read_bytes())
    (output / "model.py.snapshot").write_bytes(Path(__file__).with_name("model.py").read_bytes())
    selected = args.projects.split(",")
    if not selected or len(set(selected)) != len(selected) or any(name not in PROJECTS for name in selected):
        parser.error("--projects must select known contract projects")
    if args.model_only:
        args.model = True
        selected = []
    if args.native_crypto:
        selected += list(NATIVE_PROJECTS)
    markers = PROJECTS | NATIVE_PROJECTS
    environment = dict(os.environ)
    environment["VYX_STD_PACKAGES"] = str(PACKAGES)
    native_bins = [path.resolve(strict=True) for path in args.native_bin]
    if any(not path.is_dir() for path in native_bins):
        parser.error("--native-bin must name a directory")
    if native_bins:
        environment["PATH"] = os.pathsep.join([*(str(path) for path in native_bins), environment.get("PATH", "")])
    record = {"compiler": str(compiler), "compiler_sha256": sha256(compiler),
              "packages": inventory(), "results": [],
              "inputs": input_hashes(selected), "runner_sha256": sha256(Path(__file__)),
              "scope": "project contracts; package inventory is not API coverage"}
    model_directory = output / "model"
    semver_directory = output / "semver-model"
    if "std_semver_contracts" in selected:
        record["semver_model"] = write_semver_project(semver_directory)
        (output / "semver_model.py.snapshot").write_bytes(Path(__file__).with_name("semver_model.py").read_bytes())
    if args.model:
        record["model"] = write_project(model_directory, args.model_seed, args.model_steps,
                                        args.model_block_size, kinds)
        record["model"]["generator_sha256"] = sha256(Path(__file__).with_name("model.py"))
        record["model"]["source_sha256"] = sha256(model_directory / "main.vyx")
    for name in ("vyx_compiler_backend.dll", "libvyx_compiler_backend.so", "libvyx_compiler_backend.dylib",
                 "vyx_runtime.lib", "libvyx_runtime.a"):
        artifact = compiler.parent / name
        if artifact.is_file():
            record.setdefault("companions", {})[name] = sha256(artifact)
    opts = ("0", "2") if args.opt == "both" else (args.opt,)
    for opt in opts:
        for project in selected:
            directory = ROOT / "tests/projects" / project
            command = [str(compiler), "--src=project", str(directory), "--target", project,
                       "--emit=exe" if project in NATIVE_DLLS else "--run=aot", "-j4", f"-O{opt}"]
            start = time.monotonic()
            code, log = execute(command, directory, environment, args.timeout)
            commands = [command]
            libraries = []
            if code == 0 and project in NATIVE_DLLS:
                executable = directory / "target" / (project + (".exe" if os.name == "nt" else ""))
                if os.name == "nt":
                    # Deploy only explicitly selected DLLs to a private directory.
                    # No target-local or PATH DLL may shadow those identities.
                    import shutil
                    import tempfile
                    deployment = Path(tempfile.mkdtemp(prefix=f"{project}-O{opt}-", dir=output))
                    shutil.copy2(executable, deployment / executable.name)
                    libraries = deploy_native_libraries(project, native_bins, deployment)
                    executable = deployment / executable.name
                commands.append([str(executable)])
                code, runtime_log = execute(commands[-1], directory, environment, args.timeout)
                log += runtime_log
            (output / f"{project}-O{opt}.log").write_bytes(log)
            passed = code == 0 and markers[project].encode() in log
            result = {"project": project, "opt": opt, "command": command,
                      "commands": commands, "runtime_libraries": libraries,
                      "exit": code, "seconds": time.monotonic() - start, "passed": passed}
            record["results"].append(result)
            (output / "verification.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            print(f"{project} O{opt}: {'PASS' if passed else 'FAIL'} ({result['seconds']:.1f}s)", flush=True)
            if not passed:
                print(log[-7000:].decode("utf-8", "replace"), flush=True)
        if "semver_model" in record:
            command = [str(compiler), "--src=project", str(semver_directory), "--run=aot", "-j4", f"-O{opt}"]
            started = time.monotonic()
            code, log = execute(command, semver_directory, environment, args.timeout)
            passed = code == 0 and b"std semver model ok" in log
            (output / f"semver-model-O{opt}.log").write_bytes(log)
            record["results"].append({"project": "semver-model", "opt": opt, "command": command,
                "exit": code, "seconds": time.monotonic() - started, "passed": passed})
            (output / "verification.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            print(f"semver model O{opt}: {'PASS' if passed else 'FAIL'}", flush=True)
            if not passed:
                print(log[-7000:].decode("utf-8", "replace"), flush=True)
        if args.model:
            command = [str(compiler), "--src=project", str(model_directory), "--run=aot", "-j4", f"-O{opt}"]
            started = time.monotonic()
            code, log = execute(command, model_directory, environment, args.model_timeout)
            passed = code == 0 and b"std model contracts ok" in log
            (output / f"model-O{opt}.log").write_bytes(log)
            record["results"].append({"project": "model", "opt": opt, "command": command,
                "exit": code, "seconds": time.monotonic() - started, "passed": passed})
            (output / "verification.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            print(f"model O{opt}: {'PASS' if passed else 'FAIL'}", flush=True)
            if not passed:
                print(log[-7000:].decode("utf-8", "replace"), flush=True)
        if args.negative:
            for name, marker, compile_error in (
                ("no-eq", "Eq", True), ("no-ord", "Ord", True), ("no-hash", "Hashable", True),
                ("indirect-use-after-move", "moved", True),
                ("heap-no-ord", "Ord", True),
                ("operator-no-overload", "overload", True),
                ("operator-ambiguous", "ambiguous method call", True),
                ("heap-invalidates", "Iterator invalidated", False),
                ("heap-after-destroy", "PriorityQueue: use after destroy", False),
                ("sort-invalidates", "Iterator invalidated", False),
                ("swap-bounds", "Vec.swap out of bounds", False),
                ("semver-invalid-parse", "SemVer.parse:", False),
                ("semver-overflow", "SemVer.bumpPatch:", False),
                ("semver-invalid-value", "SemVer.compareTo:", False),
                ("semver-invalid-int", "semver.parse_int:", False),
                ("semver-negative-core", "SemVer.create:", False),
            ):
                directory = output / "negative" / f"{name}-O{opt}"
                directory.mkdir(parents=True, exist_ok=True)
                executable = directory / ("probe.exe" if os.name == "nt" else "probe")
                executable.unlink(missing_ok=True)
                source = Path(__file__).parent / "fixtures" / f"{name}.vyx"
                command = [str(compiler), "--src=file", str(source), "--emit=exe", f"-O{opt}", "-o", str(executable)]
                started = time.monotonic()
                code, log = execute(command, directory, environment, args.timeout)
                commands = [command]
                if compile_error:
                    text = log.decode("utf-8", "replace")
                    passed = code == 1 and not executable.exists() and marker in text and bool(re.search(r"error: E[0-9]+", text))
                elif code == 0 and executable.is_file():
                    commands.append([str(executable)])
                    code, runtime_log = execute(commands[-1], directory, environment, args.timeout)
                    log += runtime_log
                    passed = isinstance(code, int) and code != 0 and marker.encode() in runtime_log
                else:
                    passed = False
                (directory / "check.log").write_bytes(log)
                record["results"].append({"project": name, "opt": opt, "commands": commands,
                    "kind": "compile-error" if compile_error else "runtime-panic", "exit": code,
                    "seconds": time.monotonic() - started, "passed": passed})
                (output / "verification.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
                print(f"{name} O{opt}: {'PASS' if passed else 'FAIL'}", flush=True)
                if not passed:
                    print(log[-7000:].decode("utf-8", "replace"), flush=True)
    failures = sum(not row["passed"] for row in record["results"])
    record["inputs_changed"] = input_hashes(selected) != record["inputs"]
    record["compiler_changed"] = sha256(compiler) != record["compiler_sha256"]
    if record["inputs_changed"]:
        print("FAIL: source inputs changed during validation; run again with stable inputs", flush=True)
        failures += 1
    if record["compiler_changed"]:
        print("FAIL: compiler changed during validation", flush=True)
        failures += 1
    (output / "verification.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"stdlib contracts: {len(record['results']) - failures}/{len(record['results'])} passed", flush=True)
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()

"""Real Qt objects, original C++ exceptions, and forward/reverse shared ABI."""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ("compiler", "qt", "result"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    compiler, qt, result = (getattr(args, name).resolve() for name in ("compiler", "qt", "result"))
    probe = Path(__file__).resolve().parent
    repo = probe.parents[2]
    clang = repo / "clang/bin/clang++.exe"
    runtime = compiler.parent
    if not (runtime / "vyx_runtime.lib").is_file():
        runtime = compiler.parent.parent / "lib"
    if not (runtime / "vyx_runtime.lib").is_file():
        raise RuntimeError("matching vyx_runtime.lib missing for " + str(compiler))
    output = result.parent / "unwind"
    output.mkdir(parents=True, exist_ok=True)
    commands = []

    def run(name, command, env=None):
        command = list(map(str, command))
        commands.append(command)
        completed = subprocess.run(command, capture_output=True, text=True, errors="replace",
                                   timeout=90, env=env, cwd=probe)
        (output / (name + ".stdout.log")).write_text(completed.stdout, encoding="utf-8")
        (output / (name + ".stderr.log")).write_text(completed.stderr, encoding="utf-8")
        if completed.returncode:
            raise RuntimeError(f"{name}: exit={completed.returncode}\n{completed.stdout}\n{completed.stderr}")
        return completed.stdout

    native_flags = ["-std=c++17", "-fexceptions", "-fcxx-exceptions", "-fms-runtime-lib=static",
                    "-DQT_CORE_LIB", "-DQT_GUI_LIB", "-DQT_WIDGETS_LIB", "-DUNICODE", "-D_UNICODE",
                    "-I", probe / "native", "-I", qt / "include"]
    source = probe / "checks/unwind.vyx"
    contract = probe / "checks/contracts/qt_widgets_unwind.dcib"
    if not contract.is_file():
        raise RuntimeError("exception test contract missing; run the Qt gate to generate it: " + str(contract))
    stub = output / "overrides.cpp"
    run("stubs", [compiler, "--src=file", source, "--emit=dci-stubs", "-o", stub, "--dci", contract])
    stub_text = stub.read_text(encoding="utf-8")
    assert "setVisible" in stub_text and not re.search(r"setVisible\([^)]*\)\s*noexcept", stub_text), stub_text
    native = output / "native.obj"
    run("native", [clang, "-c", probe / "checks/native/exception_test.cpp", "-O2", *native_flags, "-o", native])
    checks = []
    windows = Path(os.environ.get("WINDIR", "C:/Windows"))
    isolated = {key: value for key, value in os.environ.items() if not key.upper().startswith("QT_")}
    isolated["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
    isolated["QT_QPA_PLATFORM"] = "offscreen"
    isolated["QT_QPA_FONTDIR"] = str(windows / "Fonts")
    for level in ("O0", "O2"):
        ir = output / (level + ".ll")
        run(level + "-ir", [compiler, "--src=file", source, "--emit=ir", "-" + level, "-o", ir])
        text = ir.read_text(encoding="utf-8")
        assert "__CxxFrameHandler3" in text and "invoke " in text and "cleanuppad" in text, ir
        assert "@vyx_runtime_malloc" in text and "@vyx_runtime_free" in text, ir
        # Intercept only allocations emitted in Vyx IR. Qt and C++ keep their
        # own allocator domains; the subclass factory is verified by destroyed.
        for original, tracked in (("vyx_runtime_malloc", "tracked_malloc"),
                                  ("vyx_runtime_free", "tracked_free"), ("vyx_runtime_calloc", "tracked_calloc"),
                                  ("calloc", "tracked_calloc")):
            text = re.sub(r"@" + original + r"(?=[(\s])", "@" + tracked, text)
        tracked_ir = output / (level + ".tracked.ll")
        tracked_ir.write_text(text, encoding="utf-8")
        stub_object = output / (level + ".stubs.obj")
        run(level + "-stub-object", [clang, "-c", stub, "-" + level, *native_flags, "-o", stub_object])
        exe = output / (level + ".exe")
        run(level + "-link", [clang, tracked_ir, native, stub_object,
                              "-L" + str(runtime), "-lvyx_runtime", "-ltbb12", "-lsynchronization", "-lws2_32",
                              *(qt / "lib" / (name + ".lib") for name in ("Qt6Widgets", "Qt6Gui", "Qt6Core")),
                              "-fms-runtime-lib=static", "-fuse-ld=lld", "-o", exe])
        deploy_env = os.environ.copy()
        deploy_env["QTDIR"] = str(qt)
        run(level + "-deploy", [windows / "System32/WindowsPowerShell/v1.0/powershell.exe",
                                "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                probe / "scripts/deploy_qt.ps1", "-Program", exe], env=deploy_env)
        log = run(level + "-run", [exe], env=isolated)
        for expected in ("mode=0 caught=0 objects=2 storage=3/3 native=1 OK",
                         "mode=1 caught=1 objects=2 storage=3/3 native=1 OK",
                         "mode=2 caught=1 objects=1 storage=0/0 native=1 OK",
                         "mode=3 caught=1 objects=1 storage=0/0 native=1 OK"):
            assert expected in log, log
        assert "Qt original exception and cleanup OK" in log, log
        checks.append(level + ": original exception, reverse virtual callback, constructor failure, Qt destruction and balanced storage")
        print(level + " Qt shared ABI forward/reverse and cleanup OK")
    report = {"status": "passed", "compiler": str(compiler), "compiler_sha256": digest(compiler),
              "runtime_sha256": digest(runtime / "vyx_runtime.lib"), "qt": str(qt),
              "test_contract": str(contract), "test_contract_sha256": digest(contract),
              "checks": checks, "commands": commands}
    result.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

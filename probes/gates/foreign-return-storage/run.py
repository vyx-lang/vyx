#!/usr/bin/env python3
"""Foreign storage must be independent by contract, not by a symbol's name."""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "language-composition"))
from suite import Executor, compiler_identity, digest, log_text, unique_dir, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    out = unique_dir(args.out)
    engine = Executor(Path(args.compiler), 90, 2048)
    engine.env["VYX_STD_PACKAGES"] = str(REPO / "bootstrap_compiler/std_packages")
    engine.env["VYX_RUNTIME_ROOT"] = str(engine.compiler.parent)
    cxx = str(Path(shutil.which(args.cxx) or args.cxx).resolve(strict=True))
    obj = out / ("producer.obj" if os.name == "nt" else "producer.o")
    backend_name = "vyx_compiler_backend.lib" if os.name == "nt" else (
        "libvyx_compiler_backend.dylib" if sys.platform == "darwin" else "libvyx_compiler_backend.so")
    backend = engine.compiler.parent / backend_name
    rows = []

    def execute(exe, arguments, directory, name):
        result = engine.execute(exe, arguments, directory, name, "1")
        text = log_text(directory / (name + ".stdout.log")) + log_text(directory / (name + ".stderr.log"))
        status = result["result"]
        clean = not any(status.get(k) for k in ("TimedOut", "MemoryExceeded", "DialogCaught"))
        return result, text, clean

    def check(name, source, expected=None, diagnostic=None, native=True, opt=0, compiler_api=False, extra=()):
        directory = out / name
        directory.mkdir()
        path = directory / "main.vyx"
        path.write_text(source, encoding="utf-8")
        artifact = directory / ("probe.exe" if os.name == "nt" else "probe")
        command = ["--src=file", path, "--emit=obj" if diagnostic else "--emit=exe", f"-O{opt}", "-o", artifact]
        if native:
            command += ["--link-obj", obj]
        if compiler_api:
            command += ["--link-obj", backend]
            if os.name == "nt":
                # The fixture is an executable linked to the backend API,
                # rather than the compiler's explicit-load plugin path.
                shutil.copy2(engine.compiler.parent / "vyx_compiler_backend.dll", directory)
        command += list(extra)
        build, text, clean = execute(engine.compiler, command, directory, "compile")
        row = {"name": name, "build": build, "diagnostic": text, "source_sha256": digest(path)}
        if diagnostic:
            row["pass"] = clean and build["result"]["ExitCode"] == 1 and re.search(diagnostic, text) is not None
        elif clean and build["result"]["ExitCode"] == 0:
            run, output, ran = execute(artifact, [], directory, "run")
            row.update(run=run, expected=expected, actual=output.replace("\r\n", "\n"))
            row["pass"] = ran and run["result"]["ExitCode"] == 0 and row["actual"] == expected
        else:
            row["pass"] = False
        rows.append(row)
        write_json(directory / "result.json", row)
        print(f"{name}: {'PASS' if row['pass'] else 'FAIL'}", flush=True)

    try:
        identity = compiler_identity(engine)
        native, text, clean = execute(cxx, ["-c", HERE / "fixtures/producer.cpp", "-o", obj], out, "producer")
        if not clean or native["result"]["ExitCode"] != 0:
            raise RuntimeError("native fixture build failed: " + text)
        for opt in (0, 2):
            check(f"decoder-O{opt}", (HERE / "fixtures/decoder.vyx").read_text(encoding="utf-8"),
                  "decoder PASS\n", native=False, opt=opt, compiler_api=True)
            check(f"index-source-O{opt}", (HERE / "fixtures/index_source.vyx").read_text(encoding="utf-8"),
                  "index source PASS\n", opt=opt)
        typed = (HERE / "fixtures/index_source.vyx").read_text(encoding="utf-8").replace(
            'fn gate_index_source(path: rawptr, length: rawptr) -> rawptr;',
            'fn gate_index_source(path: rawptr, length: rawptr) -> *u8;')
        check("typed-pointer", typed, "index source PASS\n")
        borrowed = ('use std.string;\nextern "C" { fn gate_alias(p: rawptr, length: rawptr) -> rawptr; }\n'
                    'fn invalid() -> str { let owner = String.from("local owning storage must not escape");\n'
                    'var n: i64 = 0; let p = gate_alias(owner.c_str(), to_rawptr(&n));\n'
                    'return from_cstr_view_len(p, 35); }\n'
                    'fn main() -> i32 { print(invalid()); return 0; }\n')
        check("unknown-alias", borrowed, diagnostic=r"main\.vyx:5:\d+: error: E3102")
        # A marker in opaque attribute data must not acquire semantic authority.
        forged = borrowed.replace('fn gate_alias', '@[opaque("|effect:ffi_returns_independent|")] fn gate_alias')
        check("forged-marker", forged, diagnostic=r"error: E3102")
        check("native-body-contract", 'use std.string;\n@[ffi_returns_independent]\n'
              'fn invalid() -> rawptr { let value = String.from("local"); return value.c_str(); }\n'
              'fn main() -> i32 { return 0; }\n', diagnostic=r"ffi_returns_independent requires a bodyless foreign function", native=False)
        check("scalar-contract", 'extern "C" { @[ffi_returns_independent] fn gate_bad() -> i32; }\n'
              'fn main() -> i32 { return 0; }\n', diagnostic=r"ffi_returns_independent requires a bodyless foreign function", native=False)
        check("contract-arguments", 'extern "C" { @[ffi_returns_independent(1)] fn gate_bad() -> rawptr; }\n'
              'fn main() -> i32 { return 0; }\n', diagnostic=r"ffi_returns_independent.*invalid arguments", native=False)
        check("dci-contract-rejected", 'extern "dci" { @[ffi_returns_independent] fn gate_bad() -> rawptr; }\n'
              'fn main() -> i32 { return 0; }\n', diagnostic=r"ffi_returns_independent requires.*C ABI only", native=False)
        # A similarly named ordinary function still has its body checked.
        check("local-owner-view", 'use std.string;\nfn gate_index_source() -> str {\n'
              'let owner = String.from("local owning storage must not escape"); return owner.raw(); }\n'
              'fn main() -> i32 { print(gate_index_source()); return 0; }\n', diagnostic=r"error: E3102", native=False)
        directory = out / "interface"
        directory.mkdir()
        declaration = ('module gate.foreign;\nextern "C" {\n@[ffi_returns_independent]\n'
                       'fn gate_index_source(path: rawptr, length: rawptr) -> rawptr;\n'
                       'fn gate_alias(storage: rawptr, length: rawptr) -> rawptr;\n}\n')
        (directory / "source.vyx").write_text(declaration, encoding="utf-8")
        interface = directory / "api.vyi"
        emitted, text, clean = execute(engine.compiler,
            ["--src=file", directory / "source.vyx", "--emit=vyi", "-o", interface], directory, "interface")
        content = interface.read_text(encoding="utf-8") if interface.exists() else ""
        good = (clean and emitted["result"]["ExitCode"] == 0
                and content.count('@[ffi_returns_independent]') == 1 and 'extern "C"' in content
                and 'fn gate_index_source(' in content and 'fn gate_alias(' in content
                and 'fn extern_C_block(' not in content)
        rows.append({"name": "interface-contract", "build": emitted, "diagnostic": text,
                     "source_sha256": digest(directory / "source.vyx"), "pass": good})
        print(f"interface-contract: {'PASS' if good else 'FAIL'}", flush=True)
        if good:
            imported = (HERE / "fixtures/index_source.vyx").read_text(encoding="utf-8")
            start, end = imported.index('extern "C"'), imported.index('\n}', imported.index('extern "C"')) + 2
            imported = imported[:start] + 'use gate.foreign;\n' + imported[end:]
            # Vyx interface lookup is path-based, so place it at the exact module path.
            module_path = directory / "gate"
            module_path.mkdir(parents=True)
            shutil.copy2(interface, module_path / "foreign.vyi")
            check("interface-consumer", imported, "index source PASS\n", extra=["--interface-root", directory])
            imported_alias = borrowed.replace('extern "C" { fn gate_alias(p: rawptr, length: rawptr) -> rawptr; }',
                                              'use gate.foreign;')
            check("interface-unknown-alias", imported_alias, diagnostic=r"error: E3102",
                  extra=["--interface-root", directory])
        write_json(out / "results.json", {"compiler": identity, "native": native,
                   "results": rows, "passed": sum(r["pass"] for r in rows), "failed": sum(not r["pass"] for r in rows)})
    finally:
        engine.close()
    return 0 if rows and all(r["pass"] for r in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())

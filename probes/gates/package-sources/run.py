"""AOT consumption of HTTP source, native binary and measured DCI binary packages."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

from test_sources import ROOT, Registry


def checked(args, cwd, env):
    result = subprocess.run([str(x) for x in args], cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
    print(result.stdout, end="", flush=True)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {args[0]}")
    return result.stdout


def archive(registry, directory, name, *, kind="source", triplet=None, abi=None):
    path = registry.root / f"{name}-1.0.0.zip"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as stream:
        for file in sorted(directory.rglob("*")):
            if file.is_file():
                stream.write(file, file.relative_to(directory).as_posix())
    artifact = {"url": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    release = artifact if kind == "source" else {"kind": kind, "artifacts": [{**artifact, "triplet": triplet, "abi": abi}]}
    registry.index["packages"][name] = {"versions": {"1.0.0": release}}
    registry.save()


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--cxx", type=Path, required=True)
    parser.add_argument("--ar", type=Path, required=True)
    parser.add_argument("--triplet", required=True)
    parser.add_argument("--abi", required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    args.cxx = args.cxx.resolve()
    args.ar = args.ar.resolve()
    env = dict(os.environ, VYX_PACKAGE_TOOLS=str(ROOT / "tools/packages"), VYX_PYTHON=sys.executable)
    env.pop("VYX_PACKAGE_PROJECT", None)
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    windows = "windows" in args.triplet
    with tempfile.TemporaryDirectory(prefix="vyx-package-aot-") as temporary:
        work = Path(temporary)
        env["VYX_PACKAGE_SOURCES"] = str(work / "sources.json")
        write(work / "sources.json", '{"schema":1,"sources":[]}')
        registry = Registry(work / "registry")
        try:
            # Source package with a transitive registry dependency compiled as a static Vyx library.
            leaf = work / "leaf"
            write(leaf / "Vyx.toml", '[package]\nname="pkg_leaf"\nversion="1.0.0"\n'
                  '[build]\noutput_dir="out"\n[target.pkg_leaf]\ntype="static"\nentry="src/lib.vyx"\n')
            write(leaf / "src/lib.vyx", 'module pkg_leaf;\npublic fn leaf_answer() -> i32 { return 42; }\n')
            archive(registry, leaf, "pkg_leaf")
            facade = work / "facade"
            write(facade / "Vyx.toml", '[package]\nname="pkg_facade"\nversion="1.0.0"\n'
                  '[target.pkg_facade]\ntype="source"\nsources=["src/lib.vyx"]\n'
                  '[dependencies]\nleaf={package="pkg_leaf", version="^1.0", source="fixture"}\n')
            write(facade / "src/lib.vyx", 'module pkg_facade;\nuse pkg_leaf;\n'
                  'public fn source_answer() -> i32 { return leaf_answer(); }\n')
            archive(registry, facade, "pkg_facade")

            # Ordinary binary package: precompiled native archive plus an explicit Vyx interface.
            native = work / "native"
            native.mkdir()
            write(native / "producer.cpp", 'extern "C" int packaged_product(int value) { return value * 7; }\n'
                  'int dci_packaged_answer(int value) noexcept { return value + 35; }\n')
            obj = native / ("producer.obj" if windows else "producer.o")
            flags = ["-O2", "-std=c++17"] + (["-fms-runtime-lib=static"] if windows else ["-fPIC"])
            checked([args.cxx, "-c", native / "producer.cpp", *flags, "-o", obj], work, env)
            library = native / ("producer.lib" if windows else "libproducer.a")
            checked([args.ar, "rcs", library, obj], work, env)
            binary = work / "binary"
            (binary / "lib").mkdir(parents=True)
            shutil.copy2(library, binary / "lib" / library.name)
            write(binary / "Vyx.toml", '[package]\nname="pkg_binary"\nversion="1.0.0"\nkind="binary"\n'
                  '[target.pkg_binary]\ntype="prebuilt"\nlinkage="static"\n'
                  f'artifact="lib/{library.name}"\ntriplet="{args.triplet}"\nabi="{args.abi}"\n'
                  'sources=["src/lib.vyx"]\n')
            write(binary / "src/lib.vyx", 'module pkg_binary;\nextern "C" { fn packaged_product(value: i32) -> i32; }\n'
                  'public fn binary_answer() -> i32 { return packaged_product(6); }\n')
            archive(registry, binary, "pkg_binary", kind="binary", triplet=args.triplet, abi=args.abi)

            # DCI package: producer native C++ symbol, Adapter-measured contract and Converter definition.
            dci = work / "dci"
            (dci / "contracts").mkdir(parents=True)
            (dci / "lib").mkdir()
            shutil.copy2(library, dci / "lib" / library.name)
            write(native / "producer.hpp", 'int dci_packaged_answer(int value) noexcept;\n')
            checked([sys.executable, ROOT / "tools/dci/dci.py", "adapter", "--language", "cpp",
                     native / "producer.hpp", "--triplet", args.triplet, "--toolchain", "clang",
                     "--compiler", args.cxx, "--extractor", args.cxx, "--export-function", "dci_packaged_answer",
                     "-o", dci / "contracts/native.dcib", "--project-root", native], work, env)
            checked([sys.executable, ROOT / "tools/dci/dci.py", "convert", dci / "contracts/native.dcib",
                     "--module", "pkg_dci", "-o", dci / "src/lib.vyx", "--deny-rejected"], work, env)
            write(dci / "Vyx.toml", '[package]\nname="pkg_dci"\nversion="1.0.0"\nkind="dci"\n'
                  '[target.pkg_dci]\ntype="prebuilt"\nlinkage="static"\n'
                  f'artifact="lib/{library.name}"\ntriplet="{args.triplet}"\nabi="{args.abi}"\n'
                  'sources=["src/lib.vyx"]\ndci_files=["contracts/native.dcib"]\n')
            archive(registry, dci, "pkg_dci", kind="dci", triplet=args.triplet, abi=args.abi)

            # A Vyx binary package exposes the compiler-produced .vyi, not its implementation source.
            vyi_source = work / "vyi-source"
            write(vyi_source / "Vyx.toml", '[package]\nname="pkg_vyi"\nversion="1.0.0"\n'
                  '[build]\noutput_dir="out"\n[target.pkg_vyi]\ntype="static"\nentry="src/lib.vyx"\n')
            write(vyi_source / "src/lib.vyx", 'module pkg_vyi;\npublic fn vyi_answer() -> i32 { return 42; }\n')
            checked([compiler, "build", "--triplet", args.triplet, "-j2"], vyi_source, env)
            vyi = work / "vyi"
            (vyi / "lib").mkdir(parents=True)
            (vyi / "interfaces").mkdir()
            vyi_lib = "pkg_vyi.lib" if windows else "libpkg_vyi.a"
            shutil.copy2(vyi_source / "out" / vyi_lib, vyi / "lib" / vyi_lib)
            candidates = list((vyi_source / "out").rglob("*.vyi"))
            public = [path for path in candidates if path.name == "pkg_vyi.vyi" and ".vyx_interfaces" not in path.parts]
            if len(public) != 1:
                raise RuntimeError(f"no unique published .vyi: {candidates}")
            shutil.copy2(public[0], vyi / "interfaces/pkg_vyi.vyi")
            write(vyi / "Vyx.toml", '[package]\nname="pkg_vyi"\nversion="1.0.0"\nkind="binary"\n'
                  '[target.pkg_vyi]\ntype="prebuilt"\nlinkage="static"\n'
                  f'artifact="lib/{vyi_lib}"\ntriplet="{args.triplet}"\nabi="{args.abi}"\n'
                  'interface_sources=["interfaces/pkg_vyi.vyi"]\n')
            archive(registry, vyi, "pkg_vyi", kind="binary", triplet=args.triplet, abi=args.abi)

            # A separate native shared library verifies runtime deployment, including warm-cache repair.
            shared = work / "shared"
            (shared / "lib").mkdir(parents=True)
            (shared / "bin").mkdir()
            export = '__declspec(dllexport) ' if windows else ''
            write(native / "shared.cpp", f'extern "C" {export}int packaged_shared_answer() {{ return 42; }}\n')
            runtime_name = "packaged_shared.dll" if windows else "libpackaged_shared.so"
            shared_lib = "packaged_shared.lib" if windows else runtime_name
            shared_args = [args.cxx, "-shared", native / "shared.cpp", *flags, "-o", shared / "bin" / runtime_name]
            shared_args += ([f"-Wl,/implib:{shared / 'lib' / shared_lib}"] if windows
                            else [f"-Wl,-soname,{runtime_name}"])
            checked(shared_args, work, env)
            if not windows:
                shutil.copy2(shared / "bin" / runtime_name, shared / "lib" / shared_lib)
            write(shared / "Vyx.toml", '[package]\nname="pkg_shared"\nversion="1.0.0"\nkind="binary"\n'
                  '[target.pkg_shared]\ntype="prebuilt"\nlinkage="shared"\n'
                  f'artifact="lib/{shared_lib}"\ntriplet="{args.triplet}"\nabi="{args.abi}"\n'
                  f'sources=["src/lib.vyx"]\nruntime_files=["bin/{runtime_name}"]\n')
            write(shared / "src/lib.vyx", 'module pkg_shared;\nextern "C" { fn packaged_shared_answer() -> i32; }\n'
                  'public fn shared_answer() -> i32 { return packaged_shared_answer(); }\n')
            archive(registry, shared, "pkg_shared", kind="binary", triplet=args.triplet, abi=args.abi)

            for package, call in (("pkg_facade", "source_answer()"), ("pkg_binary", "binary_answer()"),
                                  ("pkg_vyi", "vyi_answer()"), ("pkg_shared", "shared_answer()"),
                                  ("pkg_dci", "dci_packaged_answer(7)")):
                app = work / package
                write(app / "Vyx.toml", '[package]\nname="app"\nversion="0.1.0"\nentry="src/main.vyx"\n'
                      '[build]\noutput_dir="target"\n'
                      f'[sources.fixture]\nurl="{registry.url}"\n'
                      '[target.app]\ntype="executable"\nentry="src/main.vyx"\n'
                      f'[target.app.dependencies]\n{package}={{version="1.0.0",source="fixture"}}\n')
                write(app / "src/main.vyx", f'use {package};\nfn main() -> i32 {{\n'
                      f'    let answer = {call};\n    if (answer != 42) {{ return 1; }}\n'
                      f'    print("{package} AOT OK");\n    return 0;\n}}\n')
                checked([compiler, "source", "list"], app, env)
                checked([compiler, "--run=aot", "--src=project", app, "--triplet", args.triplet, "-j2"], work, env)
                if package == "pkg_shared":
                    deployed = app / "target" / runtime_name
                    if not deployed.is_file():
                        raise RuntimeError("prebuilt runtime was not deployed")
                    deployed.unlink()
                count = len(registry.requests)
                checked([compiler, "build", "--run=aot", "--offline", "--locked", "--triplet", args.triplet, "-j2"], app, env)
                if len(registry.requests) != count:
                    raise RuntimeError("offline build accessed registry")
                lock = json.loads((app / "Vyx.packages.lock").read_text())
                if package not in json.dumps(lock):
                    raise RuntimeError("package missing from lock")

            # Two dependencies must not silently replace each other's runtime at the application boundary.
            other = work / "shared-other"
            shutil.copytree(shared, other)
            path = other / "Vyx.toml"
            path.write_text(path.read_text().replace("pkg_shared", "pkg_shared_other"), encoding="utf-8")
            write(other / "src/lib.vyx", "module pkg_shared_other;\n")
            with (other / "bin" / runtime_name).open("ab") as stream:
                stream.write(b"different-runtime-payload")
            archive(registry, other, "pkg_shared_other", kind="binary", triplet=args.triplet, abi=args.abi)
            conflict = work / "runtime-conflict"
            write(conflict / "Vyx.toml", '[package]\nname="app"\nversion="0.1.0"\nentry="src/main.vyx"\n'
                  f'[sources.fixture]\nurl="{registry.url}"\n'
                  '[target.app]\ntype="executable"\nentry="src/main.vyx"\n'
                  '[target.app.dependencies]\npkg_shared={version="1.0.0",source="fixture"}\n'
                  'pkg_shared_other={version="1.0.0",source="fixture"}\n')
            write(conflict / "src/main.vyx", "use pkg_shared;\nfn main() -> i32 { return shared_answer() - 42; }\n")
            rejected = subprocess.run([str(compiler), "build", "--triplet", args.triplet], cwd=conflict,
                                      env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            print(rejected.stdout, end="", flush=True)
            if not rejected.returncode or "package runtime filename conflict" not in rejected.stdout:
                raise RuntimeError("different runtime payloads with the same filename were not explicitly rejected")
            print("package-sources: source/transitive, native binary, DCI binary, VYI, shared runtime AOT/offline and runtime conflict refusal PASS")
        except Exception:
            failed = ROOT / "probes/gates/package-sources/.cache/failed-aot"
            shutil.copytree(work, failed, dirs_exist_ok=True)
            print(f"Failure workspace saved: {failed}", flush=True)
            raise
        finally:
            registry.close()


if __name__ == "__main__":
    main()

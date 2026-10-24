"""Validate native DCIB emission and execute a separate AOT DCI consumer."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

GATE = Path(__file__).resolve().parent
ROOT = GATE.parents[2]
sys.path.insert(0, str(ROOT / "tools/dci"))
import dcib
import dci_validate


def checked(args, cwd, env, *, success=True, timeout=240):
    command = [str(arg) for arg in args]
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    print(result.stdout, end="", flush=True)
    if (result.returncode == 0) != success:
        raise RuntimeError(f"unexpected exit {result.returncode}: {command}")
    if not success and (result.returncode not in (1, 2) or "error" not in result.stdout.lower()):
        raise RuntimeError(f"expected a normal diagnostic rejection, received {result.returncode}: {command}")
    return result.stdout


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def compiler_runtime_path(compiler, windows):
    name = "vyx_runtime.lib" if windows else "libvyx_runtime.a"
    # Source builds colocate the runtime in out/; installed SDKs keep it
    # in lib/, alongside the bundled application link dependencies.
    for directory in (compiler.parent, compiler.parent.parent / "lib"):
        path = directory / name
        if path.is_file():
            return path
    raise RuntimeError(f"matching compiler runtime not found: {name}")


def verify_contract(path, expected_links, triplet):
    blob = path.read_bytes()
    document = dcib.decode(blob)
    if dcib.encode(document) != blob:
        raise RuntimeError("native DCIB is not byte-identical to canonical Python encoding")
    schema = dci_validate.load_json(dci_validate.DEFAULT_SCHEMA)
    errors = dci_validate.schema_errors(schema, document)
    if isinstance(document, dict):
        errors.extend(dci_validate.semantic_errors(document, strict=True))
        errors.extend(dci_validate.dci_obligations.validate_graph([(str(path), document)], check_local=False))
    if errors:
        raise RuntimeError("strict contract validation failed: " + "; ".join(errors))
    symbols = document["exports"]["symbols"]
    actual = {symbol["name"]: symbol["link_name"] for symbol in symbols}
    if len(symbols) != len(actual):
        raise RuntimeError("duplicate selected semantic name in the exported contract")
    if actual != expected_links:
        raise RuntimeError(f"unexpected exported symbols/aliases: {actual}, expected {expected_links}")
    if document["source"]["language"].lower() != "vyx":
        raise RuntimeError("contract does not identify Vyx as its producer")
    if document["target"]["triple"] != triplet:
        raise RuntimeError("contract target differs from selected target")
    return document


def convert_consume(compiler, contract, artifact, directory, env, triplet, *, extra=False):
    directory.mkdir(parents=True, exist_ok=True)
    definitions = directory / "definitions.vyx"
    checked([sys.executable, ROOT / "tools/dci/dci.py", "convert", contract,
             "--module", "native_exports", "-o", definitions, "--deny-rejected"], directory, env)
    generated = definitions.read_text(encoding="utf-8")
    if 'extern "dci"' not in generated or 'extern "C"' in generated:
        raise RuntimeError("Converter did not preserve the DCI consumer boundary")
    source = (GATE / "fixtures/consumer.vyx").read_text(encoding="utf-8")
    if extra:
        source = source.replace("    exported_void();", "    if (exported_extra(35) != 42) { return 6; }\n    exported_void();")
    write(directory / "main.vyx", source)
    write(directory / "modules.txt", str(definitions) + "\n")
    output = checked([compiler, "--run=aot", "--src=file", directory / "main.vyx",
                      "--module-sources-file", directory / "modules.txt", "--dci", contract,
                      "--link-obj", artifact, "--triplet", triplet], directory, env)
    if "native DCI export PASS" not in output:
        raise RuntimeError("consumer did not execute its assertions")


def rejected_output(compiler, input_path, output_path, env, triplet, *extra):
    sentinel = b"existing contract must survive rejected export\n"
    output_path.write_bytes(sentinel)
    checked([compiler, "--emit=dcib", "--src=file", input_path, "-o", output_path,
             "--triplet", triplet, *extra], output_path.parent, env, success=False)
    if output_path.read_bytes() != sentinel:
        raise RuntimeError("a rejected export modified the previous artifact")


def verify_distinct_root_chunks(project, target):
    tasks = (project / ".cache" / f"tasks_{target}.tmp").read_text(encoding="utf-8")
    roots = []
    for match in re.finditer(r'--emit-root-names-file\s+"([^"]+)"', tasks):
        names = Path(match.group(1).replace("\\", os.sep))
        if not names.is_absolute():
            names = project / names
        roots.append(set(names.read_text(encoding="utf-8").split("|")))
    add = [index for index, names in enumerate(roots) if "exported_add" in names]
    shared = [index for index, names in enumerate(roots) if "exported_shared_helper" in names]
    if len(add) != 1 or len(shared) != 1 or add[0] == shared[0]:
        raise RuntimeError(f"shared helper regression did not exercise distinct root chunks: {roots}")


def verify_private_helper_interface(project, target):
    interface = (project / "out" / f"{target}.vyi").read_text(encoding="utf-8")
    if re.search(r"\bfn\s+add_seven\s*\(", interface):
        raise RuntimeError("cross-CGU linkage exposed the private helper in the published interface")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--triplet", required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    env = dict(os.environ)
    env.pop("VYX_DCI_EXPORT_OUT", None)
    env.pop("VYX_PACKAGE_PROJECT", None)
    # Export itself must work without the Python Adapter/Converter toolchain.
    env["VYX_PYTHON"] = "__native_dcib_gate_no_python__"
    env["VYX_DCI_TOOLS"] = "__native_dcib_gate_no_external_tools__"
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    windows = "windows" in args.triplet
    expected = {
        "exported_add": "native_gate_add",
        "exported_i64": "native_gate_i64",
        "exported_f32": "native_gate_f32",
        "exported_f64": "native_gate_f64",
        "exported_bool": "native_gate_bool",
        "exported_void": "native_gate_void",
        "exported_pointer": "native_gate_pointer",
        "exported_default": "exported_default",
        "exported_shared_helper": "native_gate_shared_helper",
    }
    cache = GATE / ".cache"
    cache.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-export-", dir=cache) as temporary:
        work = Path(temporary).resolve()
        if not work.is_relative_to(cache.resolve()):
            raise RuntimeError("temporary gate workspace escaped its cache root")
        try:
            scalar = GATE / "fixtures/scalars.vyx"
            contract = work / "scalars.dcib"
            export_args = [compiler, "--emit=dcib", "--src=file", scalar,
                           "--triplet", args.triplet]
            checked([*export_args, "-o", contract], work, env)
            verify_contract(contract, expected, args.triplet)
            checked([*export_args, "-o", work / "scalars-again.dcib"], work, env)
            if contract.read_bytes() != (work / "scalars-again.dcib").read_bytes():
                raise RuntimeError("same source/options produced different DCIB bytes")
            # Thirty-two shared levels have only 33 function bodies. Proving
            # each incoming edge independently used to revisit billions of
            # paths, despite the tiny native program. Keep a generous timeout
            # to distinguish that regression from ordinary host scheduling.
            shared = GATE / "fixtures/shared_call_graph.vyx"
            shared_contract = work / "shared-call-graph.dcib"
            shared_start = time.perf_counter()
            checked([compiler, "--emit=dcib", "--src=file", shared,
                     "--triplet", args.triplet, "-O0", "-o", shared_contract],
                    work, env, timeout=20)
            shared_seconds = time.perf_counter() - shared_start
            verify_contract(shared_contract,
                            {"exported_shared_graph": "native_gate_shared_graph"}, args.triplet)
            obj = work / ("scalars.obj" if windows else "scalars.o")
            checked([compiler, "--emit=obj", "--src=file", scalar, "-o", obj,
                     "--triplet", args.triplet], work, env)
            convert_consume(compiler, contract, obj, work / "file-consumer", env, args.triplet)

            project = work / "producer"
            write(project / "Vyx.toml", '[package]\nname="native_export_project"\nversion="1.0.0"\n'
                  '[build]\noutput_dir="out"\n[target.api]\ntype="static"\n'
                  'sources=["src/scalars.vyx","src/extra.vyx"]\n'
                  '[target.partitioned]\ntype="static"\nmir_partition=true\n'
                  'sources=["src/scalars.vyx","src/extra.vyx"]\n'
                  '[target.app]\ntype="exe"\nentry="src/application.vyx"\nmir_partition=false\n'
                  'sources=["src/application.vyx","src/extra.vyx"]\n'
                  '[target.partitioned_app]\ntype="exe"\nentry="src/application.vyx"\nmir_partition=true\n'
                  'sources=["src/application.vyx","src/extra.vyx"]\n'
                  '[dependencies]\nhidden={path="../hidden",target="hidden"}\n')
            # The project planner deliberately keeps <=50KB single-file
            # modules whole. Cross that documented implementation threshold
            # with a generated comment, without publishing a large fixture.
            write(project / "src/scalars.vyx", scalar.read_text(encoding="utf-8")
                  + "\n// Root-partition regression padding: " + ("x" * 52000) + "\n")
            write(project / "src/extra.vyx", 'module native_export_extra;\nuse hidden_export;\n'
                  '@[dci_export(symbol="native_gate_extra")]\n'
                  'public fn exported_extra(value: i32) -> i32 { return hidden_add_seven(value); }\n')
            write(project / "src/application.vyx", 'module native_export_application;\n'
                  'use native_export_extra;\nuse hidden_export;\n'
                  'fn main() -> i32 {\n'
                  '    if (exported_extra(10) != 17 || hidden_add_seven(20) != 27) { return 1; }\n'
                  '    print("source-dependency=ok"); return 0;\n}\n')
            hidden = work / "hidden"
            write(hidden / "Vyx.toml", '[package]\nname="hidden_export"\nversion="1.0.0"\n'
                  '[target.hidden]\ntype="source"\nsources=["src/lib.vyx"]\n')
            write(hidden / "src/lib.vyx", 'module hidden_export;\n'
                  '@[dci_export(symbol="native_gate_hidden")]\n'
                  'public fn hidden_add_seven(value: i32) -> i32 { return value + 7; }\n')
            project_args = [compiler, "--emit=dcib", "--src=project", project,
                            "--target", "api", "--triplet", args.triplet, "-j2"]
            checked(project_args, work, env)
            default_contract = project / "out/api.dcib"
            verify_contract(default_contract, {**expected, "exported_extra": "native_gate_extra"}, args.triplet)
            explicit_contract = work / "project-explicit.dcib"
            checked([*project_args, "-o", explicit_contract], work, env)
            if default_contract.read_bytes() != explicit_contract.read_bytes():
                raise RuntimeError("project default and -o exports disagree")
            checked([compiler, "build", "--emit=dcib", "--target", "api",
                     "--triplet", args.triplet, "-j2"], project, env)
            if default_contract.read_bytes() != explicit_contract.read_bytes():
                raise RuntimeError("build shorthand and --src=project exports disagree")
            checked([*project_args, "--run=aot"], work, env, success=False)
            if default_contract.read_bytes() != explicit_contract.read_bytes():
                raise RuntimeError("rejected project --run modified the previous contract")
            checked([compiler, "build", "--target", "api", "--triplet", args.triplet, "-j2"], project, env)
            verify_private_helper_interface(project, "api")
            library = project / "out" / ("api.lib" if windows else "libapi.a")
            convert_consume(compiler, default_contract, library, work / "project-consumer", env,
                            args.triplet, extra=True)

            # Repeated root chunks compile the same private, explicitly
            # noinline callee. Its source visibility must not change, while
            # its machine definition remains linkable without strong duplicates.
            checked([compiler, "--emit=dcib", "--src=project", project, "--target", "partitioned",
                     "--triplet", args.triplet, "-j2"], work, env)
            partitioned_contract = project / "out/partitioned.dcib"
            verify_contract(partitioned_contract, {**expected, "exported_extra": "native_gate_extra"}, args.triplet)
            checked([compiler, "build", "--target", "partitioned", "--triplet", args.triplet, "-j2"], project, env)
            verify_distinct_root_chunks(project, "partitioned")
            verify_private_helper_interface(project, "partitioned")
            partitioned_library = project / "out" / ("partitioned.lib" if windows else "libpartitioned.a")
            convert_consume(compiler, partitioned_contract, partitioned_library,
                            work / "partitioned-consumer", env, args.triplet, extra=True)
            for application_target in ("app", "partitioned_app"):
                application = checked([compiler, "--src=project", project, "--target", application_target,
                                       "--run=aot", "--triplet", args.triplet, "-j2"], work, env)
                if "source-dependency=ok" not in application:
                    raise RuntimeError(f"{application_target}: source package implementation was not executed")

            for fixture in ("unsupported_string.vyx", "unsupported_reference.vyx", "no_selection.vyx", "unproven_call.vyx",
                            "unproven_recursion.vyx",
                            "mixed_selected_method.vyx", "mixed_selected_module.vyx",
                            "unselected_native_collision.vyx", "unselected_no_mangle_collision.vyx",
                            "unselected_no_mangle_block_collision.vyx", "unselected_export_attr_collision.vyx",
                            "unselected_export_marker_collision.vyx"):
                rejected_output(compiler, GATE / "fixtures" / fixture, work / (fixture + ".dcib"),
                                env, args.triplet)
            rejected_output(compiler, scalar, work / "run-combination.dcib", env, args.triplet, "--run=aot")
            write(cache / "verification.json", json.dumps({
                "compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
                "backend_sha256": hashlib.sha256((compiler.parent / ("vyx_compiler_backend.dll" if windows
                    else "libvyx_compiler_backend.dylib" if sys.platform == "darwin"
                    else "libvyx_compiler_backend.so")).read_bytes()).hexdigest(),
                "runtime_sha256": hashlib.sha256(compiler_runtime_path(compiler, windows).read_bytes()).hexdigest(),
                "triplet": args.triplet, "result": "PASS", "file_aot": True, "project_aot": True,
                "canonical_roundtrip": True, "deterministic": True,
                "dependency_exports_filtered": True, "rejected_output_preserved": True,
                "source_dependency_executables": ["app", "partitioned_app"],
                "private_noinline_helper": True, "distinct_project_root_chunks": True,
                "private_helper_not_in_interface": True,
                "shared_graph_levels": 32, "shared_graph_seconds": shared_seconds,
                "shared_graph_source_sha256": hashlib.sha256(shared.read_bytes()).hexdigest(),
            }, ensure_ascii=False, indent=2) + "\n")
        except Exception:
            failed = cache / "failed" / work.name
            failed.parent.mkdir(exist_ok=True)
            shutil.copytree(work, failed)
            print(f"native export failure workspace: {failed}", file=sys.stderr)
            raise
    print("dci-native-export: canonical DCIB, explicit selection, file/project AOT and rejection gates PASS")


if __name__ == "__main__":
    main()

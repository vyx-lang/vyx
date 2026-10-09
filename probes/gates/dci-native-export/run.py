"""Validate native DCIB emission and execute a separate AOT DCI consumer."""
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

GATE = Path(__file__).resolve().parent
ROOT = GATE.parents[2]
sys.path.insert(0, str(ROOT / "tools/dci"))
import dcib
import dci_validate


def checked(args, cwd, env, *, success=True):
    command = [str(arg) for arg in args]
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
    print(result.stdout, end="", flush=True)
    if (result.returncode == 0) != success:
        raise RuntimeError(f"unexpected exit {result.returncode}: {command}")
    if not success and (result.returncode not in (1, 2) or "error" not in result.stdout.lower()):
        raise RuntimeError(f"expected a normal diagnostic rejection, received {result.returncode}: {command}")
    return result.stdout


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


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
            obj = work / ("scalars.obj" if windows else "scalars.o")
            checked([compiler, "--emit=obj", "--src=file", scalar, "-o", obj,
                     "--triplet", args.triplet], work, env)
            convert_consume(compiler, contract, obj, work / "file-consumer", env, args.triplet)

            project = work / "producer"
            write(project / "Vyx.toml", '[package]\nname="native_export_project"\nversion="1.0.0"\n'
                  '[build]\noutput_dir="out"\n[target.api]\ntype="static"\n'
                  'sources=["src/scalars.vyx","src/extra.vyx"]\n'
                  '[dependencies]\nhidden={path="../hidden",target="hidden"}\n')
            write(project / "src/scalars.vyx", scalar.read_text(encoding="utf-8"))
            write(project / "src/extra.vyx", 'module native_export_extra;\nuse hidden_export;\n'
                  '@[dci_export(symbol="native_gate_extra")]\n'
                  'public fn exported_extra(value: i32) -> i32 { return hidden_add_seven(value); }\n')
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
            library = project / "out" / ("api.lib" if windows else "libapi.a")
            convert_consume(compiler, default_contract, library, work / "project-consumer", env,
                            args.triplet, extra=True)

            for fixture in ("unsupported_string.vyx", "unsupported_reference.vyx", "no_selection.vyx", "unproven_call.vyx",
                            "unproven_recursion.vyx",
                            "mixed_selected_method.vyx", "mixed_selected_module.vyx"):
                rejected_output(compiler, GATE / "fixtures" / fixture, work / (fixture + ".dcib"),
                                env, args.triplet)
            rejected_output(compiler, scalar, work / "run-combination.dcib", env, args.triplet, "--run=aot")
            write(cache / "verification.json", json.dumps({
                "compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
                "triplet": args.triplet, "result": "PASS", "file_aot": True, "project_aot": True,
                "canonical_roundtrip": True, "deterministic": True,
                "dependency_exports_filtered": True, "rejected_output_preserved": True,
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

"""Exercise native definition uniqueness and legal declaration aliases through AOT."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

GATE = Path(__file__).resolve().parent
FIXTURES = GATE / "fixtures"


def checked(command, cwd, env, *, expected=True):
    result = subprocess.run([str(arg) for arg in command], cwd=cwd, env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=120)
    print(result.stdout, end="", flush=True)
    if expected:
        if result.returncode:
            raise RuntimeError(f"command failed with {result.returncode}: {command}")
    elif result.returncode not in (1, 2) or "duplicate native function definition" not in result.stdout:
        raise RuntimeError(f"missing definition-collision diagnostic ({result.returncode}): {command}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--triplet", required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    env = dict(os.environ)
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    env.pop("VYX_PACKAGE_PROJECT", None)
    windows = "windows" in args.triplet
    cache = GATE / ".cache"
    cache.mkdir(exist_ok=True)
    results = []
    with tempfile.TemporaryDirectory(prefix="native-symbol-", dir=cache) as temporary:
        work = Path(temporary).resolve()
        if not work.is_relative_to(cache.resolve()):
            raise RuntimeError("gate workspace escaped its cache directory")
        try:
            for variant in ("declaration_first", "definition_first"):
                artifact = work / (variant + (".obj" if windows else ".o"))
                checked([compiler, "--emit=obj", "--src=file", FIXTURES / (variant + ".vyx"),
                         "--triplet", args.triplet, "-o", artifact], work, env)
                output = checked([compiler, "--run=aot", "--src=file", FIXTURES / "consumer.vyx",
                                  "--triplet", args.triplet, "--link-obj", artifact], work, env)
                if "native symbol declaration/definition PASS" not in output:
                    raise RuntimeError("declaration alias consumer assertions did not execute")
                results.append({"case": variant, "result": "PASS", "path": "independent object/AOT consumer"})

            producer = work / "producer"
            producer.mkdir()
            shutil.copyfile(FIXTURES / "declaration_first.vyx", producer / "producer.vyx")
            (producer / "Vyx.toml").write_text(
                '[package]\nname="native_alias_producer"\nversion="0.1.0"\n'
                '[build]\noutput_dir="out"\n[target.native_api]\ntype="static"\n'
                'entry="producer.vyx"\nsources=["producer.vyx"]\n', encoding="utf-8")
            checked([compiler, "build", "--target", "native_api", "--triplet", args.triplet, "-j2"],
                    producer, env)
            library = producer / "out" / ("native_api.lib" if windows else "libnative_api.a")
            output = checked([compiler, "--run=aot", "--src=file", FIXTURES / "consumer.vyx",
                              "--triplet", args.triplet, "--link-obj", library], work, env)
            if "native symbol declaration/definition PASS" not in output:
                raise RuntimeError("static-library alias consumer assertions did not execute")
            results.append({"case": "declaration_first_library", "result": "PASS", "path": "project static library/AOT consumer"})

            output = checked([compiler, "--run=aot", "--src=file", FIXTURES / "repeated_generic.vyx",
                              "--triplet", args.triplet], work, env)
            if "repeated closed generic PASS" not in output:
                raise RuntimeError("repeated generic assertions did not execute")
            results.append({"case": "repeated_generic", "result": "PASS", "path": "AOT"})

            for fixture in ("no_mangle_main", "export_main"):
                output = checked([compiler, "--run=aot", "--src=file", FIXTURES / (fixture + ".vyx"),
                                  "--triplet", args.triplet], work, env)
                if "explicit native main PASS" not in output:
                    raise RuntimeError("explicit native entry attributes did not execute")
                results.append({"case": fixture, "result": "PASS", "path": "explicit native entry AOT"})

            module_fixtures = FIXTURES / "module_mains"
            registry = work / "module-main-sources.txt"
            registry.write_text(str(module_fixtures / "left.vyx") + "\n"
                                + str(module_fixtures / "right.vyx") + "\n", encoding="utf-8")
            output = checked([compiler, "--run=aot", "--src=file", module_fixtures / "entry.vyx",
                              "--module-sources-file", registry,
                              "--triplet", args.triplet], work, env)
            if "module mains PASS" not in output:
                raise RuntimeError("imported module mains did not execute independently")
            results.append({"case": "imported_module_mains", "result": "PASS", "path": "single entry with two called source imports"})

            for parallel in (False, True):
                project = work / ("main-project-parallel" if parallel else "main-project-crate")
                source_dir = project / "src"
                source_dir.mkdir(parents=True)
                for name in ("entry.vyx", "left.vyx", "right.vyx"):
                    shutil.copyfile(module_fixtures / name, source_dir / name)
                (project / "Vyx.toml").write_text(
                    '[package]\nname="module_entry"\nversion="0.1.0"\nentry="src/entry.vyx"\n'
                    '[build]\noutput_dir="out"\n[target.module_entry]\ntype="executable"\n'
                    'entry="src/entry.vyx"\nauto_sources=false\n'
                    f'parallel_vyx={str(parallel).lower()}\n'
                    'sources=["src/left.vyx","src/right.vyx"]\n', encoding="utf-8")
                output = checked([compiler, "--src=project", project, "--run=aot",
                                  "--target", "module_entry", "--triplet", args.triplet, "-j2"], work, env)
                if "module mains PASS" not in output:
                    raise RuntimeError("project target entry and module-main assertions did not execute")
                results.append({"case": project.name, "result": "PASS", "path": "project AOT", "parallel_vyx": parallel})

            for kind in ("static", "staticlib", "shared", "dll"):
                library_project = work / ("module-main-library-" + kind)
                library_sources = library_project / "src"
                library_sources.mkdir(parents=True)
                for name in ("api.vyx", "left.vyx", "right.vyx"):
                    shutil.copyfile(module_fixtures / name, library_sources / name)
                (library_project / "Vyx.toml").write_text(
                    '[package]\nname="module_main_library"\nversion="0.1.0"\n'
                    '[build]\noutput_dir="out"\n[target.module_api]\n'
                    f'type="{kind}"\nauto_sources=false\nparallel_vyx=true\n'
                    'sources=["src/api.vyx","src/left.vyx","src/right.vyx"]\n', encoding="utf-8")
                if kind in ("static", "staticlib"):
                    checked([compiler, "build", "--target", "module_api", "--triplet", args.triplet, "-j2"],
                            library_project, env)
                    library = library_project / "out" / ("module_api.lib" if windows else "libmodule_api.a")
                    output = checked([compiler, "--run=aot", "--src=file", module_fixtures / "library_consumer.vyx",
                                      "--triplet", args.triplet, "--link-obj", library], work, env)
                    path = "project static library / independent AOT consumer"
                else:
                    # Build through an actual dependency, then execute using the
                    # consumer's deployed runtime. The producer's output is not
                    # added to PATH, so omitted DLL deployment cannot pass.
                    consumer_project = work / ("module-main-consumer-" + kind)
                    consumer_project.mkdir()
                    shutil.copyfile(module_fixtures / "library_consumer.vyx", consumer_project / "main.vyx")
                    (consumer_project / "Vyx.toml").write_text(
                        '[package]\nname="module_main_consumer"\nversion="0.1.0"\nentry="main.vyx"\n'
                        '[build]\noutput_dir="out"\n[dependencies]\n'
                        f'api={{path="../{library_project.name}",target="module_api"}}\n'
                        '[target.module_main_consumer]\ntype="executable"\nentry="main.vyx"\n'
                        'auto_sources=false\n', encoding="utf-8")
                    output = checked([compiler, "--src=project", consumer_project, "--run=aot",
                                      "--target", "module_main_consumer", "--triplet", args.triplet, "-j2"], work, env)
                    shared_name = "module_api.dll" if windows else "libmodule_api.so"
                    if not (consumer_project / "out" / shared_name).is_file():
                        raise RuntimeError(f"dependency runtime was not deployed: {kind} / {shared_name}")
                    path = "manifest shared dependency / deployed runtime AOT consumer"
                if "module mains library PASS" not in output:
                    raise RuntimeError(f"{kind} library module mains did not execute independently")
                results.append({"case": library_project.name, "result": "PASS", "target_type": kind, "path": path})

            for fixture in ("duplicate_export_name", "duplicate_after_extern",
                            "duplicate_no_mangle", "different_generic_args"):
                artifact = work / (fixture + (".obj" if windows else ".o"))
                checked([compiler, "--emit=obj", "--src=file", FIXTURES / (fixture + ".vyx"),
                         "--triplet", args.triplet, "-o", artifact], work, env, expected=False)
                if artifact.exists():
                    raise RuntimeError(f"rejected duplicate definition published an object: {artifact}")
                results.append({"case": fixture, "result": "PASS", "path": "normal diagnostic rejection"})

            (cache / "verification.json").write_text(json.dumps({
                "compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
                "triplet": args.triplet, "result": "PASS", "checks": results,
            }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        except Exception:
            failed = cache / "failed" / work.name
            failed.parent.mkdir(exist_ok=True)
            shutil.copytree(work, failed)
            print(f"native-symbol-collision failure workspace: {failed}", flush=True)
            raise
    print("native-symbol-collision: definition uniqueness, extern aliases, library AOT and generic checks PASS")


if __name__ == "__main__":
    main()

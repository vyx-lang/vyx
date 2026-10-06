"""Exercise manifest build/run through both public CLI forms."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--result", required=True, type=Path)
    parser.add_argument("--qt-project", type=Path)
    parser.add_argument("--qt-root", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    result = args.result.resolve()
    result.parent.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[3]
    checks = []
    env = os.environ.copy()
    # These must survive runtime forwarding literally, even if defined.
    env["VYX_LITERAL_ARG"] = "must-not-expand"

    def invoke(name, command, cwd, code=0, contains=(), environment=None):
        completed = subprocess.run([str(compiler), *command], cwd=cwd,
                                   env=environment or env, capture_output=True,
                                   text=True, encoding="utf-8", errors="replace", timeout=180)
        log = completed.stdout + completed.stderr
        (result.parent / (name + ".log")).write_text(log, encoding="utf-8")
        assert completed.returncode == code, (name, completed.returncode, log[-6000:])
        for expected in contains:
            assert expected in log, (name, expected, log[-6000:])
        checks.append({"name": name, "exit": completed.returncode,
                       "argv": command, "cwd": str(cwd)})
        return log

    with tempfile.TemporaryDirectory(prefix="project with spaces-", dir=result.parent) as temp:
        project = Path(temp)
        shutil.copytree(repo / "tests/projects/project_build_run", project, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns(".cache", "target", "build products", "native products"))
        argv = ["two words", "", 'say "hi"', 'C:\\trailing\\', '${VYX_LITERAL_ARG}',
                '%VYX_LITERAL_ARG%', '中文参数', '--target=not-a-build-target', '--src=file',
                '-j999', '-O3', '--version', '--help', '--']
        expected = [f"ARG:{i}:{len(value.encode('utf-8'))}:{value}" for i, value in enumerate(argv, 1)]
        manifest = (project / "Vyx.toml").read_text(encoding="utf-8")
        log = invoke("canonical-cold", ["--run=aot", "--src=project", str(project),
                                        "--target", "project_build_run", "-j2", "--", *argv],
                     repo, contains=["PROJECT_BUILD_RUN_OK", *expected])
        assert log.index("[run] ") < log.index("PROJECT_BUILD_RUN_OK"), "build output was not flushed before run"
        exe = project / "build products/selected app" / ("project_build_run.exe" if os.name == "nt" else "project_build_run")
        assert exe.is_file()
        first_mtime = exe.stat().st_mtime_ns
        invoke("canonical-warm-auto", ["--run=aot", "--src=project", "."], project, contains=["PROJECT_BUILD_RUN_OK"])
        assert exe.stat().st_mtime_ns == first_mtime, "warm run relinked the executable"
        invoke("build-shorthand-warm", ["build", "--run=aot"], project, contains=["PROJECT_BUILD_RUN_OK"])
        assert exe.stat().st_mtime_ns == first_mtime, "build shorthand changed the build plan"
        marker = project / "program-ran.txt"
        # A successful link must not hide a failed runtime asset deployment.
        # Force a real relink while retaining a valid predecessor executable.
        marker.unlink()
        predecessor = hashlib.sha256(exe.read_bytes()).hexdigest()
        asset_root = project / "native_lib/native products/runtime-data"
        asset_root.mkdir()
        (asset_root / "config.txt").write_text("deployed asset", encoding="utf-8")
        blocked_asset = exe.parent / "runtime-data"
        blocked_asset.write_text("a file blocks the asset directory", encoding="utf-8")
        values = project / "src/values.vyx"
        original_values = values.read_text(encoding="utf-8")
        values.write_text(original_values + "\n// Force deployment regression relink.\n", encoding="utf-8")
        invoke("runtime-deployment-failure", ["--src=project", str(project), "--run=aot"], repo,
               code=1, contains=["failed to copy dependency runtime assets"])
        assert not marker.exists(), "deployment failure ran the predecessor executable"
        assert hashlib.sha256(exe.read_bytes()).hexdigest() == predecessor, "deployment failure did not restore the predecessor"
        blocked_asset.unlink()
        invoke("runtime-deployment-recover", ["--src=project", str(project), "--run=aot"], repo,
               contains=["PROJECT_BUILD_RUN_OK"])
        assert (blocked_asset / "config.txt").read_text(encoding="utf-8") == "deployed asset"
        # Keep the compiled source identity for subsequent warm-cache checks.
        first_mtime = exe.stat().st_mtime_ns
        marker.unlink()
        ir = project / "build products/selected app/project_build_run.ll"
        invoke("project-ir", ["--emit=ir", "--src=project", str(project), "--target=project_build_run", "-j2"], repo)
        assert ir.is_file() and "define i32 @main(" in ir.read_text(encoding="utf-8")
        module_ir = (project / ".cache/module_fixture_values.ll").read_text(encoding="utf-8")
        definitions = re.findall(r"^define\s+[^\n]*?(@[^\s(]+)\(", module_ir, re.MULTILINE)
        assert definitions and all(symbol + "(" in ir.read_text(encoding="utf-8") for symbol in definitions), \
            "IR omitted definitions from a separate Vyx module"
        assert "PROJECT_BUILD_RUN_OK" in ir.read_text(encoding="utf-8")
        assert not marker.exists(), "IR emission ran the executable"
        assert exe.stat().st_mtime_ns == first_mtime, "IR emission linked the executable"
        ir_mtime = ir.stat().st_mtime_ns
        invoke("build-ir-warm", ["build", "--emit=ir", "--target=project_build_run", "-j2"], project)
        assert ir.stat().st_mtime_ns == ir_mtime, "warm IR output was rewritten"
        ir_text = ir.read_bytes()
        ir.unlink()
        invoke("project-ir-recover", ["--emit=ir", "--src=project", str(project), "--target=project_build_run", "-j2"], repo)
        assert ir.read_bytes() == ir_text, "missing IR output was not reconstructed from cache"
        ir.write_text("damaged IR", encoding="utf-8")
        invoke("project-ir-tamper", ["--emit=ir", "--src=project", str(project), "--target=project_build_run", "-j2"], repo)
        assert ir.read_bytes() == ir_text, "damaged IR artifact was incorrectly cached"
        # Explicit output paths are relative to the invoking directory, even
        # when the project selector changes the builder's working directory.
        custom_ir = result.parent / "custom output/secondary.ll"
        output_arg = os.path.relpath(custom_ir, repo)
        invoke("project-ir-output", ["--src=project", str(project), "--emit=ir", "--target=secondary",
                                     "-o", output_arg, "-j1", "-O1"], repo)
        text = custom_ir.read_text(encoding="utf-8")
        assert "SECONDARY_OK" in text and "PROJECT_BUILD_RUN_OK" not in text
        assert not (project / output_arg).is_file(), "output path was resolved from project instead of caller"
        obj = result.parent / "custom output/secondary.obj"
        invoke("project-object", ["--emit=obj", "--src=project", str(project), "--target=secondary", "-o=" + str(obj)], repo)
        assert obj.is_file() and obj.stat().st_size > 0
        invoke("project-object-set", ["--emit=obj", "--src=project", str(project), "--target=project_build_run", "-j2"], repo)
        object_set = project / "build products/selected app/project_build_run.objects"
        assert len(list(object_set.glob("*.obj" if os.name == "nt" else "*.o"))) >= 2
        invoke("ir-run-rejected", ["--emit=ir", "--src=project", str(project), "--run=aot"], repo,
               code=1, contains=["cannot execute"])
        invoke("emit-target-kind-rejected", ["--src=project", str(project), "--emit=lib", "--target=secondary"], repo,
               code=1, contains=["does not match"])
        # Turn the dependency into an external, prebuilt archive. Changing
        # only that archive must relink even with its size/mtime preserved.
        external_manifest = manifest.replace(
            '[dependencies]\nnative_bits = { path = "native_lib", target = "native_bits" }', "")
        external_manifest = external_manifest.replace(
            '[target.project_build_run]',
            '[target.project_build_run]\nlib_paths = ["native_lib/native products"]\nlibs = ["native_bits"]')
        (project / "Vyx.toml").write_text(external_manifest, encoding="utf-8")
        invoke("prebuilt-library-baseline", ["build", "--run=aot"], project, contains=["PROJECT_BUILD_RUN_OK"])
        native = project / "native_lib"
        answer = native / "answer.cpp"
        original_answer = answer.read_text(encoding="utf-8")
        archive = native / "native products" / ("native_bits.lib" if os.name == "nt" else "libnative_bits.a")
        original_archive = archive.stat()
        object_files = list((project / ".cache").glob("*.obj" if os.name == "nt" else "*.o"))
        objects_before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in object_files}
        answer.write_text(original_answer.replace("42", "43"), encoding="utf-8")
        invoke("prebuilt-library-change", ["build", "--target", "native_bits"], native)
        assert archive.stat().st_size == original_archive.st_size, "archive edit changed size"
        os.utime(archive, ns=(original_archive.st_atime_ns, original_archive.st_mtime_ns))
        invoke("prebuilt-library-relink", ["build", "--run=aot"], project, code=92)
        assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == digest
                   for p, digest in objects_before.items()), "archive-only change recompiled application objects"
        answer.write_text(original_answer, encoding="utf-8")
        invoke("prebuilt-library-restore", ["build", "--target", "native_bits"], native)
        os.utime(archive, ns=(original_archive.st_atime_ns, original_archive.st_mtime_ns))
        invoke("prebuilt-library-restored-run", ["build", "--run=aot"], project, contains=["PROJECT_BUILD_RUN_OK"])
        (project / "Vyx.toml").write_text(manifest, encoding="utf-8")
        invoke("project-arguments", ["--src=project", str(project), "--run=aot",
                                       "--target=project_build_run", "--", *argv], repo,
               contains=["PROJECT_BUILD_RUN_OK", *expected])
        invoke("build-project-selector", ["build", "--src=project", str(project), "--run=aot",
                                          "--target", "secondary"], repo, contains=["SECONDARY_OK"])
        custom_exe = result.parent / "custom output/selected.exe"
        invoke("project-executable-output", ["--src=project", str(project), "--run=aot", "--target=secondary",
                                             "-o", str(custom_exe)], repo, contains=["SECONDARY_OK"])
        assert custom_exe.is_file()
        invoke("program-exit", ["build", "--run=aot", "--target=project_build_run", "--", "exit7"], project, code=7)
        artifact = project / "debug artifact.txt"
        invoke("debug-artifact", ["build", "--target", "project_build_run", "--artifact-file", str(artifact)], project)
        assert Path(artifact.read_text(encoding="utf-8").rstrip("\r\n")) == exe
        invoke("missing-target", ["build", "--run=aot", "--target", "absent"], project, code=1,
               contains=["manifest target not found"])
        invoke("library-not-runnable", ["build", "--run=aot", "--target", "native_bits"], project / "native_lib", code=1,
               contains=["not an executable"])
        invoke("library-auto", ["build", "--run=aot"], project / "native_lib", code=1,
               contains=["no executable target"])
        invoke("jit-rejected", ["build", "--run=jit"], project, code=1, contains=["supports --run=aot only"])
        cross = "aarch64-unknown-linux-gnu" if os.name == "nt" else "x86_64-pc-windows-msvc"
        invoke("cross-run-rejected", ["build", "--run=aot", "--triplet", cross], project, code=1,
               contains=["cannot run a cross-compiled target"])
        (project / "Vyx.toml").write_text(manifest.replace('name = "project_build_run"', 'name = "ambiguous"', 1), encoding="utf-8")
        invoke("ambiguous-auto", ["build", "--run=aot"], project, code=1, contains=["multiple executable targets"])
        (project / "Vyx.toml").write_text(manifest.replace('name = "project_build_run"', 'name = "secondary"', 1), encoding="utf-8")
        invoke("auto-package-target", ["build", "--run=aot"], project, contains=["SECONDARY_OK"])
        (project / "Vyx.toml").write_text(manifest, encoding="utf-8")
        (project / "program-ran.txt").unlink()
        source = project / "src/main.vyx"
        source.write_text("fn main() -> i32 { return missing_symbol; }", encoding="utf-8")
        invoke("failed-build-no-stale-run", ["build", "--run=aot", "--target", "project_build_run"], project, code=1,
               contains=["missing_symbol"])
        assert not (project / "program-ran.txt").exists(), "failed build executed stale output"
        single_dir = project / "single file"
        single_dir.mkdir()
        single = single_dir / "argv.vyx"
        single.write_text('''use std.string;
            fn main() -> i32 { var i: i32 = 1; while (i < argCount()) {
                let arg = getArg(i);
                print("ARG:" + i.toString() + ":" + arg.len.toString() + ":" + arg);
                i = i + 1; } return 0; }''', encoding="utf-8")
        invoke("single-file-argv", ["--src=file", str(single), "--run=aot", "--", *argv], single_dir, contains=expected)

    if args.qt_project:
        assert args.qt_root, "--qt-root is required for Qt execution"
        qt = args.qt_root.resolve()
        qt_env = env.copy()
        qt_env.update(DCI_QT_COUNTER_SELFTEST="1", QT_QPA_PLATFORM="offscreen",
                      QT_PLUGIN_PATH=str(qt / "plugins"), QTDIR=str(qt),
                      DCI_QT_CXX=str(repo / "clang/bin/clang++.exe" if os.name == "nt" else repo / "clang/bin/clang++"))
        qt_env["PATH"] = str(qt / "bin") + os.pathsep + qt_env.get("PATH", "")
        qt_project = args.qt_project.resolve()
        before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in
                  (qt_project / "contracts/qt_widgets.dcib", qt_project / "src/qt_widgets.vyx")}
        invoke("qt-project-ir", ["--emit=ir", "--src=project", "."], qt_project,
               environment=qt_env, contains=["dci_qt_counter.ll"])
        assert (qt_project / "target/dci_qt_counter.ll").is_file()
        assert all(hashlib.sha256(p.read_bytes()).hexdigest() == digest for p, digest in before.items())
        log = invoke("qt-canonical", ["--run=aot", "--src=project", ".", "--target", "dci_qt_counter"], qt_project,
                     environment=qt_env, contains=["buttons, held press, LCD, virtual timer"])
        assert log.index("[run] ") < log.index("dci-qt-counter OK"), "Qt output overtook the build log"
        invoke("qt-project-outside", ["--src=project", str(qt_project), "--run=aot", "--target=dci_qt_counter"],
               repo, environment=qt_env, contains=["buttons, held press, LCD, virtual timer"])
    result.write_text(json.dumps({"compiler": str(compiler),
                                 "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
                                 "checks": checks}, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"PASS: {len(checks)} project build/run checks")


if __name__ == "__main__":
    main()

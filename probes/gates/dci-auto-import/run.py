"""Build/run automatic imports, cache repair and ABI input invalidation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--result-dir", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    compiler = args.compiler.resolve()
    output = args.result_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Every run owns a fresh directory; never delete the user's project cache.
    import tempfile
    project = Path(tempfile.mkdtemp(prefix="project-", dir=output))
    fixture = repo / "tests/projects/dci_auto_import"
    for name in ("Vyx.toml", "src/main.vyx", "native/session.hpp", "native/session.cpp"):
        destination = project / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(fixture / name, destination)
    env = dict(os.environ, LLVM_ROOT=str(repo / "clang"), VYX_DCI_TOOLS=str(repo / "tools/dci"))
    checks, commands = [], []
    command = [str(compiler), "build", "--run=aot", "--target", "dci_auto_import", "-j1"]
    def build(name, expected=True):
        result = subprocess.run(command, cwd=project, env=env, capture_output=True,
                                text=True, errors="replace", timeout=120)
        log = result.stdout + result.stderr
        (output / (name + ".log")).write_text(log, encoding="utf-8")
        commands.append({"name": name, "argv": command, "cwd": str(project), "exit": result.returncode})
        assert (result.returncode == 0) == expected, (name, result.returncode, log)
        if expected:
            assert "dci automatic import OK" in log, (name, log)
        else:
            assert "[run]" not in log and "dci automatic import OK" not in log, log
        checks.append(name)
        return log
    build("cold")
    cache = project / ".cache/dci/session"
    contract = project / "contracts/session.dcib"
    report = json.loads((cache / "report.json").read_text())
    assert report["export_scope"]["types"] == ["NativeSession"], report
    assert not report["bridges"], report
    definitions = (cache / "import.vyx").read_text()
    assert "initial: i32 = 11" in definitions and "multiplier: i32 = 2" in definitions, definitions
    assert "producer.hpp" not in definitions and "__vyx_dci_" not in definitions, definitions
    products = [contract] + [cache / n for n in ("import.vyx", "producer.hpp", "producer.cpp")]
    assert 'dci_import("../../../contracts/session.dcib")' in definitions
    before = {p.name: (sha(p), p.stat().st_mtime_ns) for p in products}
    assert "cached verified import" in build("warm")
    assert before == {p.name: (sha(p), p.stat().st_mtime_ns) for p in products}
    (cache / "inputs.json").write_text("broken disposable cache metadata")
    log = build("malformed-cache-metadata-repair")
    assert "using prepared contract" in log and "extracting producer API" not in log
    assert before == {p.name: (sha(p), p.stat().st_mtime_ns) for p in products}
    source = project / "src/main.vyx"
    original = source.read_text()
    source.write_text(original.replace("1000", "1001").replace("2011", "2013").replace("2023", "2025"))
    assert "cached verified import" in build("source-only")
    assert before == {p.name: (sha(p), p.stat().st_mtime_ns) for p in products}
    header = project / "native/session.hpp"
    native = header.read_bytes()
    times = header.stat()
    header.write_bytes(native.replace(b"multiplier = 2", b"multiplier = 3"))
    os.utime(header, ns=(times.st_atime_ns, times.st_mtime_ns))
    source.write_text(original.replace("2011", "3011").replace("2023", "3023"))
    assert "extracting producer API" in build("same-size-same-mtime-default-change")
    assert header.stat().st_size == times.st_size
    (cache / "producer.hpp").unlink()
    assert "using prepared contract" in build("missing-generated-header-repair")
    (cache / "import.vyx").write_text("invalid generated source")
    assert "using prepared contract" in build("tampered-generated-module-repair")
    header.write_text(header.read_text() + "\nclass NativeExtra { public: NativeExtra(); ~NativeExtra(); int read() const; };\n")
    implementation = project / "native/session.cpp"
    implementation.write_text(implementation.read_text() + "\nNativeExtra::NativeExtra() {}\nNativeExtra::~NativeExtra() {}\nint NativeExtra::read() const { return 41; }\n")
    source.write_text(source.read_text().replace('    print(', '    var extra = NativeExtra();\n    if (extra.read() != 41) { return 3; }\n    print('))
    build("consumer-usage-cannot-expand-export-scope", expected=False)
    assert "class NativeExtra" not in (cache / "import.vyx").read_text()
    manifest_path = project / "Vyx.toml"
    manifest_path.write_text(manifest_path.read_text().replace('export_types = ["NativeSession"]',
                                                              'export_types = ["NativeSession", "NativeExtra"]'))
    assert "extracting producer API" in build("author-adds-export-type")
    assert "class NativeExtra" in (cache / "import.vyx").read_text()
    saved = header.read_text()
    header.write_text(saved + "\nthis is not valid C++;\n")
    build("invalid-producer-prevents-stale-run", expected=False)
    header.write_text(saved)
    build("producer-recovery")
    # Resolve build defaults, inherited templates and platform arrays once in
    # vyxc. Both adapter extraction and native compilation require this define.
    header.write_text(header.read_text() + "\n#ifndef VYX_AUTO_CONFIG\n#error missing effective build configuration\n#endif\n")
    manifest = manifest_path.read_text().replace('dci_imports = ["session"]\n', "")
    manifest = manifest.replace('threads = 1', 'threads = 1\ndci_imports = ["session"]')
    manifest = manifest.replace('[target.dci_auto_import]', '[target.dci_auto_import]\nextends = ["native_api"]')
    native_config = '\n'.join(line for line in manifest.splitlines()
                              if line.startswith(('sources =', 'cxx =', 'cxxflags =', 'include_paths =')))
    manifest = '\n'.join(line for line in manifest.splitlines()
                         if not line.startswith(('sources =', 'cxx =', 'cxxflags =', 'include_paths =')))
    platform = "windows" if os.name == "nt" else "linux"
    native_config = native_config.replace('cxxflags = ["-std=c++17"]',
        f'cxxflags_{platform} = ["-std=c++17", "-DVYX_AUTO_CONFIG=1"]')
    native_config = native_config.replace('include_paths =', f'include_paths_{platform} =')
    manifest_path.write_text(manifest + '\n\n[template.native_api]\n' + native_config + '\n')
    build("effective-template-platform-and-build-defaults")

    # A maintained Converter file is an input, while producer facts remain
    # disposable build artifacts. Do not compile a second generated module.
    authored = project / "src/native_api.vyx"
    authored.write_text((cache / "import.vyx").read_text().replace(
        'dci_import("../../../contracts/session.dcib")', 'dci_import("../contracts/session.dcib")')
        .replace("initial: i32", "starting: i32") + "\n// Maintained by the application author.\n")
    authored_bytes = authored.read_bytes()
    manifest_path.write_text(manifest_path.read_text().replace(
        '[dci.import.session]', '[dci.import.session]\ndefinitions = "src/native_api.vyx"'))
    build("authored-definitions-switch")
    assert authored.read_bytes() == authored_bytes
    assert not (cache / "import.vyx").exists()
    products = [contract] + [cache / n for n in ("producer.hpp", "producer.cpp")]
    before = {p.name: (sha(p), p.stat().st_mtime_ns) for p in products}
    assert "cached verified import" in build("authored-definitions-warm")
    assert before == {p.name: (sha(p), p.stat().st_mtime_ns) for p in products}
    disposable = (project / ".cache").resolve()
    assert disposable.parent == project.resolve()
    shutil.rmtree(disposable)
    assert contract.is_file()
    log = build("authored-definitions-entire-cache-deleted")
    assert "using prepared contract" in log and "extracting producer API" not in log and "ast-dump" not in log
    assert authored.read_bytes() == authored_bytes
    assert not (cache / "import.vyx").exists()
    contract.unlink()
    assert "extracting producer API" in build("missing-project-contract-repair")
    assert authored.read_bytes() == authored_bytes
    before = {p.name: (sha(p), p.stat().st_mtime_ns) for p in products}
    authored.write_bytes(authored_bytes + b"\n// Local definition edit.\n")
    assert "cached verified import" in build("authored-definition-edit-reuses-producer-facts")
    assert before == {p.name: (sha(p), p.stat().st_mtime_ns) for p in products}
    authored.unlink()
    assert "authored definitions not found" in build("missing-authored-definitions-rejected", expected=False)
    assert not authored.exists()
    authored.write_bytes(b"this is not valid Vyx;\n")
    build("invalid-authored-definitions-not-overwritten", expected=False)
    assert authored.read_bytes() == b"this is not valid Vyx;\n"
    authored.write_bytes(authored_bytes)
    build("authored-definitions-recovery")

    # A genuine standalone Adapter output has no preparation cache or build
    # provenance extension. Consume it without re-extraction or rewriting it.
    adapter_command = [os.environ.get("VYX_DCI_PYTHON", "python"), str(repo / "tools/dci/dci.py"),
                       "adapter", "--language", "cpp", str(header), "--toolchain", "clang",
                       "--compiler", str(repo / "clang/bin/clang++.exe"),
                       "--extractor", str(repo / "clang/bin/clang++.exe"),
                       "--triplet", "windows_x64", "--std", "c++17", "--boundary", "shared_abi",
                       "--export-type", "NativeSession", "--export-type", "NativeExtra",
                       "--extractor-arg=-DVYX_AUTO_CONFIG=1", "-o", str(contract)]
    completed = subprocess.run(adapter_command, cwd=project, env=env, capture_output=True, text=True,
                               errors="replace", timeout=120)
    (output / "offline-adapter.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
    assert completed.returncode == 0, completed.stdout + completed.stderr
    commands.append({"name": "offline-adapter", "argv": adapter_command, "cwd": str(project), "exit": completed.returncode})
    offline_bytes, offline_mtime = contract.read_bytes(), contract.stat().st_mtime_ns
    assert disposable.parent == project.resolve()
    shutil.rmtree(disposable)
    log = build("standalone-offline-contract-without-cache")
    assert "using prepared contract" in log and "extracting producer API" not in log and "ast-dump" not in log
    assert contract.read_bytes() == offline_bytes and contract.stat().st_mtime_ns == offline_mtime
    contract.write_bytes(b"not a DCI contract")
    build("corrupt-supplied-contract-not-overwritten", expected=False)
    assert contract.read_bytes() == b"not a DCI contract"
    contract.write_bytes(offline_bytes)
    build("supplied-contract-recovery")
    report = {"status": "passed", "compiler": str(compiler), "compiler_sha256": sha(compiler),
              "project": str(project), "checks": checks, "commands": commands}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Automatic DCI import gate OK: {len(checks)} build/run checks")


if __name__ == "__main__":
    main()

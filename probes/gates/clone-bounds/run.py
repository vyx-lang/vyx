"""Closed trait bounds on container and forwarded generic calls (native AOT)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    out = gate / ".cache"
    out.mkdir(exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    env["VYX_STD_PACKAGES"] = str(repo / "bootstrap_compiler/std_packages")
    env["VYX_RUNTIME_ROOT"] = str(compiler.parent)
    records = []

    def invoke(name, command, *, reject=False, stdout=None, source=None, artifact=None,
               diagnostic="E1100: trait bound not satisfied"):
        result = subprocess.run([str(x) for x in command], env=env, cwd=repo,
                                capture_output=True, timeout=90)
        text = (result.stdout + result.stderr).decode(errors="replace")
        passed = result.returncode == (1 if reject else 0)
        if stdout is not None:
            passed &= result.stdout.replace(b"\r\n", b"\n") == stdout
        if reject:
            passed &= diagnostic in text
            passed &= "I0100" not in text and "I0001" not in text
            passed &= source.name + ":" in text
            passed &= not artifact.exists()
        records.append({"name": name, "command": [str(x) for x in command],
                        "exit": result.returncode, "stdout": result.stdout.decode(errors="replace"),
                        "stderr": result.stderr.decode(errors="replace"), "pass": bool(passed)})
        print(name, "PASS" if passed else "FAIL", flush=True)
        if not passed:
            print(text, flush=True)
        return passed

    preamble = """use std.ref;
use std.collections;
use std.clone;
class Unique { public value: i64; }
"""
    cases = {
        "box-get": 'let v = Box::<Unique>.new(Unique { value: 5 }); let bad = v.get();',
        "ref-get": 'let v = Ref::<Unique>.new(Unique { value: 5 }); let bad = v.get();',
        "box-clone": 'let v = Box::<Unique>.new(Unique { value: 5 }); let bad = v.clone();',
        "vec-get": 'var v = Vec::<Unique>.new(); v.push(Unique { value: 5 }); let bad = v.get(0);',
        "vec-unchecked": 'var v = Vec::<Unique>.new(); v.push(Unique { value: 5 }); let bad = v.get_unchecked(0);',
        "vec-clone": 'var v = Vec::<Unique>.new(); v.push(Unique { value: 5 }); let bad = v.clone();',
        "nested-payload": 'let v = Box::<Vec<Unique>>.new(Vec::<Unique>.new()); let bad = v.get();',
    }
    sources = {name: preamble + "fn main() -> i32 {\n    " + body + "\n    return 0;\n}\n"
               for name, body in cases.items()}
    sources["generic-forward"] = preamble + """
fn read<T>(v: &Box<T>) -> T { return v.get(); }
fn main() -> i32 {
    let v = Box::<Unique>.new(Unique { value: 5 });
    let bad = read::<Unique>(&v);
    return 0;
}
"""
    sources["generic-method"] = preamble + """
class Cell<T> {
    public value: T;
    public fn read<U>(&self, value: &U) -> U where U: Clone { return value.clone(); }
}
fn main() -> i32 {
    let cell = Cell::<i32> { value: 7 };
    let value = Unique { value: 5 };
    let bad = cell.read::<Unique>(&value);
    return 0;
}
"""
    sources["lazy-method"] = preamble + """
class Reader {
    @[lazy]
    public fn read<T>(&self, value: &Box<T>) -> T { return value.get(); }
}
fn main() -> i32 {
    let reader = Reader {};
    let owner = Box::<Unique>.new(Unique { value: 5 });
    let bad = reader.read::<Unique>(&owner);
    return 0;
}
"""
    sources["nested-owner-field"] = preamble + """
class Reader<T> {
    public value: Box<T>;
    @[lazy]
    public fn read(&self) -> T { return self.value.get(); }
}
fn main() -> i32 {
    let reader = Reader::<Unique> { value: Box::<Unique>.new(Unique { value: 5 }) };
    let bad = reader.read();
    return 0;
}
"""
    sources["unrelated-trait"] = """interface Measure { fn measure(&self) -> i64; }
class Unique { public value: i64; }
class Cell<T> {
    public value: T;
    public fn measure(&self) -> i64 where T: Measure { return self.value.measure(); }
}
fn main() -> i32 {
    let v = Cell::<Unique> { value: Unique { value: 5 } };
    return v.measure() as i32;
}
"""

    for name, text in sources.items():
        source = out / (name + ".vyx")
        source.write_text(text, encoding="utf-8")
        for optimization in (0, 2):
            artifact = out / (f"{name}-O{optimization}" + (".exe" if os.name == "nt" else ""))
            artifact.unlink(missing_ok=True)
            invoke(f"{name}-O{optimization}", [compiler, "--src=file", source,
                   "--emit=exe", f"-O{optimization}", "-o", artifact], reject=True,
                   source=source, artifact=artifact)

    mismatch = out / "nested-field-mismatch.vyx"
    mismatch.write_text('use std.ref;\nclass Holder<T> { public owner: Box<T>; }\n'
                        'fn main() -> i32 {\n'
                        '    let value = Holder::<i64> { owner: Box::<i32>.new(7) };\n'
                        '    return 0;\n}\n', encoding="utf-8")
    for optimization in (0, 2):
        artifact = out / (f"nested-field-mismatch-O{optimization}" + (".exe" if os.name == "nt" else ""))
        artifact.unlink(missing_ok=True)
        invoke(f"nested-field-mismatch-O{optimization}", [compiler, "--src=file", mismatch,
               "--emit=exe", f"-O{optimization}", "-o", artifact], reject=True,
               source=mismatch, artifact=artifact, diagnostic="E1000: struct field")

    project = out / "project-forward"
    project.mkdir(exist_ok=True)
    (project / "Vyx.toml").write_text(
        '[package]\nname="clone_bounds"\nversion="0.1.0"\n'
        '[target.app]\ntype="exe"\nentry="main.vyx"\nauto_sources=false\n'
        'sources=["main.vyx", "api.vyx"]\n', encoding="utf-8")
    api = project / "api.vyx"
    api.write_text('module clone_api;\nuse std.ref;\n'
                   'public class Unique { public value: i64; }\n'
                   'public fn read<T>(v: &Box<T>) -> T { return v.get(); }\n', encoding="utf-8")
    (project / "main.vyx").write_text(
        'use clone_api;\nuse std.ref;\nfn main() -> i32 {\n'
        '    let v = Box::<Unique>.new(Unique { value: 5 });\n'
        '    let bad = read::<Unique>(&v);\n    return 0;\n}\n', encoding="utf-8")
    for optimization in (0, 2):
        artifact = project / (f"app-O{optimization}" + (".exe" if os.name == "nt" else ""))
        artifact.unlink(missing_ok=True)
        invoke(f"project-forward-O{optimization}", [compiler, "--src=project", project,
               "--target", "app", "--emit=exe", f"-O{optimization}", "-o", artifact, "-j1"],
               reject=True, source=api, artifact=artifact)

    # Verify ownership and a real Clone implementation, at both optimization levels.
    positives = {gate / "borrow_only.vyx": b"borrow-only=ok\n",
                 gate / "generic_bounds.vyx": b"generic-bounds=ok\n",
                 gate / "structural_clone.vyx": b"structural-clone=ok\n",
                 gate.parent / "full-expression/clone_reference_values.vyx": b"clone-reference-values=ok\n"}
    for source, expected in positives.items():
        for optimization in (0, 2):
            name = f"{source.stem}-O{optimization}"
            artifact = out / (name + (".exe" if os.name == "nt" else ""))
            artifact.unlink(missing_ok=True)
            if invoke(name + "-compile", [compiler, "--src=file", source, "--emit=exe",
                                         f"-O{optimization}", "-o", artifact]):
                invoke(name + "-run", [artifact], stdout=expected)

    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "records": records, "passed": sum(r["pass"] for r in records), "checks": len(records)}
    (out / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f'{report["passed"]}/{report["checks"]} PASS', flush=True)
    return 0 if all(r["pass"] for r in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())

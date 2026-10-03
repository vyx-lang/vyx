"""Replay the compiler environment selected by Cargo, without reconstructing it.

Cargo owns dependency resolution, feature/cfg selection, proc macros, build
scripts and OUT_DIR.  A small native rustc wrapper records the selected package's
actual invocation; adapter probes reuse those arguments and that environment.
No library names, feature lists or dependency search paths are guessed here.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import shutil
from typing import Sequence


class CargoError(RuntimeError):
    pass


class CargoRustcArgs(list[str]):
    def __init__(self, args: Sequence[str], environment: dict[str, str]):
        super().__init__(args)
        self.environment = environment


_WRAPPER = r'''
use std::{env, fs, process::{Command, exit}};
fn quote(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""), '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"), '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"), c if c < ' ' => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"'); out
}
fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    let selected = env::var("VYX_DCI_CAPTURE_CRATE").unwrap();
    let matches = args.windows(2).any(|a| a[0] == "--crate-name" && a[1] == selected);
    if matches {
        let arguments = args.iter().skip(1).map(|a| quote(a)).collect::<Vec<_>>().join(",");
        let environment = env::vars().map(|(k, v)| format!("{}:{}", quote(&k), quote(&v))).collect::<Vec<_>>().join(",");
        let data = format!("{{\"rustc\":{},\"arguments\":[{}],\"environment\":{{{}}},\"cwd\":{}}}", quote(&args[0]), arguments, environment, quote(&env::current_dir().unwrap().to_string_lossy()));
        fs::write(env::var("VYX_DCI_CAPTURE_FILE").unwrap(), data).unwrap();
    }
    let status = Command::new(&args[0]).args(&args[1..]).status().unwrap();
    exit(status.code().unwrap_or(1));
}
'''


def _run(command: list[str], *, environment: dict[str, str] | None = None,
         cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, env=environment, cwd=cwd, capture_output=True,
                            text=True, encoding="utf-8", errors="replace", timeout=600)
    if result.returncode:
        raise CargoError(f"Cargo producer command failed ({result.returncode}):\n"
                         f"{result.stderr.strip() or result.stdout.strip()}")
    return result


class CargoContext:
    """A selected Cargo library plus its complete measured compile environment."""

    def __init__(self, manifest: Path, *, package: str | None, rustc: str,
                 target: str, features: Sequence[str] = (),
                 no_default_features: bool = False, offline: bool = False,
                 locked: bool = False, work_root: Path | None = None):
        self.manifest = Path(manifest).resolve()
        if not self.manifest.is_file():
            raise CargoError(f"Cargo manifest not found: {self.manifest}")
        self._temp = None
        if work_root is None:
            self._temp = tempfile.TemporaryDirectory(prefix="vyx-dci-cargo-")
            self.work = Path(self._temp.name)
        else:
            self.work = Path(work_root).resolve()
            self.work.mkdir(parents=True, exist_ok=True)
        options = ["--manifest-path", str(self.manifest)]
        if offline:
            options.append("--offline")
        if locked:
            options.append("--locked")
        if features:
            options.extend(["--features", ",".join(features)])
        if no_default_features:
            options.append("--no-default-features")
        environment = dict(os.environ, RUSTC=rustc)
        metadata = json.loads(_run(["cargo", "metadata", "--format-version", "1",
                                    *options], environment=environment).stdout)
        if package:
            selected = [p for p in metadata["packages"]
                        if p["name"] == package or p["id"] == package]
        else:
            selected = [p for p in metadata["packages"]
                        if Path(p["manifest_path"]).resolve() == self.manifest]
        if len(selected) != 1:
            raise CargoError(f"Cargo package selection {package!r} is ambiguous or absent; "
                             "select its exact package ID with --package")
        self.package = selected[0]
        targets = [t for t in self.package["targets"] if "lib" in t["kind"]]
        if len(targets) != 1:
            raise CargoError("selected Cargo package must have exactly one library target")
        self.root = Path(targets[0]["src_path"]).resolve()
        self.crate_name = targets[0]["name"].replace("-", "_")
        self.edition = str(self.package["edition"])
        wrapper_source = self.work / "capture.rs"
        wrapper_source.write_text(_WRAPPER, encoding="utf-8")
        wrapper = self.work / ("capture.exe" if os.name == "nt" else "capture")
        _run([rustc, "--edition", "2021", str(wrapper_source), "-o", str(wrapper)])
        capture = self.work / "invocation.json"
        environment.update(RUSTC_WRAPPER=str(wrapper),
                           VYX_DCI_CAPTURE_CRATE=self.crate_name,
                           VYX_DCI_CAPTURE_FILE=str(capture))
        # rustc, rather than build, guarantees the selected package is invoked
        # even when its dependencies already exist in the Cargo cache.
        _run(["cargo", "rustc", *options, "--package", self.package["id"], "--lib",
              "--target", target, "--target-dir", str(self.work / "target"),
              "--", "--emit=llvm-ir,metadata,link"], environment=environment)
        if not capture.is_file():
            raise CargoError("Cargo did not invoke the selected package's rustc")
        invocation = json.loads(capture.read_text(encoding="utf-8"))
        arguments = self._replay_arguments(invocation["arguments"])
        self.environment = invocation["environment"]
        # The adapter itself invokes rustc; recursively using its wrapper is
        # unnecessary and would overwrite the recorded Cargo authority.
        for name in ("RUSTC_WRAPPER", "VYX_DCI_CAPTURE_CRATE", "VYX_DCI_CAPTURE_FILE",
                     "CARGO_MAKEFLAGS", "MAKEFLAGS", "MFLAGS"):
            self.environment.pop(name, None)
        self.args = CargoRustcArgs(arguments, self.environment)
        resolve = metadata.get("resolve") or {}
        self.provenance = {
            "manifest_path": str(self.manifest), "package_id": self.package["id"],
            "name": self.package["name"], "version": self.package["version"],
            "source": self.package.get("source"),
            "features": next((n["features"] for n in resolve.get("nodes", [])
                              if n["id"] == self.package["id"]), []),
            "packages": [{"id": p["id"], "source": p.get("source"),
                          "manifest_sha256": _digest(Path(p["manifest_path"]))}
                         for p in metadata["packages"]],
            "lock_sha256": _digest(Path(metadata["workspace_root"]) / "Cargo.lock"),
        }

    def declarations(self, rustc: str, target: str):
        """Ask the pinned compiler for expanded syntax and reachable public APIs.

        Rust's JSON documentation and expanded syntax are versioned producer
        interfaces.  RUSTC_BOOTSTRAP enables those interfaces on the selected
        toolchain; changing or lacking them is a hard error, never a fallback to
        unexpanded macro/feature guesses.
        """
        environment = dict(self.environment, RUSTC_BOOTSTRAP="1")
        expanded = _run([rustc, str(self.root), "--crate-name", self.crate_name,
                         "--crate-type", "lib", "--edition", self.edition,
                         "--target", target, "-Z", "unpretty=expanded", *self.args],
                        environment=environment, cwd=self.root.parent).stdout
        path = self.work / "expanded.rs"
        path.write_text(expanded, encoding="utf-8")
        rustdoc = shutil.which("rustdoc")
        if rustdoc is None:
            raise CargoError("selected Rust toolchain has no rustdoc JSON authority")
        docdir = self.work / "rustdoc"
        docdir.mkdir(exist_ok=True)
        _run([rustdoc, str(self.root), "--crate-name", self.crate_name,
              "--edition", self.edition, "--target", target,
              "--output-format", "json", "-Z", "unstable-options",
              "--document-private-items", "-o", str(docdir), *self.args],
             environment=environment, cwd=self.root.parent)
        docfile = docdir / (self.crate_name + ".json")
        if not docfile.is_file():
            raise CargoError("rustdoc did not publish the selected package's public API document")
        document = json.loads(docfile.read_text(encoding="utf-8"))
        if not isinstance(document.get("index"), dict) or document.get("root") is None:
            raise CargoError("selected rustdoc JSON format is unsupported")
        self.provenance["rustdoc_format_version"] = document.get("format_version")
        return path, self._public_exports(document)

    @staticmethod
    def _public_exports(document) -> dict[tuple[str, ...], tuple[str, ...]]:
        index = document["index"]
        paths = document.get("paths", {})
        public = {}
        visited = set()
        def canonical(identifier, fallback):
            path = paths.get(str(identifier), {}).get("path")
            return tuple(path[1:]) if path else fallback
        def walk(identifier, exposed, *, reexport=False):
            item = index.get(str(identifier))
            if not item or (identifier, exposed) in visited:
                return
            visited.add((identifier, exposed))
            if not reexport and item.get("visibility") != "public":
                return
            inner = item.get("inner", {})
            kind = next(iter(inner), "")
            content = inner.get(kind) or {}
            if kind == "use":
                target = content.get("id")
                if target is None:
                    return
                if content.get("is_glob"):
                    source = index.get(str(target), {}).get("inner", {}).get("module", {})
                    for child in source.get("items", []):
                        child_item = index.get(str(child), {})
                        if child_item.get("name"):
                            walk(child, exposed[:-1] + (child_item["name"],))
                else:
                    walk(target, exposed, reexport=True)
                return
            if kind == "module":
                for child in content.get("items", []):
                    child_item = index.get(str(child), {})
                    name = child_item.get("name")
                    if name:
                        walk(child, exposed + (name,))
                return
            if kind in {"function", "struct", "enum", "type_alias", "trait"}:
                owner = canonical(identifier, exposed)
                public[exposed] = owner
                if kind in {"struct", "enum"}:
                    for implementation in content.get("impls", []):
                        declaration = index.get(str(implementation), {}).get("inner", {}).get("impl", {})
                        if declaration.get("trait") is not None:
                            continue
                        for method in declaration.get("items", []):
                            method_item = index.get(str(method), {})
                            if method_item.get("visibility") == "public" and "function" in method_item.get("inner", {}):
                                name = method_item["name"]
                                public[exposed + (name,)] = owner + (name,)
        root = index[str(document["root"])]["inner"]["module"]
        for item in root.get("items", []):
            name = index.get(str(item), {}).get("name")
            if name:
                walk(item, (name,))
        return public

    def _replay_arguments(self, arguments: list[str]) -> list[str]:
        replaced = {"--crate-name", "--crate-type", "--edition", "--emit", "--out-dir",
                    "--target", "--error-format", "--json", "--color"}
        result: list[str] = []
        index = 0
        while index < len(arguments):
            value = arguments[index]
            if value in replaced:
                index += 2
                continue
            if any(value.startswith(option + "=") for option in replaced):
                index += 1
                continue
            if value == "-C" and index + 1 < len(arguments):
                codegen = arguments[index + 1]
                if codegen.startswith(("incremental=", "extra-filename=")):
                    index += 2
                    continue
            if value == "--extern" and index + 1 < len(arguments):
                specification = arguments[index + 1]
                if "=" in specification:
                    name, filename = specification.split("=", 1)
                    metadata = Path(filename)
                    library = metadata.with_suffix(".rlib")
                    # Cargo passes rmeta to rustc for checking, even when it
                    # also emitted the same dependency's rlib.  A native
                    # materialization needs that measured sibling artifact.
                    if metadata.suffix == ".rmeta" and library.is_file():
                        specification = name + "=" + str(library)
                result.extend([value, specification])
                index += 2
                continue
            if not value.startswith("-") and value.endswith(".rs"):
                index += 1
                continue
            result.append(value)
            index += 1
        return result

    def close(self) -> None:
        if self._temp is not None:
            self._temp.cleanup()


def _digest(path: Path) -> str | None:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None

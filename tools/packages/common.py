"""Shared strict input and atomic file helpers for Vyx package sources."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import tomllib
from contextlib import contextmanager
import time


class PackageError(Exception):
    pass


NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")


def check_name(value: str, label: str = "package") -> str:
    if not isinstance(value, str) or not NAME.fullmatch(value):
        raise PackageError(f"invalid {label} name: {value!r}")
    return value


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise PackageError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def read_json(data: bytes | str, label: str) -> dict:
    try:
        result = json.loads(data, object_pairs_hook=_pairs,
                            parse_constant=lambda value: (_ for _ in ()).throw(PackageError(f"invalid JSON number {value}")))
    except (ValueError, UnicodeError) as exc:
        raise PackageError(f"invalid JSON in {label}: {exc}") from exc
    if not isinstance(result, dict):
        raise PackageError(f"{label} must contain a JSON object")
    return result


def atomic_bytes(path: Path, value: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def atomic_json(path: Path, value):
    atomic_bytes(path, (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode())


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def object_digest(value) -> str:
    return digest(json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode())


@contextmanager
def operation_lock(path: Path):
    """OS advisory locks release on process exit, including crashes."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as stream:
        stream.seek(0, os.SEEK_END)
        if stream.tell() == 0:
            stream.write(b"0")
            stream.flush()
        deadline = time.monotonic() + 120
        while True:
            try:
                stream.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise PackageError(f"timed out waiting for package operation lock {path}")
                time.sleep(0.05)
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def manifest_path(owner: Path) -> Path:
    for name in ("Vyx.toml", "vyx.toml"):
        path = owner / name
        if path.is_file():
            return path
    raise PackageError(f"package manifest not found in {owner}")


def load_manifest(owner: Path) -> dict:
    path = manifest_path(owner)
    try:
        value = tomllib.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as exc:
        raise PackageError(f"invalid package manifest {path}: {exc}") from exc
    if "package" in value and not isinstance(value["package"], dict):
        raise PackageError(f"invalid [package] table in {path}")
    return value


def manifest_identity(owner: Path, expected_name: str, expected_version: str):
    package = load_manifest(owner).get("package", {})
    if package.get("name") != expected_name or package.get("version") != expected_version:
        raise PackageError(
            f"archive package identity differs from index: expected "
            f"{expected_name}@{expected_version}, got "
            f"{package.get('name')!r}@{package.get('version')!r}")


def dependency_tables(manifest: dict):
    deps = manifest.get("dependencies", {})
    if not isinstance(deps, dict):
        raise PackageError("[dependencies] must be a table")
    if deps:
        yield "dependencies", deps
    for category in ("target", "template"):
        entries = manifest.get(category, {})
        if not isinstance(entries, dict):
            raise PackageError(f"[{category}] must contain tables")
        for name, table in entries.items():
            if not isinstance(table, dict):
                continue
            deps = table.get("dependencies", {})
            if not isinstance(deps, dict):
                raise PackageError(f"[{category}.{name}.dependencies] must be a table")
            if deps:
                yield f"{category}.{name}.dependencies", deps


def effective_dependency(manifest: dict, section: str, alias: str):
    """Mirror package -> inherited template -> target alias overlay."""
    result = dict(manifest.get("dependencies", {}))
    origins = {name: "dependencies" for name in result}
    parts = section.split(".")
    if len(parts) == 3 and parts[0] in ("target", "template") and parts[2] == "dependencies":
        templates = manifest.get("template", {})
        active = []

        def apply_template(name):
            if name in active:
                raise PackageError("template dependency cycle: " + " -> ".join(active + [name]))
            if name not in templates:
                raise PackageError(f"undefined template {name!r}")
            active.append(name)
            apply_table(templates[name], f"template.{name}.dependencies")
            active.pop()

        def apply_table(table, origin):
            parents = table.get("extends", [])
            if isinstance(parents, str):
                parents = [parents]
            for parent in parents:
                apply_template(parent)
            for key, value in table.get("dependencies", {}).items():
                result[key] = value
                origins[key] = origin
            for key in table.get("dependencies_remove", []):
                result.pop(key, None)
                origins.pop(key, None)

        table = manifest.get(parts[0], {}).get(parts[1], {})
        apply_table(table, section)
    elif section != "dependencies":
        raise PackageError(f"invalid dependency table {section!r}")
    if alias not in result:
        raise PackageError(f"dependency {alias!r} is not declared in [{section}]")
    return origins[alias], result[alias]


def active_dependency_tables(manifest: dict):
    """Union effective target dependencies; overridden defaults are unreachable."""
    targets = manifest.get("target", {})
    if not targets:
        yield "dependencies", manifest.get("dependencies", {})
        return
    aliases = {alias for _, deps in dependency_tables(manifest) for alias in deps}
    tables = {}
    adjacency = {}

    def ordered_dependencies(table, visiting):
        result = []
        parents = table.get("extends", [])
        if isinstance(parents, str):
            parents = [parents]
        for parent in parents:
            if parent in visiting:
                raise PackageError("template dependency cycle: " + " -> ".join(visiting + [parent]))
            template = manifest.get("template", {}).get(parent)
            if template is None:
                raise PackageError(f"undefined template {parent!r}")
            result.extend(ordered_dependencies(template, visiting + [parent]))
        dependencies = table.get("depends_on", [])
        if not isinstance(dependencies, list) or any(not isinstance(name, str) for name in dependencies):
            raise PackageError("depends_on must be a string array")
        result.extend(dependencies)
        removed = table.get("depends_on_remove", [])
        return [name for name in result if name not in removed]

    for target, data in targets.items():
        if not isinstance(data, dict):
            continue
        section = f"target.{target}.dependencies"
        adjacent = set(ordered_dependencies(data, []))
        for alias in sorted(aliases):
            try:
                origin, value = effective_dependency(manifest, section, alias)
            except PackageError as exc:
                if str(exc).startswith("dependency ") and "is not declared" in str(exc):
                    continue
                raise
            tables.setdefault(origin, {})[alias] = value
            if isinstance(value, dict) and "target" in value and "version" not in value and value.get("path", ".") == ".":
                if not isinstance(value["target"], str):
                    raise PackageError("local dependency target must be a string")
                adjacent.add(value["target"])
        adjacency[target] = adjacent

    done = set()
    stack = []

    def check(target):
        if target in stack:
            raise PackageError("local target dependency cycle: " + " -> ".join(stack + [target]))
        if target in done:
            return
        if target not in adjacency:
            raise PackageError(f"local dependency target {target!r} is not declared")
        stack.append(target)
        for adjacent in sorted(adjacency[target]):
            check(adjacent)
        stack.pop()
        done.add(target)

    for target in adjacency:
        check(target)
    yield from tables.items()

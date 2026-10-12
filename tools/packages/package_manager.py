#!/usr/bin/env python3
"""Vyx JSON package source CLI. Python 3.11+, no external dependencies."""
from __future__ import annotations

import sys
if sys.version_info < (3, 11):
    print("package error: Vyx package management requires Python 3.11 or newer; set VYX_PYTHON", file=sys.stderr)
    sys.exit(1)

import argparse
import io
import json
import os
from pathlib import Path
import re
import tarfile
import fnmatch
from contextlib import nullcontext

from archive import GENERATED, MARKER, safe_name
from common import PackageError, atomic_bytes, atomic_json, check_name, digest, load_manifest, manifest_path, operation_lock, read_json
from graph import Resolver, parse_dependency
from sources import Indices, fetch, file_path, load_sources, normalize_url, platform_identity, source_command, sources_config_path
from versions import Version


def common_options(parser):
    parser.add_argument("--project", default=os.environ.get("VYX_PACKAGE_PROJECT", "."))
    parser.add_argument("--triplet")
    parser.add_argument("--abi")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--locked", action="store_true")


def parser():
    root = argparse.ArgumentParser(description="Vyx third-party packages from named JSON sources")
    commands = root.add_subparsers(dest="command", required=True)
    install = commands.add_parser("install", help="install dependencies or add PACKAGE[@REQUIREMENT]")
    install.add_argument("package", nargs="?")
    install.add_argument("version", nargs="?")
    install.add_argument("--source")
    common_options(install)
    for name in ("lock", "update"):
        common_options(commands.add_parser(name))
    search = commands.add_parser("search")
    search.add_argument("query", nargs="?", default="")
    search.add_argument("--source")
    common_options(search)
    publish = commands.add_parser("publish")
    publish.add_argument("--source")
    publish.add_argument("--dry-run", action="store_true")
    common_options(publish)
    source = commands.add_parser("source")
    source.add_argument("action", choices=("add", "list", "remove"))
    source.add_argument("name", nargs="?")
    source.add_argument("url", nargs="?")
    source.add_argument("--token-env")
    common_options(source)
    resolve = commands.add_parser("resolve-path")
    resolve.add_argument("--owner", required=True)
    resolve.add_argument("--section", required=True)
    resolve.add_argument("--name", required=True)
    resolve.add_argument("--output", required=True)
    resolve.add_argument("--map-output")
    common_options(resolve)
    return root


def toml_value(value):
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, list):
        return "[" + ", ".join(toml_value(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{ " + ", ".join(json.dumps(str(key)) + " = " + toml_value(item) for key, item in value.items()) + " }"
    raise PackageError("unsupported dependency metadata for manifest update")


def edit_dependency(project: Path, name: str, version: str, source: str | None):
    manifest = load_manifest(project)
    existing = manifest.get("dependencies", {}).get(name)
    if existing is not None:
        parsed = parse_dependency(name, existing)
        if parsed.path:
            raise PackageError(f"dependency {name!r} is a path/local dependency; change it explicitly in Vyx.toml")
    value = dict(existing) if isinstance(existing, dict) else {}
    value["version"] = version
    if source:
        value["source"] = check_name(source, "source")
    elif "source" in value:
        # A repeated install retains the explicit source, never silently changes it.
        source = value["source"]
    path = manifest_path(project)
    original = path.read_bytes()
    text = original.decode("utf-8-sig")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)
    start = None
    end = len(lines)
    for i, line in enumerate(lines):
        if re.match(r"^\s*\[\s*dependencies\s*\]\s*(?:#.*)?$", line.rstrip("\r\n")):
            start = i + 1
            continue
        if start is not None and re.match(r"^\s*\[", line):
            end = i
            break
    replacement = f"{json.dumps(name)} = {toml_value(value)}{newline}"
    if start is None:
        updated = text.rstrip("\r\n") + newline + newline + "[dependencies]" + newline + replacement
    else:
        matching = []
        pattern = re.compile(r"^\s*(?:" + re.escape(name) + r"|\"" + re.escape(name) + r"\"|'" + re.escape(name) + r"')\s*=")
        for i in range(start, end):
            if pattern.match(lines[i]):
                matching.append(i)
        if existing is not None and len(matching) != 1:
            raise PackageError("cannot preserve this dependency declaration automatically; edit Vyx.toml explicitly")
        if matching:
            index = matching[0]
            lines[index] = replacement
        else:
            lines.insert(end, replacement)
        updated = "".join(lines)
    # Verify the entire candidate before touching the actual project manifest.
    import tomllib
    try:
        checked = tomllib.loads(updated)
    except tomllib.TOMLDecodeError as exc:
        raise PackageError(f"cannot update manifest while preserving TOML: {exc}") from exc
    if checked.get("dependencies", {}).get(name) != value:
        raise PackageError("manifest update did not produce the requested dependency")
    return path, original, updated.encode("utf-8")


def install(options, project):
    if not options.package:
        lock = Resolver(project, options.triplet, options.abi).prepare(options.offline, options.locked)
    else:
        if options.locked or options.offline:
            raise PackageError("adding a dependency cannot be combined with --locked or --offline")
        name, sep, requirement = options.package.partition("@")
        check_name(name)
        if sep and options.version:
            raise PackageError("specify the requirement with @ or as a positional argument, not both")
        requirement = requirement if sep else options.version or "*"
        from versions import Requirement
        Requirement(requirement)
        path, original, updated = edit_dependency(project, name, requirement, options.source)
        atomic_bytes(path, updated)
        try:
            lock = Resolver(project, options.triplet, options.abi).prepare()
        except Exception:
            atomic_bytes(path, original)
            raise
    print(f"installed {len(lock['packages'])} packages; lock: {project / 'Vyx.packages.lock'}")


def search(options, project):
    if options.offline:
        raise PackageError("search requires source indices; offline resolution applies to installed packages")
    sources = load_sources(project)
    if options.source:
        sources = [item for item in sources if item.name == options.source]
        if not sources:
            raise PackageError(f"unknown package source {options.source!r}")
    triplet, abi = platform_identity(options.triplet, options.abi)
    indices = Indices(sources, triplet, abi)
    query = options.query.lower()
    count = 0
    for source in sources:
        for name, value in indices.get(source)["packages"].items():
            description = value.get("description", "")
            if query in name.lower() or query in description.lower():
                versions = sorted(value["versions"], key=lambda v: (Version(v), v), reverse=True)
                print(f"{source.name}/{name}\t{versions[0] if versions else '-'}\t{description}")
                count += 1
    print(f"{count} packages")


def publish(options, project):
    if options.offline or options.locked:
        raise PackageError("publish cannot be combined with --offline or --locked")
    sources = load_sources(project)
    if options.source:
        sources = [item for item in sources if item.name == options.source]
    if len(sources) != 1:
        raise PackageError("publish requires --source naming one configured local JSON source")
    source = sources[0]
    index_path = file_path(source.url)
    manifest = load_manifest(project)
    metadata = manifest.get("package", {})
    name = check_name(metadata.get("name"))
    version = metadata.get("version")
    Version(version)
    kind = metadata.get("kind", "source")
    if kind not in ("source", "binary", "dci"):
        raise PackageError("package.kind must be source, binary or dci")
    if index_path.exists():
        index = read_json(index_path.read_bytes(), str(index_path))
        Indices([source], *platform_identity(options.triplet, options.abi)).get(source)
    else:
        index = {"schema": 1, "packages": {}}
    package = index["packages"].setdefault(name, {"versions": {}})
    if "description" in metadata:
        package["description"] = metadata["description"]
    if kind == "source" and version in package["versions"]:
        raise PackageError(f"{name}@{version} is already published; immutable versions cannot be replaced")
    triplet, abi = platform_identity(options.triplet, options.abi)
    if kind != "source":
        from archive import check_binary
        check_binary(project, {"kind": kind, "triplet": triplet, "abi": abi})
        old_release = package["versions"].get(version)
        if old_release and old_release.get("kind") != kind:
            raise PackageError("published version already has a different package kind")
        if old_release and any(item["triplet"] == triplet and item["abi"] == abi for item in old_release["artifacts"]):
            raise PackageError(f"{name}@{version} is already published for {triplet}/{abi}")
    output_dir = index_path.parent / "packages" / name / version
    suffix = "" if kind == "source" else "-" + triplet + "-" + abi
    archive_path = output_dir / (name + "-" + version + suffix + ".tar.gz")
    excluded = GENERATED | {".git", ".hg", ".svn", ".idea", ".vscode", ".orbit", ".choccy", ".cursor", "node_modules", "dist"}
    include = metadata.get("include", [])
    exclude = metadata.get("exclude", [])
    if not isinstance(include, list) or not isinstance(exclude, list) or any(not isinstance(item, str) for item in include + exclude):
        raise PackageError("package.include and package.exclude must be string arrays")
    payload = []
    for base, directories, names in os.walk(project, followlinks=False):
        parent = Path(base)
        for entry in directories + names:
            if (parent / entry).is_symlink():
                raise PackageError(f"publish rejects symbolic links: {parent / entry}")
        directories[:] = sorted(item for item in directories if item not in excluded and not item.startswith(".")
                                 and (parent / item).resolve() != index_path.parent.resolve())
        for filename in sorted(names):
            path = parent / filename
            relative = path.relative_to(project).as_posix()
            if filename.startswith(".") or filename.lower().endswith((".log", ".tmp", ".bak", ".pyc")):
                continue
            if filename in (MARKER, "Vyx.packages.lock") or path.resolve() == index_path.resolve():
                continue
            if include and relative not in ("Vyx.toml", "vyx.toml", "Vyx.lock") and not any(fnmatch.fnmatchcase(relative, glob) for glob in include):
                continue
            if any(fnmatch.fnmatchcase(relative, glob) for glob in exclude):
                continue
            safe_name(relative)
            payload.append((relative, path))
    if not any(relative in ("Vyx.toml", "vyx.toml") for relative, _ in payload):
        raise PackageError("publish payload must contain the package manifest")
    packaged = {relative for relative, _ in payload}
    if kind != "source":
        for target_name, target in manifest.get("target", {}).items():
            if target.get("type") != "prebuilt":
                continue
            required = [target["artifact"]]
            for field in ("sources", "interface_sources", "dci_files", "runtime_files"):
                required.extend(target.get(field, []))
            missing = [file for file in required if file not in packaged]
            if missing:
                raise PackageError(f"prebuilt target {target_name}: publish filters omit required files: {', '.join(missing)}")
    for relative, _ in payload:
        print(relative)
    if options.dry_run:
        print(f"{len(payload)} payload files; dry run, no archive or index written")
        return
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as archive:
        for relative, path in payload:
            info = archive.gettarinfo(str(path), relative)
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ""
            with path.open("rb") as input_file:
                archive.addfile(info, input_file)
    data = stream.getvalue()
    relative_url = archive_path.relative_to(index_path.parent).as_posix()
    artifact = {"url": relative_url, "sha256": digest(data)}
    if kind == "source":
        package["versions"][version] = artifact
    else:
        artifact.update(triplet=triplet, abi=abi)
        release = package["versions"].setdefault(version, {"kind": kind, "artifacts": []})
        release["artifacts"].append(artifact)
    atomic_bytes(archive_path, data)
    atomic_json(index_path, index)
    print(f"published {source.name}/{name}@{version} ({kind})")


def main(argv=None):
    options = parser().parse_args(argv)
    try:
        project = Path(options.project).resolve()
        if options.command == "source":
            if options.action == "add" and (not options.name or not options.url):
                raise PackageError("source add requires NAME URL")
            if options.action == "remove" and not options.name:
                raise PackageError("source remove requires NAME")
            with operation_lock(sources_config_path().with_suffix(".lock")):
                source_command(options.action, options.name, options.url, options.token_env)
        elif options.command == "search":
            search(options, project)
        else:
            with operation_lock(project / ".cache" / "packages" / "operation.lock"):
                if options.command == "publish":
                    chosen = [item for item in load_sources(project) if not options.source or item.name == options.source]
                    if len(chosen) != 1:
                        raise PackageError("publish requires --source naming one configured local JSON source")
                    with operation_lock(file_path(chosen[0].url).with_suffix(".publish.lock")):
                        publish(options, project)
                elif options.command == "install":
                    install(options, project)
                else:
                    resolver = Resolver(project, options.triplet, options.abi)
                    if options.command == "resolve-path":
                        path = resolver.resolve_path(Path(options.owner), options.section, options.name,
                                                     offline=options.offline, locked=options.locked)
                        if any(c in str(path) for c in ("\n", "\r", "\t")):
                            raise PackageError("resolved dependency path contains unsupported control characters")
                        atomic_bytes(Path(options.output), str(path).encode("utf-8"))
                        if options.map_output:
                            lock = resolver.prepare(options.offline, options.locked)
                            lines = []
                            for edge in lock["edges"]:
                                fields = [str(resolver.absolute(edge["owner"])), edge["section"], edge["alias"],
                                          str(resolver.absolute(edge["destination"]))]
                                if any(any(c in field for c in ("\n", "\r", "\t")) for field in fields):
                                    raise PackageError("package graph path map contains unsupported control characters")
                                lines.append("\t".join(fields))
                            atomic_bytes(Path(options.map_output), ("\n".join(lines) + ("\n" if lines else "")).encode("utf-8"))
                    else:
                        lock = resolver.prepare(options.offline, options.locked, options.command == "update")
                        print(f"locked {len(lock['packages'])} packages in {resolver.lock_path}")
        return 0
    except RecursionError:
        print("package error: dependency graph nesting exceeds the resolver recursion limit", file=sys.stderr)
        return 1
    except (PackageError, OSError, UnicodeError) as exc:
        print(f"package error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

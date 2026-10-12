"""Dependency graph solving, provenance locks, and compiler path queries."""
from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
import os
from pathlib import Path
import re

from archive import cache_dir, materialize
from common import PackageError, active_dependency_tables, atomic_json, check_name, dependency_tables, digest, effective_dependency, load_manifest, manifest_path, object_digest, read_json
from sources import Indices, Source, load_sources, normalize_url, platform_identity
from versions import Requirement, Version

LOCK_NAME = "Vyx.packages.lock"
MAX_NODES = 1000


@dataclass
class Dependency:
    package: str | None = None
    requirement: str | None = None
    source: str | None = None
    path: str | None = None


def parse_dependency(alias, value):
    check_name(alias, "dependency alias")
    if isinstance(value, str):
        if value.startswith("registry:"):
            spec = value[len("registry:"):]
            package, sep, req = spec.partition("@")
            return Dependency(check_name(package), req if sep else "*")
        if value.startswith("std:"):
            return Dependency(path=value)
        if re.match(r"^[\d^~<>=*]", value) or value.lower() == "x":
            Requirement(value)
            return Dependency(alias, value)
        return Dependency(path=value)
    if not isinstance(value, dict):
        raise PackageError(f"dependency {alias!r} must be a version string or table")
    path = value.get("path")
    if path is not None:
        if not isinstance(path, str) or not path:
            raise PackageError(f"dependency {alias!r}: path must be a nonempty string")
        if path.startswith("registry:"):
            spec = parse_dependency(alias, path)
            if "source" in value:
                spec.source = check_name(value["source"], "source")
            return spec
        return Dependency(path=path)
    version = value.get("version")
    if version is None:
        if "target" in value:
            return Dependency(path=".")
        raise PackageError(f"dependency {alias!r}: expected path, target, or version")
    Requirement(version)
    package = check_name(value.get("package", alias))
    source = check_name(value["source"], "source") if "source" in value else None
    return Dependency(package, version, source)


def local_path(owner: Path, value: str):
    if value.startswith("std:"):
        return None
    expanded = expand_env(value)
    if not expanded:
        raise PackageError(f"dependency path expands to an empty value: {value!r}")
    path = Path(expanded)
    if not path.is_absolute():
        path = owner / path
    return path.resolve()


def expand_env(text: str, depth=0):
    if depth > 20:
        raise PackageError("dependency environment expansion exceeds nesting limit")
    result = []
    i = 0

    def spec(value):
        name, separator, default = value.partition(":-")
        value = os.environ.get(name.strip(), "")
        if value and value[0] not in "$%":
            return value
        return expand_env(default, depth + 1) if separator else ""

    while i < len(text):
        if text.startswith("${", i):
            end = i + 2
            level = 1
            while end < len(text):
                if text.startswith("${", end):
                    level += 1
                    end += 2
                    continue
                if text[end] == "}":
                    level -= 1
                    if not level:
                        break
                end += 1
            if end < len(text):
                result.append(spec(text[i + 2:end]))
                i = end + 1
                continue
        elif text[i] == "$":
            match = re.match(r"[A-Za-z0-9_]+", text[i + 1:])
            if match:
                result.append(spec(match[0]))
                i += len(match[0]) + 1
                continue
        elif os.name == "nt" and text[i] == "%":
            end = text.find("%", i + 1)
            if end >= 0:
                result.append(spec(text[i + 1:end]))
                i = end + 1
                continue
        result.append(text[i])
        i += 1
    return "".join(result)


class Resolver:
    def __init__(self, project: Path, triplet=None, abi=None):
        self.project = project.resolve()
        self.sources = load_sources(self.project)
        self.triplet, self.abi = platform_identity(triplet, abi)
        self.indices = Indices(self.sources, self.triplet, self.abi)
        self.lock_path = self.project / LOCK_NAME
        self.attempts = 0
        self.last_conflict = ""
        self.materialized = {}

    def package_path(self, source, name, version, artifact, offline=False):
        # This cache lives only for one serialized CLI transaction. Different
        # processes always recheck payloads against their trusted lock digest.
        key = (source.name, source.url, name, version, *(artifact.get(field) for field in
               ("kind", "url", "sha256", "strip_prefix", "triplet", "abi")))
        if key in self.materialized:
            path, payload = self.materialized[key]
            if artifact.get("payload_sha256") and artifact["payload_sha256"] != payload:
                raise PackageError("conflicting locked payload digest for the same archive")
            artifact["payload_sha256"] = payload
            return path
        path = materialize(self.project, source, name, version, artifact, offline)
        self.materialized[key] = path, artifact["payload_sha256"]
        return path

    def relative(self, path: Path):
        try:
            return Path(os.path.relpath(path.resolve(), self.project)).as_posix()
        except ValueError:
            return str(path.resolve())

    def absolute(self, path: str):
        value = Path(path)
        return value.resolve() if value.is_absolute() else (self.project / value).resolve()

    def local_inputs(self):
        snapshots = {}
        visited = set()

        def scan(owner):
            if owner in visited:
                return
            visited.add(owner)
            if len(visited) > MAX_NODES:
                raise PackageError("local dependency graph exceeds package limit")
            manifest = load_manifest(owner)
            tables = list(active_dependency_tables(manifest))
            package = manifest.get("package", {})
            snapshots[self.relative(owner)] = object_digest({
                "package": {key: package[key] for key in ("name", "version", "kind") if key in package},
                "dependencies": {section: deps for section, deps in tables}})
            for section, deps in tables:
                for alias, value in deps.items():
                    dep = parse_dependency(alias, value)
                    if dep.path:
                        destination = local_path(owner, dep.path)
                        if destination and destination != owner:
                            scan(destination)

        scan(self.project)
        return snapshots

    def input_record(self):
        return {"manifests": self.local_inputs(), "sources": [source.record() for source in self.sources],
                "triplet": self.triplet, "abi": self.abi}

    def load_lock(self, offline=False, locked=False):
        if not self.lock_path.exists():
            if offline or locked:
                raise PackageError(f"{LOCK_NAME} is required for {'offline' if offline else 'locked'} resolution")
            return None
        value = read_json(self.lock_path.read_bytes(), str(self.lock_path))
        if type(value.get("schema")) is not int or value.get("schema") != 1 or not isinstance(value.get("packages"), list) or not isinstance(value.get("edges"), list):
            raise PackageError(f"invalid {LOCK_NAME} schema")
        if set(value) != {"schema", "inputs", "packages", "edges"} or not isinstance(value.get("inputs"), dict):
            raise PackageError(f"invalid {LOCK_NAME} fields")
        inputs = self.input_record()
        if value.get("inputs") != inputs:
            old = value.get("inputs", {})
            if old.get("triplet") != self.triplet or old.get("abi") != self.abi:
                raise PackageError(f"{LOCK_NAME} targets a different triplet/ABI; run update --triplet {self.triplet} --abi {self.abi}")
            if offline or locked:
                raise PackageError(f"{LOCK_NAME} does not match project manifests or source configuration")
            return None
        nodes = {}
        chosen = {}
        for record in value["packages"]:
            if not isinstance(record, dict):
                raise PackageError("invalid package lock record")
            if set(record) != {"name", "version", "source", "artifact"}:
                raise PackageError("unknown or missing package lock record fields")
            name = check_name(record.get("name"))
            version = record.get("version")
            Version(version)
            src = record.get("source", {})
            if not isinstance(src, dict):
                raise PackageError("locked source must be an object")
            if set(src) != {"name", "url"}:
                raise PackageError("locked source requires only name and url")
            source = next((item for item in self.sources if item.name == src.get("name") and item.url == src.get("url")), None)
            if not source:
                raise PackageError(f"locked source is not configured for {name}")
            artifact = record.get("artifact")
            if not isinstance(artifact, dict) or artifact.get("triplet") != self.triplet or artifact.get("abi") != self.abi:
                raise PackageError(f"invalid locked artifact platform for {name}")
            if set(artifact) != {"kind", "url", "sha256", "strip_prefix", "triplet", "abi", "payload_sha256"}:
                raise PackageError(f"invalid locked artifact fields for {name}")
            if not isinstance(artifact["payload_sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", artifact["payload_sha256"]):
                raise PackageError(f"invalid locked payload digest for {name}")
            self.indices.validate_artifact({key: artifact[key] for key in ("url", "sha256", "strip_prefix") if key in artifact}, source, binary=False)
            if artifact.get("kind") not in ("source", "binary", "dci"):
                raise PackageError(f"invalid locked package kind for {name}")
            normalized = normalize_url(artifact["url"], source.url)
            if normalized != artifact["url"]:
                raise PackageError("locked artifact URL must be absolute")
            owner = self.package_path(source, name, version, artifact, offline)
            key = source.package_key(name)
            if key in nodes:
                raise PackageError(f"duplicate package identity in {LOCK_NAME}")
            nodes[key] = owner
            chosen[key] = record
        for edge in value["edges"]:
            if not isinstance(edge, dict) or not all(isinstance(edge.get(key), str) for key in ("owner", "section", "alias", "destination")):
                raise PackageError("invalid dependency edge in package lock")
            check_name(edge["alias"], "dependency alias")
            if "package" in edge and not isinstance(edge["package"], str):
                raise PackageError("locked edge package identity must be a string")
            if edge.get("package") and edge["package"] not in nodes:
                raise PackageError("locked dependency references a missing package")
            if edge.get("package") and self.absolute(edge["destination"]) != nodes[edge["package"]]:
                raise PackageError("locked dependency path does not match verified package cache")
        requirements, rebuilt_edges, reachable = self.discover(chosen, use_locked_sources=True, offline=offline)
        if reachable != set(chosen):
            raise PackageError("package lock contains missing or unreachable package records")
        for key, demands in requirements.items():
            if key not in chosen or not all(req.accepts(Version(chosen[key]["version"])) for req, _, _, _ in demands):
                raise PackageError(f"locked package violates manifest requirements: {key}")
        if sorted(rebuilt_edges, key=lambda edge: (edge["owner"], edge["section"], edge["alias"])) != value["edges"]:
            raise PackageError("package lock dependency graph differs from verified manifests")
        return value

    def discover(self, chosen, use_locked_sources=False, offline=False):
        requirements = defaultdict(list)
        edges = []
        visited = set()
        active = []
        active_owners = []
        reachable = set()
        owners = {}

        def scan(owner, package_key=None):
            label = package_key or self.relative(owner)
            if label in active or owner in active_owners:
                raise PackageError("package dependency cycle: " + " -> ".join(active + [label]))
            if owner in visited:
                return
            if len(visited) >= MAX_NODES:
                raise PackageError("dependency graph exceeds package limit")
            visited.add(owner)
            active.append(label)
            active_owners.append(owner)
            manifest = load_manifest(owner)
            owners[label] = owner
            for section, deps in active_dependency_tables(manifest):
                for alias, value in deps.items():
                    dependency = parse_dependency(alias, value)
                    edge = {"owner": self.relative(owner), "section": section, "alias": alias}
                    if dependency.path:
                        destination = local_path(owner, dependency.path)
                        if destination is None:
                            continue
                        # A same-manifest target edge is handled by the native target scheduler.
                        if destination == owner:
                            continue
                        edge["destination"] = self.relative(destination)
                        edges.append(edge)
                        scan(destination)
                        continue
                    if use_locked_sources:
                        matches = [self.sources[i] for i in range(len(self.sources))
                                   if any(record["name"] == dependency.package and record["source"] == self.sources[i].record()
                                          for record in chosen.values())
                                   and (not dependency.source or self.sources[i].name == dependency.source)]
                        if not matches:
                            raise PackageError(f"lock has no source for {dependency.package!r}")
                        source = matches[0]
                    else:
                        source = self.indices.choose_source(dependency.package, dependency.source)
                    key = source.package_key(dependency.package)
                    reachable.add(key)
                    requirements[key].append((Requirement(dependency.requirement), source, dependency.package,
                                              f"{self.relative(owner)} [{section}] {alias}"))
                    if key in chosen:
                        selected = chosen[key]
                        destination = self.package_path(source, dependency.package, selected["version"], selected["artifact"], offline)
                        edge["destination"] = self.relative(destination)
                        edge["package"] = key
                        edges.append(edge)
                        scan(destination, key)
            active.pop()
            active_owners.pop()

        scan(self.project)
        return requirements, edges, reachable

    def solve(self, chosen):
        self.attempts += 1
        if self.attempts > 10_000:
            raise PackageError("version solving exceeded 10000 states; narrow version requirements")
        try:
            requirements, edges, reachable = self.discover(chosen)
        except PackageError as exc:
            # Only graph conflicts may backtrack; corrupt archives, indices and transport errors never do.
            if str(exc).startswith("package dependency cycle:"):
                self.last_conflict = str(exc)
                return None
            raise
        for key, values in requirements.items():
            if key in chosen and not all(req.accepts(Version(chosen[key]["version"])) for req, _, _, _ in values):
                self.last_conflict = self.conflict_message(key, values)
                return None
        missing = [key for key in requirements if key not in chosen]
        if not missing:
            return {key: value for key, value in chosen.items() if key in reachable}, edges
        key = missing[0]
        values = requirements[key]
        source, name = values[0][1:3]
        releases = self.indices.get(source)["packages"][name]["versions"]
        versions = sorted(releases, key=lambda v: (Version(v), v), reverse=True)
        matching = [version for version in versions if all(req.accepts(Version(version)) for req, _, _, _ in values)]
        if not matching:
            self.last_conflict = self.conflict_message(key, values)
            return None
        for version in matching:
            artifact = self.indices.artifact(source, name, version)
            if self.lock_path.exists():
                previous = read_json(self.lock_path.read_bytes(), str(self.lock_path))
                previous_packages = previous.get("packages", [])
                if not isinstance(previous_packages, list):
                    raise PackageError("previous package lock packages must be an array")
                for old in previous_packages:
                    if not isinstance(old, dict) or not isinstance(old.get("source"), dict) or not isinstance(old.get("artifact"), dict):
                        raise PackageError("invalid previous package lock record")
                    if (old.get("name") == name and old.get("version") == version
                            and old.get("source") == source.record()):
                        old_artifact = old.get("artifact", {})
                        same_platform = old_artifact.get("triplet") == artifact["triplet"] and old_artifact.get("abi") == artifact["abi"]
                        if old_artifact.get("kind") != artifact["kind"]:
                            raise PackageError(f"immutable package changed checksum/kind: {source.name}/{name}@{version}")
                        if (same_platform or artifact["kind"] == "source") and old_artifact.get("sha256") != artifact["sha256"]:
                            raise PackageError(f"immutable package changed checksum/kind: {source.name}/{name}@{version}")
                        if old_artifact.get("sha256") == artifact["sha256"] and old_artifact.get("payload_sha256"):
                            artifact["payload_sha256"] = old_artifact["payload_sha256"]
            next_chosen = dict(chosen)
            next_chosen[key] = {"name": name, "version": version, "source": source.record(), "artifact": artifact}
            result = self.solve(next_chosen)
            if result is not None:
                return result
        return None

    @staticmethod
    def conflict_message(key, values):
        _, source, name, _ = values[0]
        demands = "; ".join(f"{req.text} from {origin}" for req, _, _, origin in values)
        return f"version conflict for {source.name}/{name}: {demands}"

    def prepare(self, offline=False, locked=False, update=False):
        if update and (offline or locked):
            raise PackageError("update cannot be combined with --offline or --locked")
        if not update:
            value = self.load_lock(offline, locked)
            if value is not None:
                return value
        result = self.solve({})
        if result is None:
            raise PackageError(self.last_conflict or "package graph cannot be resolved")
        chosen, edges = result
        value = {"schema": 1, "inputs": self.input_record(),
                 "packages": [chosen[key] for key in sorted(chosen)],
                 "edges": sorted(edges, key=lambda edge: (edge["owner"], edge["section"], edge["alias"]))}
        atomic_json(self.lock_path, value)
        return value

    def resolve_path(self, owner: Path, section: str, alias: str, **options):
        lock = self.prepare(**options)
        owner = owner.resolve()
        origin, value = effective_dependency(load_manifest(owner), section, alias)
        dependency = parse_dependency(alias, value)
        if dependency.path:
            destination = local_path(owner, dependency.path)
            if destination is None:
                raise PackageError("std: dependencies are resolved by the SDK build pipeline")
            return destination
        key = self.relative(owner), origin, alias
        matches = [edge for edge in lock["edges"] if (edge["owner"], edge["section"], edge["alias"]) == key]
        if len(matches) != 1:
            raise PackageError(f"dependency {alias!r} has no unique verified package path for [{section}]")
        return self.absolute(matches[0]["destination"])

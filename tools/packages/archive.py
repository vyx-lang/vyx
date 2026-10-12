"""Verified package cache and archive extraction without executable hooks."""
from __future__ import annotations

import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tarfile
import tempfile
import zipfile

from common import PackageError, atomic_json, digest, load_manifest, manifest_identity, object_digest, read_json
from sources import MAX_ARCHIVE, Source, fetch, origin

MARKER = ".vyx-package.json"
GENERATED = {".cache", "target", "out"}
MAX_FILES = 100_000
MAX_EXPANDED = 1024 * 1024 * 1024


def safe_name(value: str) -> str:
    if not isinstance(value, str) or not value or "\\" in value or "\x00" in value:
        raise PackageError(f"unsafe archive path {value!r}")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise PackageError(f"unsafe archive path {value!r}")
    for part in path.parts:
        if ":" in part or part.endswith((" ", ".")) or any(ord(c) < 32 for c in part):
            raise PackageError(f"nonportable archive path {value!r}")
        if part.split(".", 1)[0].upper() in {"CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(1, 10)], *[f"LPT{i}" for i in range(1, 10)]}:
            raise PackageError(f"nonportable archive path {value!r}")
    return path.as_posix()


def _members(data: bytes):
    stream = io.BytesIO(data)
    if zipfile.is_zipfile(stream):
        with zipfile.ZipFile(stream) as archive:
            for item in archive.infolist():
                mode = item.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise PackageError(f"archive contains symbolic link {item.filename!r}")
                if item.flag_bits & 1:
                    raise PackageError("encrypted package archives are unsupported")
                if item.is_dir():
                    yield item.filename.rstrip("/"), True, 0, None, 0o755
                else:
                    if mode and stat.S_IFMT(mode) not in (0, stat.S_IFREG):
                        raise PackageError(f"archive contains special file {item.filename!r}")
                    yield item.filename, False, item.file_size, lambda i=item: archive.open(i), mode
    else:
        try:
            stream.seek(0)
            with tarfile.open(fileobj=stream, mode="r:*") as archive:
                for item in archive:
                    if item.isdir():
                        yield item.name.rstrip("/"), True, 0, None, item.mode
                    elif item.isfile():
                        yield item.name, False, item.size, lambda i=item: archive.extractfile(i), item.mode
                    else:
                        raise PackageError(f"archive contains link or special file {item.name!r}")
        except tarfile.TarError as exc:
            raise PackageError(f"invalid package archive: {exc}") from exc


def extract(data: bytes, dest: Path, strip_prefix: str):
    prefix = safe_name(strip_prefix) if strip_prefix else ""
    seen = set()
    files = set()
    spellings = {}
    count = 0
    total = 0
    for raw, directory, size, opener, mode in _members(data):
        name = safe_name(raw)
        if prefix:
            if name == prefix:
                if not directory:
                    raise PackageError("strip_prefix names an archive file")
                continue
            if not name.startswith(prefix + "/"):
                raise PackageError(f"archive member {name!r} is outside strip_prefix {prefix!r}")
            name = name[len(prefix) + 1:]
        folded = name.casefold()
        parts = PurePosixPath(name).parts
        for count_parts in range(1, len(parts) + 1):
            prefix_name = "/".join(parts[:count_parts])
            previous = spellings.setdefault(prefix_name.casefold(), prefix_name)
            if previous != prefix_name:
                raise PackageError(f"archive path case collision: {previous!r} and {prefix_name!r}")
        if folded in seen:
            raise PackageError(f"duplicate archive member {name!r}")
        seen.add(folded)
        if PurePosixPath(name).parts[0] in GENERATED or name == MARKER:
            raise PackageError(f"archive contains reserved generated path {name!r}")
        ancestors = list(PurePosixPath(name).parents)[:-1]
        if any(str(parent).casefold() in files for parent in ancestors):
            raise PackageError(f"archive file/directory collision {name!r}")
        target = dest.joinpath(*PurePosixPath(name).parts)
        if directory:
            target.mkdir(parents=True, exist_ok=True)
            continue
        if any(existing.startswith(folded + "/") for existing in seen if existing != folded):
            raise PackageError(f"archive file/directory collision {name!r}")
        files.add(folded)
        count += 1
        total += size
        if count > MAX_FILES or total > MAX_EXPANDED or size < 0:
            raise PackageError("package archive exceeds extraction limits")
        target.parent.mkdir(parents=True, exist_ok=True)
        with opener() as source, target.open("xb") as output:
            copied = 0
            while True:
                chunk = source.read(min(1024 * 1024, size - copied + 1))
                if not chunk:
                    break
                copied += len(chunk)
                if copied > size:
                    raise PackageError(f"archive member exceeds declared size: {name}")
                output.write(chunk)
            if copied != size:
                raise PackageError(f"archive member size mismatch: {name}")
        if os.name != "nt":
            target.chmod(0o755 if mode & 0o111 else 0o644)
    if not files:
        raise PackageError("package archive is empty")


def tree_files(root: Path) -> dict:
    result = {}
    for base, directories, names in os.walk(root, followlinks=False):
        parent = Path(base)
        rel = parent.relative_to(root)
        for name in directories:
            if (parent / name).is_symlink():
                raise PackageError(f"package cache contains symbolic link: {parent / name}")
        if rel == Path("."):
            directories[:] = [name for name in directories if name not in GENERATED]
        for name in names:
            path = parent / name
            key = path.relative_to(root).as_posix()
            if key == MARKER:
                continue
            if path.is_symlink() or not path.is_file():
                raise PackageError(f"package cache contains special file: {path}")
            sha = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    sha.update(chunk)
            result[key] = sha.hexdigest()
    return result


def cache_dir(project: Path, source: Source, name: str, version: str, artifact: dict) -> Path:
    return project / ".cache" / "packages" / source.identity / name / version / artifact["sha256"][:16]


def check_binary(owner: Path, artifact: dict):
    manifest = load_manifest(owner)
    kind = manifest.get("package", {}).get("kind", "source")
    if kind != artifact["kind"]:
        raise PackageError(f"package.kind {kind!r} differs from index kind {artifact['kind']!r}")
    if kind == "source":
        return
    prebuilt = []
    for name, target in manifest.get("target", {}).items():
        if not isinstance(target, dict) or target.get("type") != "prebuilt":
            continue
        prebuilt.append(name)
        if target.get("triplet") != artifact["triplet"] or target.get("abi") != artifact["abi"]:
            raise PackageError(f"prebuilt target {name}: triplet/abi differs from selected artifact")
        if target.get("linkage") not in ("static", "shared"):
            raise PackageError(f"prebuilt target {name}: linkage must be static or shared")
        library = target.get("artifact")
        if not isinstance(library, str):
            raise PackageError(f"prebuilt target {name}: artifact must name the native library")
        paths = [library]
        for field in ("sources", "interface_sources", "dci_files", "runtime_files"):
            values = target.get(field, [])
            if not isinstance(values, list) or any(not isinstance(item, str) for item in values):
                raise PackageError(f"prebuilt target {name}: {field} must be a string array")
            paths.extend(values)
        if kind == "dci" and (not target.get("dci_files") or not target.get("sources")):
            raise PackageError(f"DCI target {name} requires contracts in dci_files and Vyx definitions in sources")
        for value in paths:
            safe_name(value)
            if not (owner / value).is_file():
                raise PackageError(f"prebuilt target {name}: missing packaged file {value!r}")
    if not prebuilt:
        raise PackageError(f"{kind} package requires a prebuilt target")


def materialize(project: Path, source: Source, name: str, version: str, artifact: dict, offline=False) -> Path:
    destination = cache_dir(project, source, name, version, artifact)
    marker = destination / MARKER
    if destination.exists():
        if not marker.is_file():
            raise PackageError(f"package cache has no verification record: {destination}")
        value = read_json(marker.read_bytes(), str(marker))
        files = tree_files(destination)
        marker_artifact = value.get("artifact", {})
        expected_artifact = {key: val for key, val in artifact.items() if key != "payload_sha256"}
        recorded_artifact = {key: val for key, val in marker_artifact.items() if key != "payload_sha256"}
        if artifact.get("kind") == "source":
            # The lock owns target selection; a source archive's payload itself
            # is platform-independent and may be reused after explicit update.
            for key in ("triplet", "abi"):
                expected_artifact.pop(key, None)
                recorded_artifact.pop(key, None)
        if expected_artifact != recorded_artifact or value.get("files") != files:
            raise PackageError(f"package cache integrity check failed: {destination}")
        if artifact.get("payload_sha256"):
            if object_digest(files) != artifact["payload_sha256"]:
                raise PackageError(f"package cache payload differs from locked verified archive: {destination}")
        else:
            if offline:
                raise PackageError("offline package lock lacks the verified archive payload digest")
            # A local marker is never a trust anchor: bind an unlocked cache to
            # the index checksum by extracting the original artifact again.
            token_env = source.token_env if origin(source.url) == origin(artifact["url"]) else None
            data = fetch(artifact["url"], token_env, MAX_ARCHIVE)
            if digest(data) != artifact["sha256"]:
                raise PackageError(f"artifact checksum mismatch for {source.name}/{name}@{version}")
            with tempfile.TemporaryDirectory(prefix=".verify-", dir=destination.parent) as temp_name:
                temp = Path(temp_name)
                extract(data, temp, artifact.get("strip_prefix", ""))
                if tree_files(temp) != files:
                    raise PackageError(f"package cache differs from checksum-verified archive: {destination}")
            artifact["payload_sha256"] = object_digest(files)
        manifest_identity(destination, name, version)
        check_binary(destination, artifact)
        return destination.resolve()
    if offline:
        raise PackageError(f"offline package cache missing for {source.name}/{name}@{version}")
    # Never send a private source bearer token to an independently hosted artifact.
    token_env = source.token_env if origin(source.url) == origin(artifact["url"]) else None
    data = fetch(artifact["url"], token_env, MAX_ARCHIVE)
    if digest(data) != artifact["sha256"]:
        raise PackageError(f"artifact checksum mismatch for {source.name}/{name}@{version}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix=".extract-", dir=destination.parent))
    try:
        extract(data, temp, artifact.get("strip_prefix", ""))
        manifest_identity(temp, name, version)
        check_binary(temp, artifact)
        files = tree_files(temp)
        artifact["payload_sha256"] = object_digest(files)
        atomic_json(temp / MARKER, {"schema": 1, "artifact": artifact, "files": files})
        try:
            os.rename(temp, destination)
        except OSError:
            if not destination.exists():
                raise
            shutil.rmtree(temp)
            return materialize(project, source, name, version, artifact, offline=True)
        return destination.resolve()
    finally:
        if temp.exists():
            shutil.rmtree(temp)

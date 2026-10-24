"""Target identity and source snapshots shared by both macOS bootstrap paths."""
import json
import hashlib
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import tarfile

from common import REPO, sha256

TRIPLES = {"aarch64": "aarch64-apple-macosx15.0", "x86_64": "x86_64-apple-macosx15.0"}
CPUS = {"aarch64": 0x0100000C, "x86_64": 0x01000007}


def source_snapshot(work):
    source = work / "source"
    source.mkdir(exist_ok=False)
    archive = work / "source.tar"
    subprocess.run(["git", "archive", "--format=tar", f"--output={archive}", "HEAD"],
                   cwd=REPO, check=True)
    with tarfile.open(archive) as bundle:
        bundle.extractall(source, filter="data")
    return source


def source_commit():
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()


def verify_object(path, arch):
    with path.open("rb") as source:
        header = source.read(32)
    if len(header) != 32 or header[:4] != b"\xcf\xfa\xed\xfe":
        raise ValueError(f"Expected 64-bit little-endian Mach-O object: {path}")
    cpu, _, kind = struct.unpack_from("<III", header, 4)
    if cpu != CPUS[arch] or kind != 1:
        raise ValueError(f"Mach-O architecture/file type mismatch: {path}")


def executable_content(path):
    """Hash all Mach-O bytes except the measured UUID and code signature."""
    data = bytearray(path.read_bytes())
    if data[:4] != b"\xcf\xfa\xed\xfe" or len(data) < 32:
        raise ValueError("Expected a native 64-bit Mach-O executable")
    offset, fields = 32, []
    commands, size = struct.unpack_from("<II", data, 16)
    if offset + size > len(data):
        raise ValueError("Truncated Mach-O load commands")
    for _ in range(commands):
        command, length = struct.unpack_from("<II", data, offset)
        if length < 8 or offset + length > 32 + size:
            raise ValueError("Invalid Mach-O load command")
        if command == 0x1B:  # LC_UUID
            if length != 24:
                raise ValueError("Invalid LC_UUID")
            fields.append({"kind": "LC_UUID", "offset": offset + 8, "size": 16,
                           "sha256": hashlib.sha256(data[offset + 8:offset + 24]).hexdigest()})
            data[offset + 8:offset + 24] = b"\0" * 16
        elif command == 0x1D:  # LC_CODE_SIGNATURE
            if length != 16:
                raise ValueError("Invalid LC_CODE_SIGNATURE command")
            start, count = struct.unpack_from("<II", data, offset + 8)
            if start < 32 + size or start + count != len(data):
                raise ValueError("Invalid LC_CODE_SIGNATURE")
            fields.append({"kind": "LC_CODE_SIGNATURE", "offset": start, "size": count,
                           "sha256": hashlib.sha256(data[start:start + count]).hexdigest()})
            data[start:start + count] = b"\0" * count
        offset += length
    if offset != 32 + size:
        raise ValueError("Mach-O load command size mismatch")
    return {"content_sha256": hashlib.sha256(data).hexdigest(), "metadata": fields}


def verify_bundle(root, arch, commit):
    root = root.resolve()
    manifest = json.loads((root / "BOOTSTRAP.json").read_text())
    if (manifest.get("schema") != 1 or manifest.get("arch") != arch or
            manifest.get("triple") != TRIPLES[arch] or manifest.get("source_commit") != commit):
        raise ValueError("macOS bootstrap target/source identity mismatch")
    groups = {"compiler": [], "runtime": []}
    seen = set()
    for item in manifest["objects"]:
        relative = PurePosixPath(item["path"])
        path = root.joinpath(*relative.parts)
        group = item["group"]
        if (relative.is_absolute() or ".." in relative.parts or "\\" in item["path"] or
                not path.resolve().is_relative_to(root) or group not in groups or
                path in seen or not path.is_file()):
            raise ValueError("Invalid macOS bootstrap object path/group")
        seen.add(path)
        if sha256(path) != item["sha256"]:
            raise ValueError(f"macOS bootstrap object checksum mismatch: {relative}")
        verify_object(path, arch)
        groups[group].append(path)
    if any(not objects for objects in groups.values()):
        raise ValueError("macOS bootstrap requires compiler and runtime objects")
    return manifest, groups


def collect_objects(source, destination, arch, group):
    paths = sorted(path for path in source.rglob("*") if path.suffix in (".o", ".obj")) if source.is_dir() else [source]
    if not paths:
        raise RuntimeError(f"No {group} objects exported")
    destination.mkdir(parents=True, exist_ok=False)
    objects = []
    for index, path in enumerate(paths):
        verify_object(path, arch)
        target = destination / f"{index:04d}-{path.name}"
        shutil.copy2(path, target)
        objects.append({"path": group + "/" + target.name, "group": group, "sha256": sha256(target)})
    return objects

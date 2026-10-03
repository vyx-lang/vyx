"""Artifact bundles: validation, content addressing, atomic publication.

Implements §4.3 / §7 of the Active Adapter design: a :class:`BundleCandidate`
is staged into a temporary directory, hashed, covered by a deterministic
manifest and published with a single atomic rename.  Consumers load a bundle
by key and re-verify every byte before trusting it — a corrupted or truncated
bundle fails verification instead of producing half a contract.

Publication is single-flight: the directory rename only succeeds for the
first writer; concurrent publishers of identical content observe the already
published key.  Failed materializations never expose a partial bundle.
"""

from __future__ import annotations

import os
import shutil
import uuid
from pathlib import Path
from typing import Any

from tools.dci import active_protocol as ap
from tools.dci.active_protocol import (
    STATUS_CLOSED,
    BundleCandidate,
    ProtocolError,
    SemanticEnvironment,
)


class BundleError(ValueError):
    """Bundle validation or publication failure."""


def _digest_file(path: Path) -> tuple[str, int]:
    import hashlib
    data = path.read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def _verify_entries(base: Path, entries: list[dict[str, Any]],
                    with_format: bool, where: str) -> None:
    seen: set[str] = set()
    for entry in entries:
        relative = entry["path"]
        if relative in seen:
            raise BundleError(f"{where}: duplicate path {relative!r}")
        seen.add(relative)
        posix = Path(relative)
        if posix.is_absolute() or ".." in posix.parts:
            raise BundleError(f"{where}: unsafe path {relative!r}")
        file_path = base / posix
        if not file_path.is_file():
            raise BundleError(f"{where}: missing file {relative!r}")
        digest, size = _digest_file(file_path)
        if digest != entry["sha256"]:
            raise BundleError(f"{where}: content mismatch for {relative!r}")
        if size != entry["bytes"]:
            raise BundleError(f"{where}: size mismatch for {relative!r}")
        if with_format and not entry.get("format"):
            raise BundleError(f"{where}: artifact {relative!r} lacks format")


def validate_candidate(candidate: BundleCandidate) -> dict[str, Any]:
    """Structural validation before anything is written to disk.

    Every closed resolution must reference at least one implementation whose
    artifact exists in the candidate; refusal diagnostics must carry a known
    error code; member paths must be relative and traversal-free; bundle
    content must be non-empty.
    """
    if not candidate.contracts:
        raise BundleError("candidate has no contracts")
    for relative in list(candidate.contracts) + list(candidate.artifacts):
        member = Path(relative)
        if member.is_absolute() or ".." in member.parts or not relative:
            raise BundleError(f"unsafe member path {relative!r}")
        if member.parts[0] in (".", ""):
            raise BundleError(f"unsafe member path {relative!r}")
    artifact_paths = set(candidate.artifacts)
    for resolution in candidate.resolutions:
        if resolution.status == STATUS_CLOSED:
            if not resolution.implementations:
                raise BundleError(
                    f"closed {resolution.request_key[:12]} has no implementations")
            for impl in resolution.implementations:
                if impl.artifact not in artifact_paths:
                    raise BundleError(
                        f"closed {resolution.request_key[:12]} references missing "
                        f"artifact {impl.artifact!r}")
        else:
            for diagnostic in resolution.diagnostics:
                if diagnostic.code not in ap.CACHEABLE_ERRORS and (
                        diagnostic.code not in {
                            ap.ERROR_MISSING_FACTS,
                            ap.ERROR_CAPABILITY_UNSUPPORTED,
                            ap.ERROR_MATERIALIZATION_FAILED,
                            ap.ERROR_DEPENDENCY_UNAVAILABLE,
                            ap.ERROR_RESOURCE_LIMIT}):
                    raise BundleError(
                        f"resolution {resolution.request_key[:12]}: unknown "
                        f"diagnostic code {diagnostic.code!r}")
    return candidate.manifest()


def stage_and_publish(candidate: BundleCandidate, cache_root: Path) -> str:
    """Validate, stage into a temp dir and atomically publish by bundle key.

    Returns the published ``bundle_key``.  Concurrent publishers of identical
    content deduplicate: only the first rename lands, the rest observe the
    published directory.  Any failure removes the staging directory and leaves
    the cache untouched.
    """
    manifest = validate_candidate(candidate)
    bundle_key = manifest["bundle"]["key"]
    final = cache_root / bundle_key
    if final.is_dir():
        return bundle_key  # single-flight: identical content already live

    cache_root.mkdir(parents=True, exist_ok=True)
    staging = cache_root / f".staging-{os.getpid()}-{uuid.uuid4().hex}"
    try:
        (staging / "contracts").mkdir(parents=True)
        (staging / "artifacts").mkdir(parents=True)
        for relative, blob in candidate.contracts.items():
            (staging / "contracts" / relative).write_bytes(blob)
        for relative, (blob, _format) in candidate.artifacts.items():
            (staging / "artifacts" / relative).write_bytes(blob)
        import json
        (staging / "manifest.cjson.json").write_text(
            json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
            encoding="utf-8")
        try:
            os.rename(staging, final)
        except OSError:
            if final.is_dir():
                return bundle_key  # lost the race to identical content
            raise
        return bundle_key
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def load_bundle(cache_root: Path, bundle_key: str) -> dict[str, Any]:
    """Load and fully verify a published bundle; returns its manifest."""
    import json
    if not bundle_key or any(ch not in "0123456789abcdef" for ch in bundle_key):
        raise BundleError(f"malformed bundle key {bundle_key!r}")
    bundle_dir = cache_root / bundle_key
    manifest_path = bundle_dir / "manifest.cjson.json"
    if not manifest_path.is_file():
        raise BundleError(f"bundle {bundle_key[:12]} not published")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise BundleError(f"bundle {bundle_key[:12]}: unreadable manifest") from exc

    _require_manifest_shape(manifest)
    recomputed = ap.content_digest({**manifest, "bundle": {"key": ""}})
    if recomputed != bundle_key:
        raise BundleError(
            f"bundle {bundle_key[:12]}: manifest hash mismatch "
            f"(expected {bundle_key[:12]}, got {recomputed[:12]})")

    contracts = bundle_dir / "contracts"
    artifacts = bundle_dir / "artifacts"
    _verify_entries(contracts, manifest["contracts"], False, "contracts")
    _verify_entries(artifacts, manifest["artifacts"], True, "artifacts")

    covered = manifest["covered_operations"]
    if not covered and not manifest["refused_requests"]:
        raise BundleError(f"bundle {bundle_key[:12]}: covers nothing")
    for entry in covered:
        required = {"request_key", "status", "implementations"}
        missing = required - entry.keys()
        if missing:
            raise BundleError(
                f"bundle {bundle_key[:12]}: covered operation missing {sorted(missing)}")
        for impl in entry["implementations"]:
            if not any(a["path"] == impl["artifact"] for a in manifest["artifacts"]):
                raise BundleError(
                    f"bundle {bundle_key[:12]}: operation references missing "
                    f"artifact {impl['artifact']!r}")
    return manifest


def bundle_path(cache_root: Path, bundle_key: str, relative: str) -> Path:
    """Filesystem path of a verified bundle member (contracts/... or artifacts/...)."""
    member = Path(relative)
    if member.is_absolute() or ".." in member.parts:
        raise BundleError(f"unsafe bundle member {relative!r}")
    return cache_root / bundle_key / relative


def _require_manifest_shape(manifest: Any) -> None:
    if not isinstance(manifest, dict):
        raise BundleError("manifest is not an object")
    required = {
        "protocol", "bundle", "session", "contracts", "artifacts",
        "covered_operations", "refused_requests", "provenance",
    }
    missing = required - manifest.keys()
    if missing:
        raise BundleError(f"manifest missing keys {sorted(missing)}")
    if manifest["protocol"] != ap.PROTOCOL:
        raise BundleError(f"manifest protocol {manifest['protocol']!r} unsupported")
    session = manifest["session"]
    if not isinstance(session, dict):
        raise BundleError("manifest session is not an object")
    SemanticEnvironment(
        language=session["language"], producer=session["producer"],
        target_triple=session["target_triple"], abi_family=session["abi_family"],
        environment=session.get("environment", {}),
        source_digests=session.get("source_digests", {}),
        generated_inputs=session.get("generated_inputs", {}),
    )


__all__ = [
    "BundleError", "bundle_path", "load_bundle", "stage_and_publish",
    "validate_candidate",
]

_ = ProtocolError  # re-exported for callers that normalize both error types

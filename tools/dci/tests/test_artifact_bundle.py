import json
import tempfile
import threading
import unittest
from pathlib import Path

from tools.dci import active_protocol as ap
from tools.dci import artifact_bundle


def _session() -> ap.SemanticEnvironment:
    return ap.SemanticEnvironment(
        language="rust", producer="rustc 1.85.0",
        target_triple="x86_64-pc-windows-msvc", abi_family="rust",
        environment={"features": []}, source_digests={"src/lib.rs": "a" * 64})


def _candidate(artifact_bytes: bytes = b"object-bytes") -> ap.BundleCandidate:
    session = _session()
    return ap.BundleCandidate(
        session=session,
        contracts={"base.dcib": b"contract-bytes"},
        artifacts={"pair.obj": (artifact_bytes, "object")},
        resolutions=[ap.Resolution.closed(
            ap.Query(query_id="q1",
                     entity={"kind": "generic", "path": ["pair", "Pair"],
                             "args": [ap.type_ref(ap.TYPE_PRIMITIVE, "i32")]},
                     operation={"op": "rust/new/1"}),
            session,
            result_types=[ap.type_ref(ap.TYPE_NATIVE, "Pair<i32>")],
            facts={"layout": {"size": 8}},
            implementations=[ap.RequiredImplementation(
                symbol="__rust_pair_new", artifact="pair.obj")],
        )],
        adapter="active_rust.py", producer="rustc 1.85.0",
    )


class StageAndPublishTests(unittest.TestCase):
    def test_publish_then_load_round_trip(self):
        candidate = _candidate()
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "dci-bundles"
            key = artifact_bundle.stage_and_publish(candidate, cache)
            manifest = artifact_bundle.load_bundle(cache, key)
            self.assertEqual(manifest["bundle"]["key"], key)
            self.assertEqual(len(manifest["covered_operations"]), 1)
            member = artifact_bundle.bundle_path(cache, key, "artifacts/pair.obj")
            self.assertEqual(member.read_bytes(), b"object-bytes")

    def test_no_staging_left_behind(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "dci-bundles"
            artifact_bundle.stage_and_publish(_candidate(), cache)
            leftovers = [p for p in cache.iterdir() if p.name.startswith(".staging")]
            self.assertEqual(leftovers, [])

    def test_identical_content_single_flight(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "dci-bundles"
            keys: list[str] = []
            errors: list[Exception] = []

            def publish():
                try:
                    keys.append(artifact_bundle.stage_and_publish(_candidate(), cache))
                except Exception as exc:  # noqa: BLE001 - recorded below
                    errors.append(exc)

            threads = [threading.Thread(target=publish) for _ in range(4)]
            for thread in threads: thread.start()
            for thread in threads: thread.join()
            self.assertEqual(errors, [])
            self.assertEqual(len(set(keys)), 1)
            dirs = [p for p in cache.iterdir() if not p.name.startswith(".staging")]
            self.assertEqual(len(dirs), 1)

    def test_failure_leaves_cache_untouched(self):
        session = _session()
        broken = ap.BundleCandidate(
            session=session, contracts={"c.dcib": b"c"},
            resolutions=[ap.Resolution.closed(
                ap.Query(query_id="q", entity={"kind": "record", "path": ["x"]},
                         operation={"op": "rust/new/1"}),
                session, result_types=[], facts={},
                implementations=[ap.RequiredImplementation(
                    symbol="s", artifact="ghost.obj")])])
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "dci-bundles"
            with self.assertRaises(artifact_bundle.BundleError):
                artifact_bundle.stage_and_publish(broken, cache)
            self.assertFalse(cache.exists())

    def test_invalid_rejects_missing_artifact(self):
        session = _session()
        dangling = ap.BundleCandidate(
            session=session, contracts={"c.dcib": b"c"},
            resolutions=[ap.Resolution.closed(
                ap.Query(query_id="q", entity={"kind": "record", "path": ["x"]},
                         operation={"op": "rust/new/1"}),
                session, result_types=[], facts={},
                implementations=[ap.RequiredImplementation(
                    symbol="s", artifact="ghost.obj")])])
        with self.assertRaises(artifact_bundle.BundleError):
            artifact_bundle.validate_candidate(dangling)


class LoadVerificationTests(unittest.TestCase):
    def test_tampered_artifact_fails_verification(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "dci-bundles"
            key = artifact_bundle.stage_and_publish(_candidate(), cache)
            target = artifact_bundle.bundle_path(cache, key, "artifacts/pair.obj")
            target.write_bytes(b"tampered")
            with self.assertRaises(artifact_bundle.BundleError):
                artifact_bundle.load_bundle(cache, key)

    def test_tampered_manifest_fails_verification(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "dci-bundles"
            key = artifact_bundle.stage_and_publish(_candidate(), cache)
            manifest_path = cache / key / "manifest.cjson.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["provenance"]["adapter"] = "evil.py"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(artifact_bundle.BundleError):
                artifact_bundle.load_bundle(cache, key)

    def test_truncated_contract_fails_verification(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "dci-bundles"
            key = artifact_bundle.stage_and_publish(_candidate(), cache)
            contract = artifact_bundle.bundle_path(cache, key, "contracts/base.dcib")
            contract.write_bytes(b"")
            with self.assertRaises(artifact_bundle.BundleError):
                artifact_bundle.load_bundle(cache, key)

    def test_unknown_key_and_malformed_key_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "dci-bundles"
            cache.mkdir(parents=True)
            with self.assertRaises(artifact_bundle.BundleError):
                artifact_bundle.load_bundle(cache, "0" * 64)
            with self.assertRaises(artifact_bundle.BundleError):
                artifact_bundle.load_bundle(cache, "../../etc")

    def test_emptiness_and_path_traversal_guarded(self):
        session = _session()
        with self.assertRaises(artifact_bundle.BundleError):
            artifact_bundle.validate_candidate(ap.BundleCandidate(session=session))
        evil = ap.BundleCandidate(
            session=session, contracts={"../escape.dcib": b"x"},
            artifacts={"a.obj": (b"o", "object")},
            resolutions=[ap.Resolution.closed(
                ap.Query(query_id="q", entity={"kind": "record", "path": ["x"]},
                         operation={"op": "rust/new/1"}),
                session, result_types=[], facts={},
                implementations=[ap.RequiredImplementation(
                    symbol="s", artifact="a.obj")])])
        with self.assertRaises(artifact_bundle.BundleError):
            artifact_bundle.validate_candidate(evil)


if __name__ == "__main__":
    unittest.main()

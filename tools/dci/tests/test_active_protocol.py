import unittest

from tools.dci import active_protocol as ap


def _session(**overrides) -> ap.SemanticEnvironment:
    fields = dict(
        language="rust",
        producer="rustc 1.85.0 (stable)",
        target_triple="x86_64-pc-windows-msvc",
        abi_family="rust",
        environment={"features": ["serde"], "edition": "2021"},
        source_digests={"src/lib.rs": "a" * 64},
    )
    fields.update(overrides)
    return ap.SemanticEnvironment(**fields)


def _query(query_id: str = "q1", **overrides) -> ap.Query:
    fields = dict(
        query_id=query_id,
        entity={"kind": "generic", "path": ["pair", "Pair"],
                "args": [ap.type_ref(ap.TYPE_PRIMITIVE, "i32")]},
        operation={"op": "rust/new/1"},
    )
    fields.update(overrides)
    return ap.Query(**fields)


class CanonicalIdentityTests(unittest.TestCase):
    def test_content_digest_is_order_insensitive(self):
        a = {"b": 1, "a": [3, 2, 1]}
        b = {"a": [3, 2, 1], "b": 1}
        self.assertEqual(ap.content_digest(a), ap.content_digest(b))

    def test_request_key_ignores_query_id(self):
        session = _session()
        self.assertEqual(
            _query("one").request_key(session), _query("two").request_key(session))

    def test_request_key_binds_session_context(self):
        session = _session()
        baseline = _query().request_key(session)
        variants = [
            _query().request_key(_session(environment={"features": []})),
            _query().request_key(_session(source_digests={"src/lib.rs": "b" * 64})),
            _query().request_key(_session(target_triple="aarch64-pc-windows-msvc")),
            _query(operation={"op": "rust/clone/1"}).request_key(session),
            _query(entity={"kind": "generic", "path": ["pair", "Pair"],
                           "args": [ap.type_ref(ap.TYPE_PRIMITIVE, "i64")]}
                   ).request_key(session),
        ]
        for index, key in enumerate(variants):
            self.assertNotEqual(baseline, key, f"variant {index} collided")

    def test_entity_key_differs_from_request_key(self):
        session = _session()
        query = _query()
        self.assertNotEqual(query.entity_key(session), query.request_key(session))

    def test_session_identity_changes_with_any_field(self):
        base = _session().session_identity
        self.assertNotEqual(base, _session(producer="rustc 1.86.0").session_identity)
        self.assertNotEqual(base, _session(generated_inputs={"x.rs": "c" * 64}).session_identity)


class ValidationTests(unittest.TestCase):
    def test_capabilities_gate(self):
        capabilities = ap.AdapterCapabilities(
            languages=("rust",), operations=("rust/new/1",),
            request_capabilities=("rust/borrow/1",))
        _query().validate(capabilities)
        with self.assertRaises(ap.ProtocolError):
            _query(operation={"op": "rust/drop/1"}).validate(capabilities)
        with self.assertRaises(ap.ProtocolError):
            _query(required_capabilities=("rust/pin/1",)).validate(capabilities)

    def test_open_entity_allowed_in_query_but_must_be_marked(self):
        ap.Query(query_id="q", entity={"kind": "open"},
                 operation={"op": "rust/new/1"}).validate()
        with self.assertRaises(ap.ProtocolError):
            _query(entity={"kind": "generic", "path": []}).validate()
        with self.assertRaises(ap.ProtocolError):
            _query(entity={"kind": "generic", "args": []}).validate()

    def test_type_ref_forms(self):
        nested = ap.type_ref(ap.TYPE_NATIVE, "Widget")
        container = ap.type_ref(ap.TYPE_NATIVE, "Container", [nested])
        ap.Query(query_id="q", entity={"kind": "generic", "path": ["c"], "args": [container]},
                 operation={"op": "rust/new/1"}).validate()
        with self.assertRaises(ap.ProtocolError):
            ap.type_ref("quantum", "x")
        with self.assertRaises(ap.ProtocolError):
            _query(entity={"kind": "generic", "path": ["p"],
                           "args": [ap.type_ref(ap.TYPE_NATIVE)]}).validate()


class ResolutionTests(unittest.TestCase):
    def test_closed_resolution_binds_implementation(self):
        session = _session()
        query = _query()
        resolution = ap.Resolution.closed(
            query, session,
            result_types=[ap.type_ref(ap.TYPE_NATIVE, "Pair<i32>")],
            facts={"layout": {"size": 8, "align": 4}},
            implementations=[ap.RequiredImplementation(
                symbol="__rust_pair_new", artifact="pair.obj")],
        )
        self.assertEqual(resolution.status, ap.STATUS_CLOSED)
        document = resolution.to_dict()
        self.assertIn("implementations", document)
        self.assertNotIn("diagnostics", document)
        # implementations are attached at materialization time; publication
        # (artifact_bundle.validate_candidate) enforces the non-empty rule.
        deferred = ap.Resolution.closed(query, session,
                                        result_types=[], facts={})
        self.assertEqual(deferred.status, ap.STATUS_CLOSED)
        self.assertEqual(deferred.implementations, ())

    def test_refused_resolution_carries_error_code(self):
        session = _session()
        resolution = ap.Resolution.refused(
            _query(), session, status=ap.STATUS_REJECTED,
            code=ap.ERROR_CONSTRAINT_FAILED, message="i32: Copy violated",
            producer_span="src/lib.rs:7:9")
        document = resolution.to_dict()
        self.assertEqual(document["status"], ap.STATUS_REJECTED)
        self.assertEqual(document["diagnostics"][0]["producer_span"], "src/lib.rs:7:9")
        with self.assertRaises(ap.ProtocolError):
            ap.Resolution.refused(_query(), session, status=ap.STATUS_CLOSED,
                                  code=ap.ERROR_CONSTRAINT_FAILED, message="x")

    def test_cacheable_error_partition(self):
        self.assertIn(ap.ERROR_CONSTRAINT_FAILED, ap.CACHEABLE_ERRORS)
        self.assertNotIn(ap.ERROR_RESOURCE_LIMIT, ap.CACHEABLE_ERRORS)
        self.assertNotIn(ap.ERROR_MATERIALIZATION_FAILED, ap.CACHEABLE_ERRORS)


class BundleManifestTests(unittest.TestCase):
    def test_manifest_is_deterministic_and_binds_content(self):
        session = _session()
        candidate = ap.BundleCandidate(
            session=session,
            contracts={"base.dcib": b"contract-bytes"},
            artifacts={"pair.obj": (b"object-bytes", "object")},
            resolutions=[ap.Resolution.closed(
                _query(), session,
                result_types=[ap.type_ref(ap.TYPE_NATIVE, "Pair<i32>")],
                facts={"layout": {}},
                implementations=[ap.RequiredImplementation(
                    symbol="__rust_pair_new", artifact="pair.obj")],
            )],
            adapter="active_rust.py", producer="rustc 1.85.0",
        )
        first = candidate.manifest()
        second = ap.BundleCandidate(
            session=session,
            artifacts={"pair.obj": (b"object-bytes", "object")},
            contracts={"base.dcib": b"contract-bytes"},
            resolutions=[ap.Resolution.closed(
                _query(), session,
                result_types=[ap.type_ref(ap.TYPE_NATIVE, "Pair<i32>")],
                facts={"layout": {}},
                implementations=[ap.RequiredImplementation(
                    symbol="__rust_pair_new", artifact="pair.obj")],
            )],
            producer="rustc 1.85.0", adapter="active_rust.py",
        ).manifest()
        self.assertEqual(first, second)
        self.assertEqual(first["bundle"]["key"], second["bundle"]["key"])
        self.assertTrue(all(entry["sha256"] for entry in first["contracts"]))

    def test_manifest_key_changes_when_artifact_byte_changes(self):
        session = _session()
        impl = [ap.RequiredImplementation(symbol="s", artifact="a.obj")]
        make = lambda blob: ap.BundleCandidate(  # noqa: E731
            session=session, contracts={"c.dcib": b"c"},
            artifacts={"a.obj": (blob, "object")},
            resolutions=[ap.Resolution.closed(
                _query(), session, result_types=[], facts={}, implementations=impl)],
        ).manifest()["bundle"]["key"]
        self.assertNotEqual(make(b"one"), make(b"two"))

    def test_covered_and_refused_resolutions_are_partitioned(self):
        session = _session()
        clone = ap.Resolution.refused(
            _query("q2"), session, status=ap.STATUS_REJECTED,
            code=ap.ERROR_CONSTRAINT_FAILED, message="missing Clone")
        candidate = ap.BundleCandidate(
            session=session, contracts={"c.dcib": b"c"},
            artifacts={"a.obj": (b"o", "object")},
            resolutions=[clone],
        )
        manifest = candidate.manifest()
        self.assertEqual(manifest["covered_operations"], [])
        self.assertEqual(len(manifest["refused_requests"]), 1)
        self.assertEqual(
            manifest["refused_requests"][0]["diagnostics"][0]["code"],
            ap.ERROR_CONSTRAINT_FAILED)


if __name__ == "__main__":
    unittest.main()

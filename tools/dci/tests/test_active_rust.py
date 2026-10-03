import shutil
import tempfile
import unittest
from pathlib import Path

from tools.dci import active_protocol as ap
from tools.dci.active_rust import RustActiveSession

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tools/dci/tests/fixtures/active_rust/lib.rs"

HAS_RUSTC = shutil.which("rustc") is not None


def _generic(path: str, args: tuple[str, ...], query_id: str = "q",
             form: str = ap.TYPE_PRIMITIVE) -> ap.Query:
    return ap.Query(
        query_id=query_id,
        entity={"kind": "generic", "path": path,
                "args": [ap.type_ref(form, a) for a in args]},
        operation={"op": "rust/call/1"},
    )


@unittest.skipUnless(HAS_RUSTC, "rustc not available")
class RustResolveTests(unittest.TestCase):
    def setUp(self):
        self.session = RustActiveSession(FIXTURE)
        self.addCleanup(self.session.close)

    def test_session_identity_binds_environment(self):
        document = self.session.session.to_dict()
        self.assertEqual(document["language"], "rust")
        self.assertTrue(document["producer"].startswith("rustc "))
        self.assertIn("lib.rs", document["source_digests"])
        self.assertEqual(len(self.session.session.session_identity), 64)

    def test_close_generic_call(self):
        resolutions = self.session.resolve_batch(
            [_generic("pair", ("i32", "i32"))])
        self.assertEqual(resolutions[0].status, ap.STATUS_CLOSED)
        facts = resolutions[0].facts
        self.assertEqual(facts["symbol"], "__vyx_rust_pair_i32_i32")
        self.assertEqual(facts["return_type"], "(i32,i32)")
        self.assertEqual(len(resolutions[0].request_key), 64)

    def test_constraint_failure_carries_producer_span(self):
        good = _generic("twice", ("i32",), "ok")
        bad = _generic("twice", ("Marker",), "bad", form=ap.TYPE_RECORD)
        resolutions = self.session.resolve_batch([good, bad])
        self.assertEqual(resolutions[0].status, ap.STATUS_CLOSED)
        refused = resolutions[1]
        self.assertEqual(refused.status, ap.STATUS_REJECTED)
        self.assertEqual(refused.diagnostics[0].code, ap.ERROR_CONSTRAINT_FAILED)
        self.assertIn("Marker", refused.diagnostics[0].message)

    def test_per_operation_admission(self):
        marker = ap.type_ref(ap.TYPE_RECORD, "Marker")
        ok = ap.Query(query_id="size", entity={"kind": "generic",
                                               "path": "size_of_val",
                                               "args": [marker]},
                      operation={"op": "rust/call/1"})
        bad = ap.Query(query_id="display", entity={"kind": "generic",
                                                   "path": "display_of",
                                                   "args": [marker]},
                       operation={"op": "rust/call/1"})
        resolutions = self.session.resolve_batch([ok, bad])
        self.assertEqual(resolutions[0].status, ap.STATUS_CLOSED)
        self.assertEqual(resolutions[1].status, ap.STATUS_REJECTED)
        self.assertEqual(resolutions[1].diagnostics[0].code,
                         ap.ERROR_CONSTRAINT_FAILED)

    def test_unknown_entity_refused(self):
        resolutions = self.session.resolve_batch(
            [_generic("nope", ("i32", "i32"))])
        self.assertEqual(resolutions[0].status, ap.STATUS_REJECTED)
        self.assertEqual(resolutions[0].diagnostics[0].code, ap.ERROR_ENTITY_NOT_FOUND)

    def test_unknown_primitive_is_representation_incompatible(self):
        resolutions = self.session.resolve_batch(
            [_generic("twice", ("i128",))])
        self.assertEqual(resolutions[0].diagnostics[0].code,
                         ap.ERROR_REPRESENTATION_INCOMPATIBLE)

    def test_open_type_ref_is_unsupported(self):
        resolutions = self.session.resolve_batch(
            [_generic("twice", ("T",), form=ap.TYPE_OPEN)])
        self.assertEqual(resolutions[0].status, ap.STATUS_REJECTED)
        self.assertEqual(resolutions[0].diagnostics[0].code,
                         ap.ERROR_CAPABILITY_UNSUPPORTED)

    def test_arity_mismatch_refused(self):
        resolutions = self.session.resolve_batch([_generic("pair", ("i32",))])
        self.assertEqual(resolutions[0].status, ap.STATUS_REJECTED)
        self.assertEqual(resolutions[0].diagnostics[0].code,
                         ap.ERROR_CONSTRAINT_FAILED)

    def test_request_key_survives_query_id_but_binds_session(self):
        session = self.session.session
        baseline = _generic("pair", ("i32", "i32"), "a").request_key(session)
        self.assertEqual(baseline,
                         _generic("pair", ("i32", "i32"), "b").request_key(session))
        self.assertNotEqual(baseline,
                            _generic("pair", ("i64", "i64"), "a").request_key(session))


@unittest.skipUnless(HAS_RUSTC, "rustc not available")
class RustMaterializeTests(unittest.TestCase):
    def setUp(self):
        self.cache = Path(tempfile.mkdtemp(prefix="dci-active-rust-cache-"))
        self.addCleanup(shutil.rmtree, self.cache, ignore_errors=True)
        self.session = RustActiveSession(FIXTURE)
        self.addCleanup(self.session.close)
        self.resolutions = self.session.resolve_batch([
            _generic("twice", ("i32",), "a"),
            _generic("pair", ("i32", "i32"), "b"),
        ])

    def _publish(self):
        candidate = self.session.materialize_batch(self.resolutions)
        key = self.session.validate_and_publish(candidate, self.cache)
        return candidate, key

    def test_publish_and_reload(self):
        from tools.dci import artifact_bundle as bundle
        candidate, key = self._publish()
        self.assertRegex(key, r"^[0-9a-f]{64}$")
        manifest = bundle.load_bundle(self.cache, key)
        self.assertEqual(len(manifest["covered_operations"]), 2)
        self.assertEqual(manifest["provenance"]["adapter"], "active-rust/1")
        self.assertEqual(len(manifest["artifacts"]), 1)
        self.assertEqual(manifest["artifacts"][0]["path"], "shims.obj")

    def test_shim_object_is_deterministic(self):
        first, _ = self._publish()
        with RustActiveSession(FIXTURE) as session:
            resolutions = session.resolve_batch([
                _generic("twice", ("i32",), "x"),
                _generic("pair", ("i32", "i32"), "y"),
            ])
            second = session.materialize_batch(resolutions)
        self.assertEqual(
            first.artifacts["shims.obj"][0], second.artifacts["shims.obj"][0])

    def test_single_flight_publish_is_idempotent(self):
        _, key_first = self._publish()
        _, key_second = self._publish()
        self.assertEqual(key_first, key_second)
        published = [d for d in self.cache.iterdir() if d.is_dir()]
        self.assertEqual(len(published), 1)

    def test_symbol_present_in_object(self):
        from tools.dci import artifact_bundle as bundle
        _, key = self._publish()
        manifest = bundle.load_bundle(self.cache, key)
        symbols = {op["facts"]["symbol"] for op in manifest["covered_operations"]}
        self.assertIn("__vyx_rust_twice_i32", symbols)
        self.assertIn("__vyx_rust_pair_i32_i32", symbols)


if __name__ == "__main__":
    unittest.main()

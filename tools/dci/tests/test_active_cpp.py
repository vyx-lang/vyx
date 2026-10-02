import shutil
import tempfile
import unittest
from pathlib import Path

from tools.dci import active_protocol as ap
from tools.dci.active_cpp import CppActiveSession

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tools/dci/tests/fixtures/active_cpp/active_fixture.hpp"

HAS_CLANGXX = shutil.which("clang++") is not None


def _generic(path: str, args: tuple[str, ...], query_id: str = "q") -> ap.Query:
    return ap.Query(
        query_id=query_id,
        entity={"kind": "generic", "path": path,
                "args": [ap.type_ref(ap.TYPE_PRIMITIVE, a) for a in args]},
        operation={"op": "cpp/call/1"},
    )


@unittest.skipUnless(HAS_CLANGXX, "clang++ not available")
class CppResolveTests(unittest.TestCase):
    def setUp(self):
        self.session = CppActiveSession(FIXTURE)
        self.addCleanup(self.session.close)

    def test_session_identity_binds_environment(self):
        document = self.session.session.to_dict()
        self.assertEqual(document["language"], "cpp")
        self.assertIn("active_fixture.hpp", document["source_digests"])
        self.assertEqual(document["environment"]["std"], "c++20")
        self.assertEqual(len(self.session.session.session_identity), 64)

    def test_template_discovery(self):
        self.assertEqual(self.session.template_names, {"tadd", "tlen"})

    def test_close_generic_call(self):
        resolutions = self.session.resolve_batch([_generic("tadd", ("i32", "i32"))])
        self.assertEqual(resolutions[0].status, ap.STATUS_CLOSED)
        facts = resolutions[0].facts
        self.assertEqual(facts["symbol"], "__vyx_cpp_tadd_int_int")
        self.assertEqual(facts["calling_convention"], "cdecl")

    def test_constraint_failure_for_noadd(self):
        bad = ap.Query(
            query_id="bad",
            entity={"kind": "generic", "path": "tadd",
                    "args": [ap.type_ref(ap.TYPE_RECORD, "NoAdd")]},
            operation={"op": "cpp/call/1"},
        )
        good = _generic("tadd", ("i32", "i32"), "good")
        resolutions = self.session.resolve_batch([bad, good])
        self.assertEqual(resolutions[0].status, ap.STATUS_REJECTED)
        self.assertEqual(resolutions[0].diagnostics[0].code,
                         ap.ERROR_CONSTRAINT_FAILED)
        self.assertEqual(resolutions[1].status, ap.STATUS_CLOSED)

    def test_unknown_template_refused(self):
        resolutions = self.session.resolve_batch([_generic("tnope", ("i32", "i32"))])
        self.assertEqual(resolutions[0].status, ap.STATUS_REJECTED)
        self.assertEqual(resolutions[0].diagnostics[0].code, ap.ERROR_ENTITY_NOT_FOUND)

    def test_unknown_record_is_representation_incompatible(self):
        query = ap.Query(
            query_id="rec",
            entity={"kind": "generic", "path": "tadd",
                    "args": [ap.type_ref(ap.TYPE_RECORD, "Missing")]},
            operation={"op": "cpp/call/1"},
        )
        resolutions = self.session.resolve_batch([query])
        self.assertEqual(resolutions[0].diagnostics[0].code,
                         ap.ERROR_REPRESENTATION_INCOMPATIBLE)

    def test_request_key_binds_session(self):
        session = self.session.session
        baseline = _generic("tadd", ("i32", "i32"), "a").request_key(session)
        self.assertNotEqual(baseline, _generic("tadd", ("i64", "i64"), "a").request_key(session))


@unittest.skipUnless(HAS_CLANGXX, "clang++ not available")
class CppMaterializeTests(unittest.TestCase):
    def setUp(self):
        self.cache = Path(tempfile.mkdtemp(prefix="dci-active-cpp-cache-"))
        self.addCleanup(shutil.rmtree, self.cache, ignore_errors=True)
        self.session = CppActiveSession(FIXTURE)
        self.addCleanup(self.session.close)
        self.resolutions = self.session.resolve_batch([
            _generic("tadd", ("i32", "i32"), "a"),
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
        self.assertEqual(len(manifest["covered_operations"]), 1)
        self.assertEqual(manifest["provenance"]["adapter"], "active-cpp/1")
        self.assertEqual(manifest["artifacts"][0]["path"], "shims.obj")

    def test_shim_object_is_deterministic(self):
        first, _ = self._publish()
        with CppActiveSession(FIXTURE) as session:
            resolutions = session.resolve_batch([_generic("tadd", ("i32", "i32"), "x")])
            second = session.materialize_batch(resolutions)
        self.assertEqual(
            first.artifacts["shims.obj"][0], second.artifacts["shims.obj"][0])

    def test_symbol_present_in_object(self):
        from tools.dci import artifact_bundle as bundle
        _, key = self._publish()
        manifest = bundle.load_bundle(self.cache, key)
        symbols = {op["facts"]["symbol"] for op in manifest["covered_operations"]}
        self.assertIn("__vyx_cpp_tadd_int_int", symbols)


if __name__ == "__main__":
    unittest.main()

from pathlib import Path
import copy
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cpp_auto_import as auto
import dci_adapter_msvc as adapter


def method(owner, name, params, result="void"):
    return adapter.Symbol(name=owner + "::" + name, owner=owner, member_name=name,
                          mangled="native_" + name, kind="method", calling_convention="cxx_method",
                          params=[{"name": f"p{i}", "type": adapter.dci_type(t)} for i, t in enumerate(params)],
                          ret={"type": adapter.dci_type(result)})


class AutoImports(unittest.TestCase):
    def test_project_contract_destination_is_separate_from_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            self.assertEqual(auto.contract_destination(project, "api", {}),
                             (project / "contracts/api.dcib").resolve())
            self.assertEqual(auto.contract_destination(project, "api", {"contract": "contracts/native.dcib"}),
                             (project / "contracts/native.dcib").resolve())
            for value, diagnostic in [(True, "must name"), ("", "must name"),
                                      ("src/api.vyx", "must name"),
                                      (".cache/dci/api/contract.dcib", "outside")]:
                with self.subTest(value=value), self.assertRaisesRegex(auto.ImportError, diagnostic):
                    auto.contract_destination(project, "api", {"contract": value})

    def test_authored_definitions_are_required_source_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            source = project / "src/api.vyx"
            source.parent.mkdir()
            source.write_text('// user-maintained definitions\nmodule native.api;\n')
            before = source.read_bytes()
            self.assertEqual(auto.definition_source(project, "api", {"definitions": "src/api.vyx"}), source.resolve())
            self.assertEqual(source.read_bytes(), before)
            self.assertIsNone(auto.definition_source(project, "api", {}))
            for value, diagnostic in [(True, "must name"), ("", "must name"),
                                      ("src/missing.vyx", "not found"),
                                      (".cache/dci/api/import.vyx", "outside")]:
                with self.subTest(value=value), self.assertRaisesRegex(auto.ImportError, diagnostic):
                    auto.definition_source(project, "api", {"definitions": value})

    def test_effective_build_context_overrides_raw_target_inputs(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "Vyx.toml"
            manifest.write_text('[target.app]\nextends=["native"]\n[dci.import.api]\nmodule="api"\nheaders=["api.hpp"]\n')
            context = {"dci_imports": ["api"], "entry": "src/main.vyx", "sources": [],
                       "cxx": "clang++", "cxxflags": ["-DFROM_PLATFORM=1"], "include_paths": ["native"]}
            path = Path(directory) / "context.json"
            path.write_text(json.dumps(context))
            with patch.object(auto, "prepare_one") as prepare:
                auto.prepare(manifest, "app", build_context=path)
            self.assertEqual(prepare.call_args.args[3], context)

    def test_measured_multiple_inheritance_keeps_all_bases(self):
        def layout(name, bases=()):
            return {"type_name": name, "bases": [{"type_name": b, "visibility": "public"} for b in bases]}
        document = {"exports": {"symbols": [], "layouts": [
            layout("Widget", ["Object", "PaintDevice"]), layout("PaintDevice"), layout("Object")]}}
        emitted, rejected = auto.emit_import(document, "qt.widgets", "api.dcib", "producer.hpp")
        self.assertFalse(rejected)
        self.assertIn("class Widget : Object, PaintDevice", emitted)
        self.assertLess(emitted.index("class Object"), emitted.index("class Widget"))
        self.assertLess(emitted.index("class PaintDevice"), emitted.index("class Widget"))

    def test_unavailable_bases_close_the_declaration_surface(self):
        document = {"exports": {"symbols": [], "layouts": [
            {"type_name": "Base", "bases": [{"type_name": "ns::Foreign", "visibility": "public"}]},
            {"type_name": "Derived", "bases": [{"type_name": "Base", "visibility": "public"}]}]}}
        emitted, rejected = auto.emit_import(document, "api", "api.dcib", "producer.hpp")
        self.assertNotIn("class Base", emitted)
        self.assertNotIn("class Derived", emitted)
        self.assertEqual({r["selector"] for r in rejected}, {"Base", "Derived"})

    def test_default_presence_does_not_guess_a_value(self):
        m = method("Widget", "resize", ["int", "int"])
        plan, rejects = auto.bridge_plan([(m, [False, True], False)], {"Widget"}, {}, "cpp")
        self.assertEqual(plan, [])
        producer = auto.emit_producer(["api.hpp"], plan)
        self.assertNotIn("resize", producer)
        self.assertNotIn('extern "C"', producer)

    def test_defaults_do_not_synthesize_pointer_bindings(self):
        m = method("Widget", "take", ["Widget*", "int"])
        plan, rejects = auto.bridge_plan([(m, [False, True], False)], {"Widget"}, {}, "cpp")
        self.assertEqual(plan, [])
        self.assertFalse(rejects)

    def test_actual_overload_supplies_short_call(self):
        long = method("Widget", "resize", ["int", "int"])
        short = method("Widget", "resize", ["int"])
        plan, _ = auto.bridge_plan([(long, [False, True], False), (short, [False], False)], {"Widget"}, {}, "cpp")
        self.assertEqual(plan, [])

    def test_ambiguous_default_overloads_are_not_arbitrarily_selected(self):
        a, b = method("Widget", "resize", ["int", "int"]), method("Widget", "resize", ["int", "bool"])
        plan, rejects = auto.bridge_plan([(a, [False, True], False), (b, [False, True], False)], {"Widget"}, {}, "cpp")
        self.assertEqual(plan, [])
        self.assertFalse(rejects)  # Native overloads remain for Consumer resolution.

    def test_default_wrapper_generation_is_rejected(self):
        m = method("Widget", "resize", ["int", "int"])
        with self.assertRaisesRegex(auto.ImportError, "must not generate"):
            auto.emit_producer(["api.hpp"], [{"mode": "default", "symbol": m}])

    def test_only_declared_signals_get_connection_bridges(self):
        m = method("Button", "clicked", ["bool"])
        ordinary, _ = auto.bridge_plan([(m, [False], False)], {"Button"}, {}, "qt")
        signal, _ = auto.bridge_plan([(m, [False], True)], {"Button"}, {}, "qt", ["Button::clicked(bool)"])
        self.assertEqual(ordinary, [])
        self.assertEqual(signal[0]["member"], "on_clicked")
        self.assertIn("static_cast<Member>(&Button::clicked)", auto.emit_producer(["api.hpp"], signal))

    def test_macro_provenance_and_payload_lifetimes(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "button.hpp"
            source.write_text("Q_SIGNALS\n")
            record = {"kind": "CXXRecordDecl", "name": "Button", "completeDefinition": True,
                      "loc": {"file": str(source)}, "inner": [
                          {"kind": "AccessSpecDecl", "access": "public", "loc": {"offset": 0, "tokLen": 9}},
                          {"kind": "CXXMethodDecl", "name": "payload", "mangledName": "payload",
                           "type": {"qualType": "void (Widget *)"}, "inner": [{"kind": "ParmVarDecl"}]}]}
            harvested = auto.harvest_methods([record], {"Button", "Widget"}, "qt")
            self.assertTrue(harvested[0][2])
            plan, rejects = auto.bridge_plan(harvested, {"Button", "Widget"}, {}, "qt")
            self.assertFalse(plan)
            self.assertFalse(rejects)
            plan, rejects = auto.bridge_plan(harvested, {"Button", "Widget"}, {}, "qt", ["Button::payload(Widget*)"])
            self.assertIn("lifetime", rejects[0]["reason"])

    def test_measured_abi_types_survive_receiver_adoption(self):
        original = method("Button", "clicked", ["bool"])
        plan, _ = auto.bridge_plan([(original, [False], True)], {"Button"}, {}, "qt", ["Button::clicked(bool)"])
        n = plan[0]["name"]
        native_link = "?" + n + "@@NativeCppMeasuredSymbol"
        measured = adapter.Symbol(name=n, owner="", member_name=n, mangled=native_link, kind="function",
                                  calling_convention="cdecl", params=[
                                      {"name": f"p{i}", "type": adapter.dci_type(t)} for i, t in enumerate(
                                          ["Button*", "QObject*", "void (*)(void*,bool)", "void*"])],
                                  ret={"type": adapter.dci_type("bool")},
                                  parameter_ownerships={0:"borrow", 1:"borrow", 2:"copy", 3:"borrow"})
        exported = adapter.symbol_json(measured)
        before = copy.deepcopy(exported["abi"])
        document = {"exports": {"symbols": [adapter.symbol_json(original), exported]}}
        self.assertEqual(auto.adopt_bridges(document, plan), [])
        adopted = document["exports"]["symbols"][-1]
        self.assertEqual(adopted["abi"]["receiver"]["type"], before["params"][0]["type"])
        self.assertEqual(adopted["abi"]["return"], before["return"])
        self.assertEqual([p["type"] for p in adopted["abi"]["params"]], [p["type"] for p in before["params"][1:]])
        self.assertEqual(adopted["link_name"], native_link)
        self.assertEqual(adopted["producer_bridge"]["link_name"], native_link)
        self.assertEqual(auto.prepared_connection_plan(document, "qt", ["Button::clicked(bool)"])[0]["name"], n)
        duplicate = copy.deepcopy(document)
        duplicate["exports"]["symbols"].append(duplicate["exports"]["symbols"][0])
        with self.assertRaisesRegex(auto.ImportError, "unique measured"):
            auto.prepared_connection_plan(duplicate, "qt", ["Button::clicked(bool)"])
        changed = copy.deepcopy(document)
        changed["exports"]["symbols"][-1]["params"][1]["type"]["signature"]["params"][1]["name"] = "i32"
        with self.assertRaisesRegex(auto.ImportError, "signature mismatch"):
            auto.prepared_connection_plan(changed, "qt", ["Button::clicked(bool)"])

    def test_nested_environment_fallback_and_missing_value(self):
        with patch.dict(os.environ, {"QT_ROOT":"/native/qt"}, clear=True):
            self.assertEqual(auto.expand("${QT_INCLUDE:-${QT_ROOT}/include}"), "/native/qt/include")
            with self.assertRaisesRegex(auto.ImportError, "required"):
                auto.expand("${UNKNOWN}")

    def test_exports_require_explicit_author_scope(self):
        for spec in ({}, {"export_types": []}, {"export_all": "true"},
                     {"export_all": True, "export_types": ["Session"]},
                     {"export_types": ["Session", "Session"]}):
            with self.subTest(spec=spec), self.assertRaises(auto.ImportError):
                auto.export_scope(spec)
        self.assertEqual(auto.export_scope({"export_types": ["Session"]})["types"], ["Session"])
        self.assertTrue(auto.export_scope({"export_all": True})["all"])

    def test_a_declared_signal_alone_does_not_select_a_connection_export(self):
        signal = method("Button", "clicked", ["bool"])
        self.assertEqual(auto.bridge_plan([(signal, [False], True)], {"Button"}, {}, "qt"), ([], []))
        with self.assertRaisesRegex(auto.ImportError, "outside the selected exports"):
            auto.bridge_plan([(signal, [False], True)], {"Button"}, {}, "qt", ["Other::clicked(bool)"])

    def test_requested_connection_must_be_a_real_signal(self):
        method_decl = method("Button", "clicked", ["bool"])
        plan, rejected = auto.bridge_plan([(method_decl, [False], False)], {"Button"}, {}, "qt", ["Button::clicked(bool)"])
        self.assertEqual(plan, [])
        self.assertIn("not a declared native signal", rejected[0]["reason"])


if __name__ == "__main__":
    unittest.main()

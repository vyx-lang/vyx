from pathlib import Path
import copy
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import converter
import dci_adapter_msvc as adapter


class ConverterTests(unittest.TestCase):
    def test_original_signature_retains_defaults_and_link_identity(self):
        symbol = adapter.Symbol(name="Widget::resize", owner="Widget", member_name="resize",
            mangled="original_resize", kind="method", calling_convention="win64",
            params=[{"name": "width", "type": adapter.dci_type("int")},
                    {"name": "height", "type": adapter.dci_type("int"),
                     "default": {"kind": "constant", "value": 20, "source": "clang_ast"}}],
            ret={"type": adapter.dci_type("void")})
        document = {"exports": {"layouts": [{"type_name": "Widget", "bases": []}],
                                "symbols": [adapter.symbol_json(symbol)]}}
        source, rejected = converter.emit_import(document, "widgets", "api.dcib", ["widget.hpp"])
        self.assertFalse(rejected)
        self.assertIn("resize(width: i32, height: i32 = 20)", source)
        self.assertEqual(source.count("fn resize("), 1)
        self.assertIn('@[cpp_include("widget.hpp")]', source)
        self.assertEqual(document["exports"]["symbols"][0]["link_name"], "original_resize")

    def test_unrepresentable_defaults_preserve_native_signature_with_diagnostics(self):
        symbol = adapter.Symbol(name="Widget::resize", owner="Widget", member_name="resize",
            mangled="original_resize", kind="method", calling_convention="win64",
            params=[{"name": "width", "type": adapter.dci_type("int"),
                     "default": {"kind": "producer_expression", "reason": "runtime function call"}},
                    {"name": "height", "type": adapter.dci_type("int"),
                     "default": {"kind": "constant", "value": 0, "source": "clang_ast"}}],
            ret={"type": adapter.dci_type("void")})
        document = {"exports": {"layouts": [{"type_name": "Widget", "bases": []}],
                                "symbols": [adapter.symbol_json(symbol)]}}
        source, rejected = converter.emit_import(document, "widgets", "api.dcib")
        self.assertIn("resize(width: i32, height: i32 = 0)", source)
        self.assertEqual(rejected[0]["scope"], "default_argument")
        self.assertEqual(document["exports"]["symbols"][0]["link_name"], "original_resize")

    def test_default_constants_cannot_change_types_or_overflow(self):
        for spelling, value in (("int", True), ("int", 1 << 31), ("unsigned int", -1),
                                ("Widget*", 0), ("bool", 1)):
            with self.subTest(spelling=spelling, value=value), self.assertRaises(converter.ConversionError):
                converter.default_literal({"type": adapter.dci_type(spelling), "default": {"kind": "constant", "value": value}})
        self.assertEqual(converter.default_literal({"type": adapter.dci_type("Widget*"), "default": {"kind": "constant", "value": None}}), "null")

    def test_function_pointer_identity_is_preserved_or_diagnosed(self):
        self.assertEqual(converter.type_shape(adapter.dci_type("void (*)(void*,bool)")), "cfn(rawptr,bool)->void")
        for ty in ("void (Widget::*)(bool)", "void (*)(int, ...)", "void (*)(int) noexcept"):
            with self.subTest(ty=ty), self.assertRaises(converter.ConversionError):
                converter.type_shape(adapter.dci_type(ty))

    def test_callback_referencing_an_unavailable_record_is_not_emitted(self):
        ty = adapter.dci_type("void (*)(Widget*)")
        self.assertFalse(converter.available_type(ty, set()))
        self.assertTrue(converter.available_type(ty, {"Widget"}))

    def test_const_virtual_signature_and_measured_contract_survive_conversion(self):
        symbol = adapter.Symbol(name="Widget::read", owner="Widget", member_name="read",
            mangled="?read@Widget@@UEBAHH@Z", kind="method", calling_convention="win64",
            params=[{"name": "index", "type": adapter.dci_type("int")}],
            ret={"type": adapter.dci_type("int")}, is_virtual=True, is_const=True)
        document = {"exports": {"layouts": [{"type_name": "Widget", "bases": []}],
                                "symbols": [adapter.symbol_json(symbol)]}}
        before = copy.deepcopy(document)
        source, rejected = converter.emit_import(document, "widgets", "../contracts/api.dcib")
        self.assertFalse(rejected)
        self.assertIn('extern "dci"', source)
        self.assertIn("public virtual fn read(index: i32) const -> i32;", source)
        self.assertEqual(document, before)

    def test_modified_definition_is_not_silently_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "widgets.vyx"
            converter.write_definition(path, "original\n")
            converter.write_definition(path, "original\n")
            path.write_text("author edits\n")
            with self.assertRaisesRegex(converter.ConversionError, "--force"):
                converter.write_definition(path, "replacement\n")
            self.assertEqual(path.read_text(), "author edits\n")
            converter.write_definition(path, "replacement\n", force=True)
            self.assertEqual(path.read_text(), "replacement\n")

    def test_host_names_and_paths_cannot_inject_declarations(self):
        document = {"exports": {"layouts": [], "symbols": []}}
        for module, path, headers in (("bad; fn x()", "api.dcib", []),
                                      ("fn", "api.dcib", []),
                                      ("api", 'api.dcib")]', []),
                                      ("api", "api.dcib", ['header.hpp\nextern "C"'])):
            with self.subTest(module=module, path=path), self.assertRaises(converter.ConversionError):
                converter.emit_import(document, module, path, headers)


if __name__ == "__main__":
    unittest.main()

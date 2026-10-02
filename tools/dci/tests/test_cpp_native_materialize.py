from pathlib import Path
import tempfile
import unittest

from tools.dci.cpp_materialize import cpp_type, member_source
from tools.dci.tests.test_cpp_adapter_multiabi import adapter
from tools.dci.dci_close_instances import (
    _canon_instance, _merge_documents, _requested_list_initializers,
    _rust_adapter_cmd, _unverified_return_instances,
)


class NativeClosureTests(unittest.TestCase):
    def test_conditional_noexcept_does_not_suppress_unwind(self):
        for signature in ("int ()", "int () noexcept(false)", "int () noexcept(dependent<T>)"):
            self.assertEqual(adapter.function_unwind_contract(signature), "may_unwind")
        for signature in ("int () noexcept", "int () noexcept(true)"):
            self.assertEqual(adapter.function_unwind_contract(signature), "no_unwind")

    def test_cpp_scalar_identity_is_preserved_beyond_machine_width(self):
        self.assertEqual(cpp_type(adapter.dci_type("char16_t")), "char16_t")
        self.assertEqual(cpp_type(adapter.dci_type("long")), "long")
        self.assertEqual(cpp_type(adapter.dci_type("const char16_t &")), "const char16_t&")

    def test_exact_namespace_identity(self):
        self.assertEqual(_canon_instance("std::vector<int>"), "std.vector<i32>")
        self.assertNotEqual(_canon_instance("a::Box<int>"), _canon_instance("b::Box<int>"))

    def test_initializers_extend_existing_instance_and_deduplicate(self):
        request = {"kind": "list_constructor", "owner": "std::vector<int>", "arguments": ["int"]}
        docs = [{"exports": {"symbols": [{"cpp_materialization": request}]}}]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "requests"
            path.write_text("list-init\tstd.vector::<i32>\ti32\n"
                            "list-init\tstd.vector::<i32>\ti32,i32\n"
                            "list-init\tstd.vector::<i32>\ti32,i32\n", encoding="utf-8")
            self.assertEqual(_requested_list_initializers(path, docs),
                             [("std::vector<int>", ["int", "int"], "std.vector<i32>")])

    def test_merge_retains_distinct_constructor_overloads(self):
        first = {"kind": "constructor", "owner": "X<i32>", "name": "X::constructor", "link_name": "one"}
        second = dict(first, link_name="two")
        merged = _merge_documents({"exports": {"symbols": [first]}}, {"exports": {"symbols": [second]}})
        self.assertEqual({s["link_name"] for s in merged["exports"]["symbols"]}, {"one", "two"})

    def test_missing_returns_ignore_unrelated_template_surfaces(self):
        document = {"exports": {"rejected_symbols": [
            {"name": "a::Box<int>::value", "reason": "value return type 'a::Other<int>' has no verified nonzero layout"},
            {"name": "b::Box<int>::value", "reason": "value return type 'b::Other<int>' has no verified nonzero layout"},
        ]}}
        self.assertEqual(_unverified_return_instances(document, {"a.Box<i32>"}), ["a::Other<int>"])

    def test_cargo_provenance_replayed(self):
        command = _rust_adapter_cmd({"source": {"crate_root": "/crate/src/lib.rs", "cargo": {
            "manifest_path": "/crate/Cargo.toml", "package_id": "crate@1.0", "requested_features": ["std"],
            "no_default_features": True, "offline": True, "locked": True,
        }, "selected_items": ["hash"]}}, None)
        for value in ("--manifest-path", "/crate/Cargo.toml", "--features", "std", "--no-default-features", "--offline", "--locked", "--item", "hash"):
            self.assertIn(value, command)

    def test_list_materialization_keeps_brace_semantics(self):
        symbol = {"kind": "constructor", "link_name": "dci_list_test", "params": [
            {"type": {"name": "i32", "reference": "value"}}],
            "cpp_materialization": {"kind": "list_constructor", "owner": "std::vector<int>", "member": "constructor"}}
        source = "\n".join(member_source(symbol, 0))
        self.assertIn("R{std::forward<int>(p0)}", source)
        self.assertIn('extern "C" R* dci_list_test', source)
        self.assertNotIn("push_back", source)

    def test_mutable_and_rvalue_reference_types_stay_distinct(self):
        self.assertEqual(cpp_type({"name": "i32", "reference": "reference"}), "int&")
        self.assertEqual(cpp_type({"name": "i32", "reference": "reference", "reference_kind": "rvalue"}), "int&&")


if __name__ == "__main__":
    unittest.main()

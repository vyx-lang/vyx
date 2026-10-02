from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


TOOL_DIR = Path(__file__).resolve().parents[1]
if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

import dcib
import dci_adapter_rust as adapter
import dci_validate

import subprocess


def _supported_edition() -> str:
    """Return the newest Rust edition the installed rustc actually compiles.

    The adapter fixtures do not depend on edition-specific semantics, so the
    tests run against whatever the local toolchain supports (rustc < 1.85 has no
    edition 2024).
    """
    rustc = shutil.which("rustc")
    if not rustc:
        return "2021"
    for edition in ("2024", "2021"):
        with tempfile.TemporaryDirectory() as probe_dir:
            source = Path(probe_dir) / "probe.rs"
            source.write_text("", encoding="utf-8")
            try:
                result = subprocess.run(
                    [rustc, "--edition", edition, "--crate-type", "lib",
                     "--emit=metadata", "-o", str(Path(probe_dir) / "probe.rmeta"),
                     str(source)],
                    capture_output=True, timeout=30,
                )
            except (OSError, subprocess.SubprocessError):
                continue
            if result.returncode == 0:
                return edition
    return "2021"


class RustParserTests(unittest.TestCase):
    def parse(self, source: str) -> adapter.ParsedCrate:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            return adapter.RustParser(root).parse()

    def test_parses_modern_export_attributes_and_stable_repr(self) -> None:
        crate = self.parse(
            r'''
            #[repr(C, align(16))]
            pub struct Pair { pub left: u64, pub right: u64 }

            #[unsafe(no_mangle)]
            pub unsafe extern "system" fn pair_sum(value: *const Pair) -> u64 { 0 }

            #[unsafe(export_name = "pair-reset")]
            pub extern "C" fn reset(value: *mut Pair) {}
            '''
        )
        record = crate.records[("Pair",)]
        self.assertEqual(record.representation, "stable")
        self.assertEqual(record.explicit_align, 16)
        self.assertEqual([field.name for field in record.fields], ["left", "right"])
        self.assertEqual(crate.functions[0].abi, "system")
        self.assertEqual(crate.functions[0].export_name, "pair_sum")
        self.assertEqual(crate.functions[1].export_name, "pair-reset")

    def test_parses_external_and_inline_modules(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(
                "pub mod wire; pub mod inline { #[repr(C)] pub struct B { pub x: i32 } }",
                encoding="utf-8",
            )
            (Path(temp_dir) / "wire.rs").write_text(
                "#[repr(transparent)] pub struct A(pub u32); pub mod nested;",
                encoding="utf-8",
            )
            (Path(temp_dir) / "wire").mkdir()
            (Path(temp_dir) / "wire" / "nested.rs").write_text(
                "#[repr(C)] pub struct C { pub y: u64 }",
                encoding="utf-8",
            )
            crate = adapter.RustParser(root).parse()
        self.assertIn(("wire", "A"), crate.records)
        self.assertIn(("wire", "nested", "C"), crate.records)
        self.assertIn(("inline", "B"), crate.records)

    def test_type_system_accepts_measured_references_and_rejects_native_fn_pointers(self) -> None:
        crate = self.parse(
            """
            #[repr(C)] pub struct A { pub value: i32 }
            pub trait Sink { fn consume(&self, x: i32) -> i32; }
            """
        )
        target = adapter.TargetInfo(
            "x86_64-unknown-linux-gnu", "x86_64", 64, "little", "linux",
            "gnu", "elf", "sysv64",
        )
        types = adapter.TypeSystem(crate, target, "sample")
        reference = types.parse(adapter.lex_rust("&mut A"), ())
        slice_ref = types.parse(adapter.lex_rust("&[u8]"), ())
        dyn_ref = types.parse(adapter.lex_rust("&dyn Sink"), ())
        native_fn = types.parse(adapter.lex_rust("fn(i32) -> i32"), ())
        nullable_c_fn = types.parse(
            adapter.lex_rust('Option<unsafe extern "C" fn(i32) -> i32>'), ()
        )
        fat_pointer = types.parse(adapter.lex_rust("*const str"), ())
        self.assertTrue(types.is_stable(reference)[0], types.is_stable(reference)[1])
        self.assertFalse(reference.fat)
        self.assertTrue(types.is_stable(slice_ref)[0], types.is_stable(slice_ref)[1])
        self.assertTrue(slice_ref.fat)
        self.assertTrue(types.is_stable(dyn_ref)[0], types.is_stable(dyn_ref)[1])
        self.assertTrue(dyn_ref.fat)
        self.assertFalse(types.is_stable(native_fn)[0])
        self.assertEqual(nullable_c_fn.kind, "function")
        self.assertTrue(nullable_c_fn.nullable)
        self.assertTrue(types.is_stable(nullable_c_fn)[0])
        self.assertTrue(types.is_stable(fat_pointer)[0], types.is_stable(fat_pointer)[1])
        self.assertTrue(fat_pointer.fat)

    def test_parses_trait_impl_and_accepts_native_records(self) -> None:
        crate = self.parse(
            """
            pub struct NativePair { pub flag: u8, pub wide: u64, pub mid: u32 }
            pub trait Sink { fn consume(&self, x: i32) -> i32; }
            impl Sink for NativePair {
                fn consume(&self, x: i32) -> i32 { x }
            }
            """
        )
        self.assertEqual(crate.records[("NativePair",)].representation, "")
        self.assertIn(("Sink",), crate.traits)
        self.assertEqual(crate.traits[("Sink",)].methods[0].name, "consume")
        self.assertEqual(crate.trait_impls[0].trait_path, ("Sink",))
        self.assertEqual(crate.trait_impls[0].type_path, ("NativePair",))
        target = adapter.TargetInfo(
            "x86_64-pc-windows-msvc", "x86_64", 64, "little", "windows",
            "msvc", "coff", "win64",
        )
        types = adapter.TypeSystem(crate, target, "sample")
        native = types.parse(adapter.lex_rust("NativePair"), ())
        self.assertTrue(types.is_stable(native)[0])

    def test_parses_inherent_pub_methods_and_rewrites_self(self) -> None:
        crate = self.parse(
            """
            pub struct NativePair { pub flag: u8, pub wide: u64, pub mid: u32 }
            impl NativePair {
                pub fn flag_of(&self) -> u8 { self.flag }
                pub fn take(self) -> NativePair { self }
                fn hidden(&self) -> u8 { self.flag }
            }
            pub fn take_pair(value: NativePair) { core::mem::forget(value); }
            """
        )
        paths = {function.path: function for function in crate.functions}
        self.assertIn(("NativePair", "flag_of"), paths)
        self.assertIn(("NativePair", "take"), paths)
        self.assertIn(("take_pair",), paths)
        self.assertTrue(paths[("NativePair", "flag_of")].public)
        self.assertFalse(paths[("NativePair", "hidden")].public)
        self.assertIn("NativePair", adapter.tokens_text(paths[("NativePair", "flag_of")].params[0].type_tokens))
        self.assertEqual(adapter.tokens_text(paths[("NativePair", "take")].params[0].type_tokens).replace(" ", ""), "NativePair")

    def test_demangles_legacy_rustc_symbols(self) -> None:
        self.assertEqual(
            adapter.demangle_legacy_rust_symbol("_ZN6native10NativePair7flag_of17h411f5bf710e61c3cE"),
            ("native", "NativePair", "flag_of"),
        )
        self.assertEqual(
            adapter.demangle_legacy_rust_symbol("_ZN6native9take_pair17hfae6872197d0a28cE"),
            ("native", "take_pair"),
        )

    def test_use_alias_resolves_to_stable_record(self) -> None:
        crate = self.parse(
            "pub mod wire { #[repr(C)] pub struct Value { pub data: i32 } } "
            "use crate::wire::Value as WireValue;"
        )
        target = adapter.TargetInfo(
            "x86_64-pc-windows-msvc", "x86_64", 64, "little", "windows",
            "msvc", "coff", "win64",
        )
        types = adapter.TypeSystem(crate, target, "sample")
        value = types.parse(adapter.lex_rust("WireValue"), ())
        self.assertEqual(value.name, "sample.wire.Value")
        self.assertTrue(types.is_stable(value)[0])

    def test_android_api_and_convenience_targets_map_to_rustc_targets(self) -> None:
        self.assertEqual(adapter.canonical_rust_target("arm64_android"), "aarch64-linux-android")
        self.assertEqual(
            adapter.canonical_rust_target("aarch64-linux-android23"),
            "aarch64-linux-android",
        )

    def test_embedded_ownership_rules_are_normalized(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "lib.rs"
            source.write_text(
                '''/* dci-ownership
                {"take(*mut Value)": {"parameters": {"0": "move"}, "return": "owned"}}
                dci-ownership-end */''',
                encoding="utf-8",
            )
            rules = adapter.discover_ownership_annotations([source])
        self.assertEqual(
            rules["take(*mut Value)"],
            {"parameters": {0: "move"}, "return": "owned"},
        )

    def test_generic_type_parameters_are_captured(self) -> None:
        crate = self.parse(
            "#[repr(C)] pub struct Pair<T> { pub left: T, pub right: T } "
            "#[repr(C)] pub struct Const<const N: usize> { pub data: [u8; N] }"
        )
        pair = crate.records[("Pair",)]
        self.assertTrue(pair.generic)
        self.assertEqual(pair.type_params, ["T"])
        self.assertFalse(pair.const_or_lifetime_params)
        const_record = crate.records[("Const",)]
        self.assertTrue(const_record.const_or_lifetime_params)

    def test_lifecycle_annotations_require_operations_for_declared_semantics(self) -> None:
        contract = adapter.normalize_lifecycle_contract(
            "crate::Handle",
            {
                "ownership_model": "unique",
                "copy_semantics": "forbidden",
                "move_semantics": "forbidden",
                "destruction": "operation",
                "allocator_domain": "sample.heap",
                "operations": {"destroy": {"symbol": "handle_free"}},
            },
        )
        self.assertEqual(contract["operations"], {"destroy": "handle_free"})
        self.assertEqual(contract["allocator_domain"], "sample.heap")
        with self.assertRaises(adapter.RustAdapterError):
            adapter.normalize_lifecycle_contract(
                "crate::Handle",
                {"destruction": "operation", "move_semantics": "forbidden", "operations": {}},
            )

    def test_stub_request_annotation_is_normalized(self) -> None:
        request = adapter.normalize_stub_request(
            "sample.Widget.on_event",
            {
                "kind": "reverse_override",
                "target": "crate::Widget",
                "wrapper": "sample_widget_stub",
                "operations": [{"name": "on_event", "requires_stub": True}],
            },
        )
        self.assertEqual(request["kind"], "reverse_override")
        self.assertEqual(request["wrapper"]["link_name"], "sample_widget_stub")
        self.assertEqual(request["capabilities"], ["emit-source", "compile-object"])
        self.assertEqual(request["control_flow"]["propagation"]["mode"], "forbidden")
        with self.assertRaises(adapter.RustAdapterError):
            adapter.normalize_stub_request("bad", {"kind": "reverse_override"})
        with self.assertRaises(adapter.RustAdapterError):
            adapter.normalize_stub_request("bad", {"kind": "unknown", "wrapper": "w"})

    def test_edition_is_inferred_from_nearest_cargo_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "src").mkdir()
            source = root / "src" / "lib.rs"
            source.write_text("", encoding="utf-8")
            (root / "Cargo.toml").write_text(
                '[package]\nname = "adapter_probe"\nversion = "0.1.0"\nedition = "2024"\n',
                encoding="utf-8",
            )
            self.assertEqual(adapter.inferred_edition(source), "2024")


@unittest.skipUnless(shutil.which("rustc"), "rustc is required for Adapter integration tests")
class RustAdapterIntegrationTests(unittest.TestCase):
    def test_generates_strict_contract_from_verified_rustc_facts(self) -> None:
        source = r'''
            #![allow(improper_ctypes_definitions)]

            pub mod wire;
            use crate::wire::External;

            #[repr(C)]
            #[derive(Clone, Copy)]
            pub struct Pair { pub left: u64, pub right: u64 }

            #[repr(transparent)]
            #[derive(Clone, Copy)]
            pub struct Code(pub u32);

            pub type StatusCode = Code;

            #[repr(u8)]
            pub enum State { Idle = 0, Busy = 1 }

            #[repr(C)]
            pub struct UnstableField { pub text: Vec<u8> }

            #[repr(C)]
            pub struct ZeroSized;

            #[cfg(dci_never)]
            #[repr(C)]
            pub struct Disabled { pub value: i32 }

            #[unsafe(no_mangle)]
            pub extern "C" fn pair_make(left: u64, right: u64) -> Pair {
                Pair { left, right }
            }

            #[unsafe(no_mangle)]
            pub extern "system" fn pair_sum(value: Pair, state: State) -> u64 {
                value.left + value.right + state as u64
            }

            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn pair_read(value: *const Pair) -> u64 {
                if value.is_null() { 0 } else { unsafe { (*value).left } }
            }

            #[unsafe(no_mangle)]
            pub extern "C" fn external_value(value: External) -> i32 { value.value }

            #[unsafe(no_mangle)]
            pub extern "C" fn apply_callback(
                callback: Option<unsafe extern "C" fn(i32) -> i32>,
                value: i32,
            ) -> i32 {
                match callback { Some(callback) => unsafe { callback(value) }, None => value }
            }

            pub enum Payload { Empty, One(u32) }

            #[unsafe(no_mangle)]
            pub extern "C" fn rejected_reference(value: &Pair) -> u64 { value.left }

            #[unsafe(no_mangle)]
            pub extern "Rust" fn rejected_native(value: i32) -> i32 { value }

            #[unsafe(no_mangle)]
            pub extern "C" fn rejected_payload(value: Payload) -> u32 {
                match value { Payload::Empty => 0, Payload::One(x) => x }
            }

            #[cfg(dci_never)]
            #[unsafe(no_mangle)]
            pub extern "C" fn disabled_export(value: Disabled) -> i32 { value.value }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            (Path(temp_dir) / "wire.rs").write_text(
                "#[repr(C)] #[derive(Clone, Copy)] pub struct External { pub value: i32 }",
                encoding="utf-8",
            )
            output = Path(temp_dir) / "contract.dcib"
            debug = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", _supported_edition(),
                "-o", str(output), "--debug-json-out", str(debug),
            ])
            self.assertEqual(rc, 0)
            document = dcib.decode(output.read_bytes())
            debug_document = json.loads(debug.read_text(encoding="utf-8"))
            self.assertEqual(document, debug_document)

            schema = json.loads((TOOL_DIR / "schema" / "dci-1.0.schema.json").read_text(encoding="utf-8"))
            errors = dci_validate.schema_errors(schema, document)
            errors.extend(dci_validate.semantic_errors(document, True))
            self.assertEqual(errors, [])

            layouts = {layout["type_name"]: layout for layout in document["exports"]["layouts"]}
            self.assertEqual(layouts["sample.Pair"]["size"], 16)
            self.assertEqual([field["offset"] for field in layouts["sample.Pair"]["fields"]], [0, 8])
            self.assertEqual(layouts["sample.Code"]["representation"], "transparent")
            self.assertEqual(layouts["sample.State"]["size"], 1)
            self.assertEqual(layouts["sample.wire.External"]["size"], 4)
            self.assertNotIn("sample.UnstableField", layouts)
            self.assertNotIn("sample.ZeroSized", layouts)
            self.assertNotIn("sample.Disabled", layouts)
            aliases = {alias["name"]: alias["target"] for alias in document["exports"]["aliases"]}
            self.assertEqual(aliases["sample.StatusCode"], "sample.Code")

            symbols = {symbol["link_name"]: symbol for symbol in document["exports"]["symbols"]}
            expected_exports = {
                "pair_make", "pair_sum", "pair_read", "external_value", "apply_callback",
                "rejected_native", "rejected_reference",
            }
            self.assertTrue(expected_exports.issubset(set(symbols)), set(symbols))
            self.assertEqual(symbols["pair_read"]["params"][0]["ownership"], "borrow")
            self.assertEqual(symbols["rejected_reference"]["params"][0]["ownership"], "borrow")
            self.assertEqual(symbols["rejected_reference"]["abi"]["parameters"][0]["passing"], "direct")
            self.assertTrue(symbols["apply_callback"]["params"][0]["type"]["nullable"])
            self.assertIn(symbols["pair_make"]["abi"]["return"]["passing"], {"coerce", "sret", "split", "direct"})
            self.assertEqual(symbols["rejected_native"]["calling_convention"], "rust")
            rejected = {item["link_name"]: item["reason"] for item in document["exports"]["rejected_symbols"]}
            self.assertIn("data-carrying enum", rejected["rejected_payload"])
            self.assertNotIn("rejected_reference", rejected)
            self.assertNotIn("rejected_native", rejected)
            self.assertNotIn("disabled_export", rejected)
            self.assertNotIn(str(Path(temp_dir).resolve()), json.dumps(document))

    def test_exports_closed_generic_instances_on_demand(self) -> None:
        source = r'''
            #[repr(C)]
            pub struct Pair<T> { pub left: T, pub right: T }

            #[repr(C)]
            pub struct Wrap<T> { pub value: T, pub tag: u8 }

            pub type PairI32 = Pair<i32>;

            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn sum_pair(p: *const Pair<i64>) -> i64 {
                unsafe { (*p).left + (*p).right }
            }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", "2021",
                "--export-instance", "Wrap<u32>",
                "-o", str(output),
            ])
            self.assertEqual(rc, 0)
            document = json.loads(output.read_text(encoding="utf-8"))
            layouts = {layout["type_name"]: layout for layout in document["exports"]["layouts"]}
            # Closed instances requested by alias, CLI, and a call signature.
            self.assertIn("sample.Pair<i32>", layouts)
            self.assertIn("sample.Pair<i64>", layouts)
            self.assertIn("sample.Wrap<u32>", layouts)
            # The open generic itself is never exported.
            self.assertNotIn("sample.Pair", layouts)
            self.assertNotIn("sample.Wrap", layouts)
            self.assertEqual(layouts["sample.Pair<i64>"]["size"], 16)
            self.assertEqual(
                [f["type"]["name"] for f in layouts["sample.Pair<i32>"]["fields"]],
                ["i32", "i32"],
            )
            self.assertEqual(layouts["sample.Pair<i32>"]["instance"]["of"], "sample.Pair")
            aliases = {a["name"]: a["target"] for a in document["exports"]["aliases"]}
            self.assertEqual(aliases["sample.PairI32"], "sample.Pair<i32>")
            symbols = {s["link_name"]: s for s in document["exports"]["symbols"]}
            self.assertEqual(symbols["sum_pair"]["params"][0]["type"]["name"], "sample.Pair<i64>")

            schema = json.loads((TOOL_DIR / "schema" / "dci-1.0.schema.json").read_text(encoding="utf-8"))
            errors = dci_validate.schema_errors(schema, document)
            errors.extend(dci_validate.semantic_errors(document, True))
            self.assertEqual(errors, [])

    def test_rejects_unknown_export_instance_request(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text("#[repr(C)] pub struct A { pub x: i32 }", encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", "2021",
                "--export-instance", "Vec<u8>",
                "-o", str(output),
            ])
            self.assertEqual(rc, 2)

    def test_evidence_based_lifecycle_contract_is_emitted(self) -> None:
        source = r'''
            /* dci-lifecycle
            {"crate::Handle": {"ownership_model":"unique","copy_semantics":"forbidden",
             "move_semantics":"operation","destruction":"operation",
             "moved_from_state":"destructible_only","allocator_domain":"sample.heap",
             "operations":{"move":{"symbol":"handle_move"},"destroy":{"symbol":"handle_free"}}}}
            dci-lifecycle-end */

            #[repr(C)]
            pub struct Handle { pub id: u64 }

            impl Drop for Handle { fn drop(&mut self) {} }

            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn handle_free(h: *mut Handle) { let _ = h; }
            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn handle_move(dst: *mut Handle, src: *mut Handle) {
                let _ = (dst, src);
            }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", "2021",
                "-o", str(output),
            ])
            self.assertEqual(rc, 0)
            document = json.loads(output.read_text(encoding="utf-8"))
            handle = next(
                layout for layout in document["exports"]["layouts"]
                if layout["type_name"] == "sample.Handle"
            )
            lifecycle = handle["lifecycle"]
            self.assertEqual(lifecycle["destruction"], "operation")
            self.assertEqual(lifecycle["operations"]["destroy"]["symbol"], "handle_free")
            self.assertEqual(lifecycle["allocator_domain"], "sample.heap")
            self.assertFalse(handle["is_trivially_destructible"])
            schema = json.loads((TOOL_DIR / "schema" / "dci-1.0.schema.json").read_text(encoding="utf-8"))
            errors = dci_validate.schema_errors(schema, document)
            errors.extend(dci_validate.semantic_errors(document, True))
            self.assertEqual(errors, [])

    def test_drop_record_without_lifecycle_evidence_is_not_exported(self) -> None:
        source = r'''
            #[repr(C)]
            pub struct Handle { pub id: u64 }
            impl Drop for Handle { fn drop(&mut self) {} }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", "2021",
                "-o", str(output),
            ])
            self.assertEqual(rc, 0)
            document = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(document["exports"]["layouts"], [])

    def test_lifecycle_operation_symbol_must_be_exported(self) -> None:
        source = r'''
            /* dci-lifecycle
            {"crate::Handle": {"copy_semantics":"forbidden","move_semantics":"forbidden",
             "destruction":"operation","operations":{"destroy":{"symbol":"missing_free"}}}}
            dci-lifecycle-end */

            #[repr(C)]
            pub struct Handle { pub id: u64 }
            impl Drop for Handle { fn drop(&mut self) {} }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", "2021",
                "-o", str(output),
            ])
            self.assertEqual(rc, 2)

    def test_stub_request_is_emitted_and_declares_stub_mode(self) -> None:
        source = r'''
            /* dci-stub
            {"sample.Widget.on_event": {"kind":"reverse_override","target":"crate::Widget",
             "wrapper":"sample_widget_stub","capabilities":["emit-source","compile-object"],
             "operations":[{"name":"on_event","requires_stub":true}]}}
            dci-stub-end */

            #[repr(C)]
            pub struct Widget { pub id: u32 }

            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn widget_id(w: *const Widget) -> u32 { unsafe { (*w).id } }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", "2021",
                "-o", str(output),
            ])
            self.assertEqual(rc, 0)
            document = json.loads(output.read_text(encoding="utf-8"))
            self.assertIn("stub", document["profile"]["consumer_modes"])
            requests = document["exports"]["stub_requests"]
            reverse = [item for item in requests if item.get("kind") == "reverse_override"]
            self.assertEqual(len(reverse), 1)
            self.assertEqual(reverse[0]["wrapper"]["link_name"], "sample_widget_stub")
            self.assertEqual(reverse[0]["target"], "crate::Widget")
            schema = json.loads((TOOL_DIR / "schema" / "dci-1.0.schema.json").read_text(encoding="utf-8"))
            errors = dci_validate.schema_errors(schema, document)
            errors.extend(dci_validate.semantic_errors(document, True))
            self.assertEqual(errors, [])

    def test_stub_request_wrapper_conflict_is_rejected(self) -> None:
        source = r'''
            /* dci-stub
            {"conflict": {"kind":"operation_wrapper","wrapper":"widget_id"}}
            dci-stub-end */

            #[repr(C)]
            pub struct Widget { pub id: u32 }

            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn widget_id(w: *const Widget) -> u32 { unsafe { (*w).id } }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", "2021",
                "-o", str(output),
            ])
            self.assertEqual(rc, 2)

    def test_deny_rejected_fails_without_writing_contract(self) -> None:
        source = r'''
            pub enum Payload { Empty, One(u32) }
            #[unsafe(no_mangle)]
            pub extern "C" fn bad(value: Payload) -> u32 {
                match value { Payload::Empty => 0, Payload::One(x) => x }
            }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.dcib"
            rc = adapter.main([
                str(root), "--edition", "2024", "-o", str(output), "--deny-rejected",
            ])
            self.assertEqual(rc, 2)
            self.assertFalse(output.exists())

    def test_panic_and_result_exports_emit_translate_unwind_translators(self) -> None:
        source = r'''
            #[unsafe(no_mangle)]
            pub extern "C-unwind" fn rust_div(a: i32, b: i32) -> i32 {
                if b == 0 { panic!("div0"); }
                a / b
            }

            #[unsafe(no_mangle)]
            pub extern "C" fn rust_checked(a: i32, b: i32) -> Result<i32, i32> {
                if b == 0 { Err(7) } else { Ok(a / b) }
            }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            stub = Path(temp_dir) / "stub.rs"
            rc = adapter.main([
                str(root), "--crate-name", "sample", "--edition", "2021",
                "-o", str(output), "--stub-out", str(stub),
            ])
            self.assertEqual(rc, 0, "adapter failed")
            document = json.loads(output.read_text(encoding="utf-8"))
            schema = json.loads((TOOL_DIR / "schema" / "dci-1.0.schema.json").read_text(encoding="utf-8"))
            errors = dci_validate.schema_errors(schema, document)
            errors.extend(dci_validate.semantic_errors(document, True))
            self.assertEqual(errors, [])
            symbols = {item["link_name"]: item for item in document["exports"]["symbols"]}
            self.assertIn("rust_div", symbols)
            self.assertEqual(symbols["rust_div"]["control_flow"]["unwind"], "may_unwind")
            self.assertIn("rust_checked", symbols)
            self.assertEqual(symbols["rust_checked"]["control_flow"]["unwind"], "may_unwind")
            requests = [
                item for item in document["exports"]["stub_requests"]
                if (item.get("synthesis") or {}).get("strategy") == "translate_unwind"
            ]
            by_target = {item["target"]: item for item in requests}
            self.assertIn("rust_div", by_target)
            self.assertIn("rust_checked", by_target)
            for target in ("rust_div", "rust_checked"):
                wrapper = by_target[target]["wrapper"]["link_name"]
                self.assertTrue(wrapper.startswith("dci_tr_"), wrapper)
                self.assertEqual(symbols[wrapper]["control_flow"]["unwind"], "no_unwind")
            self.assertTrue(stub.is_file())
            stub_text = stub.read_text(encoding="utf-8")
            self.assertIn("dci_tr_rust_div", stub_text)
            self.assertIn("translate_result", stub_text)
            rustc = shutil.which("rustc")
            exe = Path(temp_dir) / "probe.exe"
            probe = Path(temp_dir) / "probe.rs"
            probe.write_text(
                stub_text
                + r'''
fn main() {
    let ok = dci_tr_rust_div(10, 2);
    assert_eq!(ok.tag, 0);
    assert_eq!(ok.ok, 5);
    let boom = dci_tr_rust_div(1, 0);
    assert_eq!(boom.tag, 1);
    assert_eq!(boom.err.producer_tag, 3);
    let checked_ok = dci_tr_rust_checked(10, 2);
    assert_eq!(checked_ok.tag, 0);
    assert_eq!(checked_ok.ok, 5);
    let checked_err = dci_tr_rust_checked(1, 0);
    assert_eq!(checked_err.tag, 1);
    assert_eq!(checked_err.err.producer_tag, 4);
}
''',
                encoding="utf-8",
            )
            compiled = subprocess.run(
                [
                    rustc, str(probe), "-C", "panic=unwind", "-C", "opt-level=0",
                    "--edition", "2021", "-o", str(exe),
                ],
                capture_output=True, text=True, errors="replace",
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, errors="replace")
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_rustc_measures_native_layout_and_dyn_vtable(self) -> None:
        root = TOOL_DIR / "tests" / "fixtures" / "rustc_native_lib.rs"
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "native", "--edition", _supported_edition(),
                "-o", str(output),
            ])
            self.assertEqual(rc, 0, output.read_text(encoding="utf-8") if output.is_file() else "no output")
            document = json.loads(output.read_text(encoding="utf-8"))
            layouts = {layout["type_name"]: layout for layout in document["exports"]["layouts"]}
            pair = layouts["native.NativePair"]
            self.assertEqual(pair["representation"], "native")
            self.assertEqual(pair["size"], 16)
            self.assertEqual(pair["alignment"], 8)
            offsets = {field["name"]: field["offset"] for field in pair["fields"]}
            self.assertEqual(offsets, {"wide": 0, "mid": 8, "flag": 12})
            self.assertEqual(pair["rust"]["param"]["passing"], "indirect")
            sink = layouts["native.VyxSink"]
            self.assertEqual(sink["size"], 4)
            trait_layout = layouts["native.Sink"]
            self.assertTrue(trait_layout["has_vtable"])
            self.assertEqual(trait_layout["size"], 8)
            self.assertEqual(trait_layout["lifecycle"]["ownership_model"], "value")
            self.assertEqual(trait_layout["rust"]["kind"], "trait")
            tables = { (table["class_name"], table["base_class"]): table
                       for table in document["exports"]["dispatch_tables"] }
            self.assertIn(("native.Sink", "native.Sink"), tables)
            self.assertIn(("native.VyxSink", "native.Sink"), tables)
            impl_table = tables[("native.VyxSink", "native.Sink")]
            members = [entry["member_name"] for entry in impl_table["entries"]]
            self.assertEqual(members[:4], ["drop_in_place", "size", "align", "consume"])
            consume_entries = [entry for entry in impl_table["entries"] if entry["member_name"] == "consume"]
            self.assertEqual(consume_entries[0]["link_name"], "dci.rust.dyn.native.Sink.consume")
            by_name = {symbol["name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertTrue(by_name["Sink::consume"]["is_virtual"])
            self.assertEqual(by_name["Sink::consume"]["calling_convention"], "rust")
            symbols = {symbol["link_name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertEqual(symbols["vyx_rust_arg"]["calling_convention"], "rust")
            self.assertEqual(symbols["vyx_c_thin"]["abi"]["parameters"][0]["passing"], "direct")
            self.assertEqual(symbols["vyx_rust_thin"]["abi"]["parameters"][0]["passing"], "direct")
            self.assertEqual(symbols["vyx_c_slice"]["abi"]["parameters"][0]["passing"], "indirect")
            self.assertEqual(symbols["vyx_c_slice"]["params"][0]["type"]["wide"], True)
            self.assertEqual(symbols["vyx_c_slice"]["params"][0]["type"]["metadata_kind"], "len")
            self.assertEqual(symbols["vyx_rust_slice"]["abi"]["parameters"][0]["passing"], "split")
            self.assertEqual(symbols["vyx_rust_slice"]["abi"]["parameters"][0]["registers"], ["ptr", "i64"])
            self.assertEqual(symbols["vyx_c_dyn"]["abi"]["parameters"][0]["passing"], "indirect")
            self.assertEqual(symbols["vyx_rust_dyn"]["abi"]["parameters"][0]["passing"], "split")
            self.assertEqual(symbols["vyx_rust_dyn"]["abi"]["parameters"][0]["registers"], ["ptr", "ptr"])
            by_name = {symbol["name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertIn("NativePair::flag_of", by_name)
            self.assertIn("take_pair", by_name)
            self.assertTrue(by_name["NativePair::flag_of"]["link_name"].startswith("_ZN"))
            self.assertEqual(by_name["NativePair::flag_of"]["calling_convention"], "rust")
            self.assertEqual(by_name["NativePair::flag_of"]["kind"], "method")
            self.assertEqual(by_name["NativePair::flag_of"]["abi"]["receiver"]["passing"], "direct")
            self.assertEqual(by_name["take_pair"]["calling_convention"], "rust")
            self.assertEqual(by_name["take_pair"]["abi"]["parameters"][0]["passing"], "indirect")
            self.assertEqual(by_name["NativePair::take"]["abi"]["return"]["passing"], "sret")
            self.assertNotIn("NativePair::hidden", by_name)
            color = layouts["native.Color"]
            self.assertEqual(color["representation"], "native")
            self.assertEqual(color["size"], 1)
            self.assertEqual(document["profile"]["level"], "L3")
            self.assertIn("rustc-fat-pointer", document["profile"]["features"])
            self.assertIn("rustc-trait-object", document["profile"]["features"])

    def test_opt_level_3_still_exports_unmanaged_pub_symbol(self) -> None:
        rustc = shutil.which("rustc")
        if not rustc:
            self.skipTest("rustc is not installed")
        source = (
            "pub struct NativePair { pub flag: u8 }\n"
            "impl NativePair {\n"
            "    pub fn flag_of(&self) -> u8 { self.flag }\n"
            "}\n"
            '#[unsafe(no_mangle)]\n'
            "pub extern \"C\" fn native_pair_heap() -> *mut NativePair {\n"
            "    core::ptr::null_mut()\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.rs"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "native", "--edition", _supported_edition(),
                "-o", str(output), "--rustc-arg=-Copt-level=3",
            ])
            self.assertEqual(rc, 0, output.read_text(encoding="utf-8") if output.is_file() else "no output")
            document = json.loads(output.read_text(encoding="utf-8"))
            by_name = {symbol["name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertIn("NativePair::flag_of", by_name)
            self.assertTrue(by_name["NativePair::flag_of"]["link_name"].startswith("_ZN"))
            rejected = {item["name"]: item["reason"] for item in document["exports"]["rejected_symbols"]}
            self.assertNotIn("NativePair::flag_of", rejected)


if __name__ == "__main__":
    unittest.main()

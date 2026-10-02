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
import dci_adapter_zig as adapter
import dci_validate


LIB_SOURCE = """
const nested = @import("nested.zig");

pub const Pair = extern struct {
    left: i32,
    right: i32,
};

pub const Native = struct {
    flag: u8,
    wide: u64,

    pub fn flag_of(self: Native) u8 {
        return self.flag;
    }
};

pub const PackedByte = packed struct {
    lo: u8,
    hi: u8,
};

pub const Tag = enum(u8) {
    idle = 0,
    ready = 1,
};

export fn pair_sum(value: Pair) i32 {
    return value.left + value.right;
}

export fn pair_make(left: i32, right: i32) Pair {
    return .{ .left = left, .right = right };
}

pub fn c_mul(a: i32, b: i32) callconv(.c) i32 {
    return a * b;
}

pub fn zig_only(value: Native) u8 {
    return value.flag_of();
}

export fn nested_value(item: nested.Nested) i32 {
    return item.value;
}

fn hidden() void {}
"""

NESTED_SOURCE = """
pub const Nested = extern struct {
    value: i32,
};
"""


class ZigParserTests(unittest.TestCase):
    def parse(self, source: str, nested: str | None = NESTED_SOURCE) -> adapter.ParsedModule:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.zig"
            root.write_text(source, encoding="utf-8")
            if nested is not None:
                (Path(temp_dir) / "nested.zig").write_text(nested, encoding="utf-8")
            return adapter.ZigParser(root).parse()

    def test_parses_extern_struct_export_fn_and_c_callconv(self) -> None:
        module = self.parse(LIB_SOURCE)
        pair = module.records[("Pair",)]
        self.assertEqual(pair.representation, "stable")
        self.assertEqual([field.name for field in pair.fields], ["left", "right"])
        native = module.records[("Native",)]
        self.assertEqual(native.representation, "native")
        self.assertEqual([field.name for field in native.fields], ["flag", "wide"])
        self.assertIn(("nested", "Nested"), module.records)
        self.assertEqual(module.enums[("Tag",)].integer_repr, "u8")
        paths = {function.path: function for function in module.functions}
        self.assertEqual(paths[("pair_sum",)].abi, "c")
        self.assertTrue(paths[("pair_sum",)].exported)
        self.assertEqual(paths[("pair_sum",)].export_name, "pair_sum")
        self.assertEqual(paths[("c_mul",)].abi, "c")
        self.assertFalse(paths[("c_mul",)].exported)
        self.assertEqual(paths[("zig_only",)].abi, "zig")
        self.assertIn(("Native", "flag_of"), paths)
        self.assertNotIn(("hidden",), paths)

    def test_canonical_zig_targets_match_zig_0_16_spelling(self) -> None:
        self.assertEqual(adapter.canonical_zig_target("x64_windows"), "x86_64-windows-msvc")
        self.assertEqual(adapter.canonical_zig_target("x86_64-pc-windows-msvc"), "x86_64-windows-msvc")
        self.assertEqual(adapter.canonical_zig_target("android_arm64"), "aarch64-linux-android")
        self.assertEqual(
            adapter.canonical_zig_target("aarch64-linux-android23"),
            "aarch64-linux-android",
        )
        self.assertEqual(adapter.canonical_zig_target("x64_linux"), "x86_64-linux-gnu")
        self.assertEqual(adapter.canonical_zig_target("x86_64-windows-msvc"), "x86_64-windows-msvc")


@unittest.skipUnless(shutil.which("zig"), "zig is required for Adapter integration tests")
class ZigAdapterIntegrationTests(unittest.TestCase):
    def _write_fixture(self, temp_dir: str) -> Path:
        root = Path(temp_dir) / "lib.zig"
        root.write_text(LIB_SOURCE, encoding="utf-8")
        (Path(temp_dir) / "nested.zig").write_text(NESTED_SOURCE, encoding="utf-8")
        return root

    def test_generates_strict_contract_from_verified_zig_facts(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = self._write_fixture(temp_dir)
            output = Path(temp_dir) / "contract.dcib"
            debug = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "native",
                "--target", "x64_windows",
                "-o", str(output), "--debug-json-out", str(debug),
            ])
            self.assertEqual(rc, 0)
            document = dcib.decode(output.read_bytes())
            debug_document = json.loads(debug.read_text(encoding="utf-8"))
            self.assertEqual(document, debug_document)
            self.assertEqual(document["source"]["language"], "zig")
            self.assertEqual(document["source"]["compiler"]["name"], "zig")
            flags = " ".join(document["source"]["compiler"].get("flags") or [])
            self.assertIn("ReleaseFast", flags)
            self.assertIn("x86_64-windows-msvc", flags)

            schema = json.loads((TOOL_DIR / "schema" / "dci-1.0.schema.json").read_text(encoding="utf-8"))
            errors = dci_validate.schema_errors(schema, document)
            errors.extend(dci_validate.semantic_errors(document, True))
            self.assertEqual(errors, [])

            layouts = {layout["type_name"]: layout for layout in document["exports"]["layouts"]}
            pair = layouts["native.Pair"]
            self.assertEqual(pair["size"], 8)
            self.assertEqual(pair["alignment"], 4)
            self.assertEqual(pair["representation"], "stable")
            pair_fields = {field["name"]: field for field in pair["fields"]}
            self.assertEqual(pair_fields["left"]["offset"], 0)
            self.assertEqual(pair_fields["right"]["offset"], 4)

            native_layout = layouts["native.Native"]
            self.assertEqual(native_layout["size"], 16)
            self.assertEqual(native_layout["alignment"], 8)
            self.assertEqual(native_layout["representation"], "native")
            native_fields = {field["name"]: field for field in native_layout["fields"]}
            self.assertEqual(native_fields["wide"]["offset"], 0)
            self.assertEqual(native_fields["flag"]["offset"], 8)

            packed = layouts["native.PackedByte"]
            self.assertEqual(packed["size"], 2)
            packed_fields = {field["name"]: field for field in packed["fields"]}
            self.assertEqual(packed_fields["lo"]["offset"], 0)
            self.assertEqual(packed_fields["hi"]["offset"], 1)

            self.assertEqual(layouts["native.Tag"]["size"], 1)
            self.assertEqual(layouts["native.nested.Nested"]["size"], 4)

            symbols = {symbol["link_name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertIn("pair_sum", symbols)
            self.assertIn("pair_make", symbols)
            self.assertIn("c_mul", symbols)
            self.assertIn("nested_value", symbols)
            self.assertEqual(symbols["pair_sum"]["calling_convention"], "c")
            self.assertEqual(symbols["pair_sum"]["abi"]["parameters"][0]["passing"], "coerce")
            self.assertEqual(symbols["pair_sum"]["abi"]["parameters"][0]["coerce_to"]["name"], "u64")
            self.assertEqual(symbols["c_mul"]["name"], "c_mul")

            by_name = {symbol["name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertIn("zig_only", by_name)
            self.assertEqual(by_name["zig_only"]["calling_convention"], "zig")
            self.assertIn("zig_only", by_name["zig_only"]["link_name"])
            self.assertEqual(by_name["zig_only"]["kind"], "function")
            self.assertEqual(by_name["zig_only"]["abi"]["parameters"][0]["passing"], "indirect")
            self.assertEqual(by_name["zig_only"]["params"][0]["type"]["name"], "native.Native")

            flag_of = by_name["Native::flag_of"]
            self.assertEqual(flag_of["calling_convention"], "zig")
            self.assertEqual(flag_of["kind"], "method")
            self.assertEqual(flag_of["owner"], "native.Native")
            self.assertEqual(flag_of["member_name"], "flag_of")
            self.assertIn("flag_of", flag_of["link_name"])
            self.assertEqual(flag_of["abi"]["receiver"]["passing"], "indirect")

            self.assertEqual(native_layout["zig"]["param"]["passing"], "indirect")
            conventions = {item["name"] for item in document["exports"]["calling_conventions"]}
            self.assertEqual({"c", "zig"}, conventions)
            self.assertIn("zig-calling-convention", document["profile"]["features"])

            rejected = {item["name"]: item["reason"] for item in document["exports"]["rejected_symbols"]}
            self.assertNotIn("zig_only", rejected)
            self.assertNotIn("Native::flag_of", rejected)
            self.assertNotIn("hidden", rejected)
            self.assertNotIn("pair_sum", rejected)

    def test_releasefast_still_exports_unmanaged_c_callconv(self) -> None:
        source = (
            "pub fn c_mul(a: i32, b: i32) callconv(.c) i32 {\n"
            "    return a * b;\n"
            "}\n"
            "export fn pair_add(left: i32, right: i32) i32 {\n"
            "    return left + right;\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.zig"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "native",
                "--target", "x64_windows",
                "-o", str(output),
                "--zig-arg=-O", "--zig-arg=ReleaseFast",
            ])
            self.assertEqual(rc, 0, output.read_text(encoding="utf-8") if output.is_file() else "no output")
            document = json.loads(output.read_text(encoding="utf-8"))
            by_link = {symbol["link_name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertIn("c_mul", by_link)
            self.assertIn("pair_add", by_link)
            flags = " ".join(document["source"]["compiler"].get("flags") or [])
            self.assertIn("ReleaseFast", flags)

    def test_user_optimize_mode_is_not_overridden(self) -> None:
        source = "export fn answer() i32 { return 42; }\n"
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.zig"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "native",
                "--target", "x64_windows",
                "-o", str(output),
                "--zig-arg=-O", "--zig-arg=ReleaseSmall",
            ])
            self.assertEqual(rc, 0)
            document = json.loads(output.read_text(encoding="utf-8"))
            flags = document["source"]["compiler"].get("flags") or []
            self.assertIn("ReleaseSmall", flags)
            self.assertNotIn("ReleaseFast", flags)

    def test_releasefast_still_exports_unmanaged_pub_zig(self) -> None:
        source = (
            "pub const Native = struct { flag: u8, wide: u64 };\n"
            "pub fn zig_only(value: Native) u8 { return value.flag; }\n"
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.zig"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "native",
                "--target", "x64_windows",
                "-o", str(output),
                "--zig-arg=-O", "--zig-arg=ReleaseFast",
            ])
            self.assertEqual(rc, 0, output.read_text(encoding="utf-8") if output.is_file() else "no output")
            document = json.loads(output.read_text(encoding="utf-8"))
            by_name = {symbol["name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertIn("zig_only", by_name)
            self.assertEqual(by_name["zig_only"]["calling_convention"], "zig")
            self.assertIn("zig_only", by_name["zig_only"]["link_name"])
            rejected = {item["name"]: item["reason"] for item in document["exports"]["rejected_symbols"]}
            self.assertNotIn("zig_only", rejected)
            flags = " ".join(document["source"]["compiler"].get("flags") or [])
            self.assertIn("ReleaseFast", flags)

    def test_deny_rejected_accepts_zig_calling_convention(self) -> None:
        source = (
            "pub const Native = struct { flag: u8, wide: u64 };\n"
            "pub fn zig_only(value: Native) u8 { return value.flag; }\n"
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.zig"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "native",
                "--target", "x64_windows",
                "-o", str(output),
                "--deny-rejected",
            ])
            self.assertEqual(rc, 0, output.read_text(encoding="utf-8") if output.is_file() else "no output")
            document = json.loads(output.read_text(encoding="utf-8"))
            by_name = {symbol["name"]: symbol for symbol in document["exports"]["symbols"]}
            self.assertIn("zig_only", by_name)
            self.assertEqual(by_name["zig_only"]["calling_convention"], "zig")

    def test_deny_rejected_fails_on_anytype(self) -> None:
        source = "pub fn bad(value: anytype) void { _ = value; }\n"
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "lib.zig"
            root.write_text(source, encoding="utf-8")
            output = Path(temp_dir) / "contract.json"
            rc = adapter.main([
                str(root), "--crate-name", "native",
                "--target", "x64_windows",
                "-o", str(output),
                "--deny-rejected",
            ])
            self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()

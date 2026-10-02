from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


def _portable_contract(compiler_version: str) -> dict:
    return {
        "dci": "1.0",
        "kind": "abi",
        "profile": {
            "id": "native-object-v1",
            "version": "1.0",
            "level": "L3",
            "consumer_modes": ["direct", "stub"],
        },
        "target": {
            "triple": "x86_64-linux-gnu",
            "architecture": "x86_64",
            "pointer_width": 64,
            "endianness": "little",
            "abi_family": "itanium",
        },
        "control_flow": {
            "default_boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
        },
        "source": {
            "language": "cpp",
            "compiler": {
                "name": "gcc",
                "vendor": "gcc",
                "version": compiler_version,
                "target_triplet": "x86_64-linux-gnu",
                "flags": ["-fpack-struct=1", "-DFEATURE_ON=1"],
            },
        },
        "exports": {"layouts": [], "symbols": []},
    }


MODULE_PATH = Path(__file__).parents[1] / "dci.py"
SPEC = importlib.util.spec_from_file_location("dci_cli", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
dci_cli = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = dci_cli
SPEC.loader.exec_module(dci_cli)


class DciCliTests(unittest.TestCase):
    def test_target_aliases(self) -> None:
        self.assertEqual(
            dci_cli.canonical_target("x64_windows"),
            "x86_64-pc-windows-msvc",
        )
        self.assertEqual(
            dci_cli.canonical_target("android_arm64"),
            "aarch64-linux-android23",
        )
        self.assertEqual(
            dci_cli.canonical_target("riscv64-unknown-linux-gnu"),
            "riscv64-unknown-linux-gnu",
        )

    def test_encode_decode_and_inspect_summary(self) -> None:
        document = {
            "dci": "1.0",
            "kind": "abi",
            "profile": {"name": "portable", "target": "x86_64-pc-windows-msvc"},
            "source": {"language": "cpp"},
            "exports": {
                "layouts": [{"type_name": "Api.Value"}],
                "symbols": [{"name": "api_value"}, {"name": "api_drop"}],
                "dispatch_tables": [],
                "type_aliases": [],
                "rejected_symbols": [{"selector": "Api.bad"}],
            },
        }
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "api.dci"
            binary = root / "api.dcib"
            decoded = root / "decoded.json"
            source.write_text(json.dumps(document), encoding="utf-8")

            self.assertEqual(
                dci_cli.main(["encode", str(source), str(binary)]), 0
            )
            self.assertEqual(
                dci_cli.main(["decode", str(binary), str(decoded)]), 0
            )
            self.assertEqual(json.loads(decoded.read_text(encoding="utf-8")), document)

            summary = dci_cli._contract_summary(binary, dci_cli.load_contract(binary))
            self.assertEqual(summary["format"], "dcib")
            self.assertEqual(summary["layouts"], 1)
            self.assertEqual(summary["symbols"], 2)
            self.assertEqual(summary["rejected_symbols"], 1)

    def test_adapter_requires_header(self) -> None:
        self.assertEqual(dci_cli.main(["adapter"]), 2)

    def test_triplet_is_canonical_and_target_is_compatible(self) -> None:
        parser = dci_cli.build_parser()
        canonical = parser.parse_args(
            ["adapter", "api.hpp", "--triplet", "arm64_windows"]
        )
        legacy = parser.parse_args(
            ["adapter", "api.hpp", "--target", "x64_windows"]
        )
        self.assertEqual(canonical.triplet, "arm64_windows")
        self.assertEqual(legacy.triplet, "x64_windows")

    def test_rust_adapter_dispatches_to_rustc_verified_producer(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "lib.rs"
            output = root / "contract.dcib"
            source.write_text("pub fn internal() {}", encoding="utf-8")
            with mock.patch.object(dci_cli.dci_adapter_rust, "main", return_value=0) as run:
                rc = dci_cli.main([
                    "adapter", "--language", "rust", str(source),
                    "--rustc", "custom-rustc", "--crate-name", "native_api",
                    "--edition", "2024", "--triplet", "android_arm64",
                    "--deny-rejected", "-o", str(output), "--", "--cfg", "dci_test",
                ])
        self.assertEqual(rc, 0)
        argv = run.call_args.args[0]
        self.assertIn("--rustc", argv)
        self.assertIn("custom-rustc", argv)
        self.assertIn("--crate-name", argv)
        self.assertIn("native_api", argv)
        self.assertIn("aarch64-linux-android23", argv)
        self.assertIn("--deny-rejected", argv)
        self.assertEqual(argv[-3:], ["--cfg", "dci_test", "--deny-rejected"])

    def test_zig_adapter_dispatches_to_zig_verified_producer(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "lib.zig"
            output = root / "contract.dcib"
            source.write_text("export fn answer() i32 { return 42; }", encoding="utf-8")
            with mock.patch.object(dci_cli.dci_adapter_zig, "main", return_value=0) as run:
                rc = dci_cli.main([
                    "adapter", "--language", "zig", str(source),
                    "--zig", "custom-zig", "--crate-name", "native_api",
                    "--triplet", "android_arm64",
                    "--deny-rejected", "-o", str(output), "--", "-lc",
                ])
        self.assertEqual(rc, 0)
        argv = run.call_args.args[0]
        self.assertIn("--zig", argv)
        self.assertIn("custom-zig", argv)
        self.assertIn("--crate-name", argv)
        self.assertIn("native_api", argv)
        self.assertIn("aarch64-linux-android23", argv)
        self.assertIn("--deny-rejected", argv)
        self.assertEqual(argv[-2:], ["-lc", "--deny-rejected"])

    def test_strict_validation_does_not_lock_compiler_version(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            contract = Path(td) / "api.dci"
            contract.write_text(
                json.dumps(_portable_contract("99.99.99-not-installed")),
                encoding="utf-8",
            )
            rc = dci_cli.main(["validate", "--strict", str(contract)])
        self.assertEqual(rc, 0)

    def test_inspect_json_reports_compiler_vendor_version_triplet_flags(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            contract = Path(td) / "api.dci"
            contract.write_text(
                json.dumps(_portable_contract("13.2.1")), encoding="utf-8"
            )
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                rc = dci_cli.main(["inspect", "--json", str(contract)])
        self.assertEqual(rc, 0)
        summary = json.loads(buffer.getvalue())
        self.assertEqual(summary["compiler_vendor"], "gcc")
        self.assertEqual(summary["compiler_version"], "13.2.1")
        self.assertEqual(summary["compiler_triplet"], "x86_64-linux-gnu")
        self.assertEqual(
            summary["compiler_flags"], ["-fpack-struct=1", "-DFEATURE_ON=1"]
        )

    def test_cmake_build_path_flows_through_to_cpp_adapter(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            header = root / "api.hpp"
            header.write_text("extern int api();\n", encoding="utf-8")
            build = root / "cmake-build-release"
            build.mkdir()
            (build / "compile_commands.json").write_text("[]", encoding="utf-8")
            output = root / "contract.dcib"
            with mock.patch.object(dci_cli.dci_adapter_cpp, "main", return_value=0) as run:
                rc = dci_cli.main([
                    "adapter", str(header), "--toolchain", "gcc", "--cxx", "g++",
                    "--triplet", "linux_x64",
                    "--cmake_build_path", str(build),
                    "--compile_flags", "-Iinclude",
                    "--compile_flags", "-DFOO=1",
                    "-o", str(output),
                ])
        self.assertEqual(rc, 0)
        argv = run.call_args.args[0]
        self.assertIn("--cmake_build_path", argv)
        self.assertIn(str(build.resolve()), argv)
        self.assertIn("--compile_flags=-Iinclude", argv)
        self.assertIn("--compile_flags=-DFOO=1", argv)

    def test_cpp_adapter_dispatches_frontend_and_abi_independently(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            header = root / "api.hpp"
            include_dir = root / "include"
            output = root / "contract.dcib"
            header.write_text("extern int api();\n", encoding="utf-8")
            include_dir.mkdir()
            with mock.patch.object(dci_cli.dci_adapter_cpp, "main", return_value=0) as run:
                rc = dci_cli.main([
                    "adapter", "--language", "cpp", str(header),
                    "--toolchain", "gcc", "--cxx", "x86_64-linux-gnu-g++",
                    "--triplet", "linux_x64", "-I", str(include_dir),
                    "--frontend-arg=-fvisibility=hidden", "-o", str(output),
                    "--", "-DAPI_BUILD=1",
                ])
        self.assertEqual(rc, 0)
        argv = run.call_args.args[0]
        self.assertIn("gcc", argv)
        self.assertIn("x86_64-linux-gnu-g++", argv)
        self.assertIn("x86_64-unknown-linux-gnu", argv)
        self.assertIn("-fvisibility=hidden", argv)
        self.assertEqual(argv[-3:], ["-I", str(include_dir.resolve()), "-DAPI_BUILD=1"])

    def test_cpp_adapter_forwards_jobs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            header = root / "api.hpp"
            output = root / "contract.dcib"
            header.write_text("extern int api();\n", encoding="utf-8")
            with mock.patch.object(dci_cli.dci_adapter_cpp, "main", return_value=0) as run:
                rc = dci_cli.main([
                    "adapter", str(header),
                    "--triplet", "windows_x64",
                    "-j", "16",
                    "-o", str(output),
                ])
        self.assertEqual(rc, 0)
        argv = run.call_args.args[0]
        self.assertEqual(argv[argv.index("--jobs") + 1], "16")


if __name__ == "__main__":
    unittest.main()

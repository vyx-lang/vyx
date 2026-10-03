from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


TOOL_DIR = Path(__file__).resolve().parents[1]
if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

import dci_adapter_rust as adapter
import dci_rustc_stub_compile as stub_compile


def _flag_of_link_name(ir_text: str) -> str:
    for line in ir_text.splitlines():
        if "NativePair" in line and "flag_of" in line and line.startswith("define "):
            start = line.find("@")
            end = line.find("(", start)
            if start >= 0 and end > start:
                return line[start + 1:end].strip().strip('"')
    return ""


class RustcStubCompileTests(unittest.TestCase):
    def test_parse_stub_header(self) -> None:
        header = stub_compile.parse_stub_header(
            "// generated\n"
            "// dci-rust-producer=C:/crate/lib.rs\n"
            "// dci-rust-crate-name=native\n"
            "// dci-rust-edition=2021\n"
            "// dci-rust-target=x86_64-pc-windows-msvc\n"
            "#![allow(dead_code)]\n"
        )
        self.assertEqual(header["dci-rust-producer"], "C:/crate/lib.rs")
        self.assertEqual(header["dci-rust-crate-name"], "native")
        self.assertEqual(header["dci-rust-edition"], "2021")
        self.assertEqual(header["dci-rust-target"], "x86_64-pc-windows-msvc")

    def test_staticlib_emits_unmanaged_pub_symbol_at_opt_level_3(self) -> None:
        rustc = shutil.which("rustc")
        if not rustc:
            self.skipTest("rustc is not installed")
        identity = subprocess.run(
            [rustc, "-vV"], capture_output=True, text=True, timeout=30,
        )
        host = ""
        for line in identity.stdout.splitlines():
            if line.startswith("host:"):
                host = line.split(":", 1)[1].strip()
                break
        if not host:
            self.skipTest("cannot read rustc host triple")
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            producer = root / "lib.rs"
            producer.write_text(
                "pub struct NativePair { pub flag: u8 }\n"
                "impl NativePair {\n"
                "    pub fn flag_of(&self) -> u8 { self.flag }\n"
                "}\n",
                encoding="utf-8",
            )
            ir_path = root / "stripped.ll"
            subprocess.run(
                [
                    rustc, str(producer), "--crate-name", "native", "--crate-type", "lib",
                    "--emit=llvm-ir", "-C", "panic=abort", "-C", "opt-level=3",
                    "--edition", "2021", "--target", host, "-o", str(ir_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertFalse(
                _flag_of_link_name(ir_path.read_text(encoding="utf-8")),
                "O3 without keep must not emit NativePair::flag_of",
            )

            crate = adapter.RustParser(producer).parse()
            target = adapter.target_info(rustc, host)
            kept_ir = adapter.compile_identity_ir(
                producer, crate, "native", rustc, target, "2021", ["-C", "opt-level=3"],
            )
            mangled = _flag_of_link_name(kept_ir)
            self.assertTrue(mangled, "keep must force NativePair::flag_of into O3 IR")

            stub = root / "stub.rs"
            stub.write_text(
                f"// dci-rust-producer={producer.as_posix()}\n"
                "// dci-rust-crate-name=native\n"
                "// dci-rust-edition=2021\n"
                f"// dci-rust-target={host}\n"
                "#![allow(dead_code, unused_imports)]\n"
                "extern crate native;\n"
                "pub use native::*;\n",
                encoding="utf-8",
            )
            output = root / ("native_stub.lib" if sys.platform == "win32" else "libnative_stub.a")
            previous = os.environ.get("VYX_DCI_RUSTC_OPT_LEVEL")
            os.environ["VYX_DCI_RUSTC_OPT_LEVEL"] = "3"
            try:
                stub_compile.compile_stub(stub, output, rustc)
            finally:
                if previous is None:
                    os.environ.pop("VYX_DCI_RUSTC_OPT_LEVEL", None)
                else:
                    os.environ["VYX_DCI_RUSTC_OPT_LEVEL"] = previous
            self.assertTrue(output.is_file())
            archive = output.read_bytes()
            self.assertIn(mangled.encode("ascii"), archive)
            self.assertIn(b"__rust_alloc", archive)

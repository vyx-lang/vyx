"""macOS handoff rejects wrong targets, stale sources and modified objects."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from common import sha256
from macos_common import CPUS, TRIPLES, executable_content, verify_bundle, verify_object
from package_macos import resolve_dependency


class MacOSInputs(unittest.TestCase):
    def test_handoff_checks_every_object_and_both_groups(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            objects = []
            # Valid Mach-O headers isolate the handoff checks from LLVM itself.
            # Real cross-emission is exercised before CI uploads the bundle.
            for group in ("compiler", "runtime"):
                path = root / (group + ".o")
                path.write_bytes(struct.pack("<8I", 0xFEEDFACF, CPUS["aarch64"], 0, 1, 0, 0, 0, 0))
                objects.append({"group": group, "path": path.name, "sha256": sha256(path)})
            manifest = {"schema": 1, "arch": "aarch64", "triple": TRIPLES["aarch64"],
                        "source_commit": "a" * 40, "objects": objects}

            def write():
                (root / "BOOTSTRAP.json").write_text(json.dumps(manifest))

            write()
            _, groups = verify_bundle(root, "aarch64", "a" * 40)
            self.assertEqual(set(groups), {"compiler", "runtime"})
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                verify_bundle(root, "x86_64", "a" * 40)
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                verify_bundle(root, "aarch64", "b" * 40)
            with self.assertRaisesRegex(ValueError, "architecture"):
                verify_object(root / "compiler.o", "x86_64")
            (root / "compiler.o").write_bytes(b"corruption")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                verify_bundle(root, "aarch64", "a" * 40)
            objects[0]["path"] = "../compiler.o"
            write()
            with self.assertRaisesRegex(ValueError, "object path"):
                verify_bundle(root, "aarch64", "a" * 40)
            manifest["objects"] = objects[1:]
            write()
            with self.assertRaisesRegex(ValueError, "requires compiler and runtime"):
                verify_bundle(root, "aarch64", "a" * 40)

    def test_fixpoint_excludes_only_measured_uuid_and_signature(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "compiler"
            header = struct.pack("<8I", 0xFEEDFACF, CPUS["aarch64"], 0, 2, 2, 40, 0, 0)
            image = header + struct.pack("<II", 0x1B, 24) + b"a" * 16
            image += struct.pack("<4I", 0x1D, 16, 80, 8) + b"CODEHERE" + b"sig-data"
            path.write_bytes(image)
            first = executable_content(path)
            path.write_bytes(image[:40] + b"b" * 16 + image[56:80] + b"new-sign")
            self.assertEqual(first["content_sha256"], executable_content(path)["content_sha256"])
            changed = bytearray(path.read_bytes())
            changed[72] ^= 1
            path.write_bytes(changed)
            self.assertNotEqual(first["content_sha256"], executable_content(path)["content_sha256"])

    def test_dylib_resolution_fails_on_missing_or_conflicting_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            roots = [root / "one", root / "two"]
            for path in roots:
                path.mkdir()
            with patch("package_macos.rpaths", return_value=[]):
                with self.assertRaisesRegex(RuntimeError, "Missing or ambiguous"):
                    resolve_dependency("@rpath/libLLVM.dylib", root / "clang", roots)
                for path in roots:
                    (path / "libLLVM.dylib").write_bytes(path.name.encode())
                with self.assertRaisesRegex(RuntimeError, "Missing or ambiguous"):
                    resolve_dependency("@rpath/libLLVM.dylib", root / "clang", roots)
                (roots[1] / "libLLVM.dylib").write_bytes(b"one")
                self.assertEqual(resolve_dependency("@rpath/libLLVM.dylib", root / "clang", roots),
                                 roots[0] / "libLLVM.dylib")


if __name__ == "__main__":
    unittest.main()

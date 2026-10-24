"""macOS handoff rejects wrong targets, stale sources and modified objects."""
import json
import io
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from common import sha256
from macos_common import CPUS, TRIPLES, executable_content, verify_bundle, verify_object
from package_macos import macho, parse_dependencies, resolve_dependency
from prepare_macos_llvm import extract_bottle, resolve_input


class MacOSInputs(unittest.TestCase):
    def test_universal_otool_headers_are_not_dylib_dependencies(self):
        output = ("/tmp/llvm/lib/clang/22/lib/darwin/runtime.dylib (architecture x86_64):\n"
                  "\t@rpath/runtime.dylib (compatibility version 1.0.0, current version 1.0.0)\n"
                  "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)\n"
                  "/tmp/llvm/lib/clang/22/lib/darwin/runtime.dylib (architecture arm64):\n"
                  "\t@rpath/runtime.dylib (compatibility version 1.0.0, current version 1.0.0)\n")
        self.assertEqual(parse_dependencies(output), ["@rpath/runtime.dylib",
                         "/usr/lib/libSystem.B.dylib", "@rpath/runtime.dylib"])

    def test_universal_static_archive_is_not_a_loadable_macho(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile_osx.a"
            # Same FAT header as the shipped compiler-rt profile archive.
            header = struct.pack(">7I", 0xCAFEBABE, 1, CPUS["x86_64"], 0, 32, 32, 3)
            prefix = header.ljust(32, b"\0")
            path.write_bytes(prefix + b"!<arch>\n" + b"\0" * 24)
            self.assertFalse(macho(path))
            path.write_bytes(prefix + struct.pack("<8I", 0xFEEDFACF, CPUS["x86_64"], 0, 6, 0, 0, 0, 0))
            self.assertTrue(macho(path))

    def test_llvm_strip_alias_keeps_objcopy_target_and_does_not_hash_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "llvm.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                item = tarfile.TarInfo("llvm/22.1.8/bin")
                item.type = tarfile.DIRTYPE
                bundle.addfile(item)
                item = tarfile.TarInfo("llvm/22.1.8/bin/llvm-strip")
                item.type, item.linkname = tarfile.SYMTYPE, "llvm-objcopy"
                bundle.addfile(item)
                for name in ("bin/llvm-objcopy", "LICENSE.TXT"):
                    item = tarfile.TarInfo("llvm/22.1.8/" + name)
                    item.size = 3
                    bundle.addfile(item, io.BytesIO(b"obj"))
            target = root / "sdk"
            files = extract_bottle(archive, target, "llvm")
            self.assertNotIn("bin", files)
            self.assertEqual((target / "bin/llvm-strip").read_bytes(), b"obj")
            self.assertTrue(all((target / path).is_file() for path in files))

    def test_pinned_bottle_extracts_real_z3_link_chain_and_license(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "z3.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                # These are the actual names/link structure in both pinned
                # bottles. Put links first to cover extraction ordering too.
                for name, target in (("libz3.dylib", "libz3.4.16.dylib"),
                                     ("libz3.4.16.dylib", "libz3.4.16.0.0.dylib")):
                    item = tarfile.TarInfo("z3/4.16.0/lib/" + name)
                    item.type, item.linkname = tarfile.SYMTYPE, target
                    bundle.addfile(item)
                for name, data in (("lib/libz3.4.16.0.0.dylib", b"actual library"),
                                   ("LICENSE.txt", b"MIT license"), ("bin/z3", b"unused")):
                    item = tarfile.TarInfo("z3/4.16.0/" + name)
                    item.size = len(data)
                    bundle.addfile(item, io.BytesIO(data))
            target = root / "sdk"
            extract_bottle(archive, target, "z3")
            self.assertEqual((target / "lib/libz3.4.16.dylib").read_bytes(), b"actual library")
            self.assertEqual((target / "lib/libz3.dylib").read_bytes(), b"actual library")
            self.assertEqual((target / "licenses/z3/LICENSE.txt").read_bytes(), b"MIT license")
            self.assertFalse((target / "bin/z3").exists())

    def test_pinned_bottle_rejects_dangling_or_escaping_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, target in enumerate(("missing.dylib", "../../../outside")):
                archive = root / f"{index}.tar.gz"
                with tarfile.open(archive, "w:gz") as bundle:
                    item = tarfile.TarInfo("z3/4.16.0/lib/libz3.4.16.dylib")
                    item.type, item.linkname = tarfile.SYMTYPE, target
                    bundle.addfile(item)
                with self.assertRaises((ValueError, tarfile.TarError)):
                    extract_bottle(archive, root / f"sdk-{index}", "z3")

    def test_pinned_dependencies_ignore_rolling_homebrew_and_reject_unknown_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            sdk, brew = root / "sdk", root / "brew"
            library = sdk / "lib/libz3.4.16.dylib"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"pinned ABI")
            rolling = brew / "opt/z3/lib/libz3.4.16.dylib"
            rolling.parent.mkdir(parents=True)
            rolling.write_bytes(b"different payload")
            name = brew.as_posix() + "/opt/z3/lib/libz3.4.16.dylib"
            self.assertEqual(resolve_input(name, sdk / "bin/clang", sdk, brew.as_posix()), library)
            with self.assertRaisesRegex(RuntimeError, "Unpinned"):
                resolve_input("@@HOMEBREW_PREFIX@@/opt/python/Frameworks/Python", sdk / "bin/clang",
                              sdk, brew.as_posix())

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
                                 (roots[0] / "libLLVM.dylib").resolve())


if __name__ == "__main__":
    unittest.main()

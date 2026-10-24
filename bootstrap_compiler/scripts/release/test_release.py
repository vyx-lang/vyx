"""Release naming, collision handling and artifact integrity regression checks."""
from datetime import date, datetime, timezone
import json
import io
from pathlib import Path
import tarfile
import subprocess
import tempfile
import unittest
import urllib.error

from common import REPO, checksum_sidecar, sha256
from build_termux import debian_version
from build_native import bootstrap_manifest, verify_debug_adapter
from prepare_macos_llvm import brew_formula_and_relative, dependency_rewrite, selected_member
from prepare_windows_llvm import extract_development, relative_member
from publish import RECORD_NAMES, SDK_NAMES, reserve_tag, tag_candidate, validate_assets


class ReleaseTests(unittest.TestCase):
    def test_windows_checkout_preserves_debugger_patch_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            patch_relative = Path("tools/debugger/patches/codelldb-1.12.3-vyx.patch")
            source = REPO / patch_relative
            (root / patch_relative).parent.mkdir(parents=True)
            (root / patch_relative).write_bytes(source.read_bytes())
            (root / ".gitattributes").write_bytes((REPO / ".gitattributes").read_bytes())
            for command in (["git", "init", "--quiet"],
                            ["git", "config", "core.autocrlf", "true"],
                            ["git", "add", ".gitattributes", patch_relative.as_posix()]):
                subprocess.run(command, cwd=root, check=True, capture_output=True)
            exported = root / "checkout"
            subprocess.run(["git", "checkout-index", "--all", f"--prefix={exported.as_posix()}/"],
                           cwd=root, check=True, capture_output=True)
            self.assertEqual((exported / patch_relative).read_bytes(), source.read_bytes())
            self.assertNotIn(b"\r\n", (exported / patch_relative).read_bytes())

    def test_debug_adapter_rejects_checkout_and_binary_hash_mismatches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            adapter = root / "adapter-bundle"
            (adapter / "adapter").mkdir(parents=True)
            patch = root / "source.patch"
            patch.write_bytes(b"patch\ncontent\n")
            (adapter / "VYX-CHANGES.patch").write_bytes(patch.read_bytes())
            binary = adapter / "adapter/codelldb.exe"
            binary.write_bytes(b"patched adapter fixture")
            (adapter / "MANIFEST.json").write_text(json.dumps({
                "platform": "windows-x86_64", "patch_sha256": sha256(patch),
                "binary_sha256": sha256(binary)}))
            verify_debug_adapter(adapter, "windows-x86_64", patch)
            patch.write_bytes(b"patch\r\ncontent\r\n")
            with self.assertRaisesRegex(RuntimeError, "patch hash mismatch"):
                verify_debug_adapter(adapter, "windows-x86_64", patch)
            patch.write_bytes(b"patch\ncontent\n")
            binary.write_bytes(b"changed adapter")
            with self.assertRaisesRegex(RuntimeError, "executable hash differs"):
                verify_debug_adapter(adapter, "windows-x86_64", patch)

    def test_bootstrap_explicit_std_sources_use_stage0(self):
        manifest = 'version = "1.0.0-alpha.1"\nparallel_std_sources = "std"\n'
        changed = bootstrap_manifest(manifest, Path("C:/SDK/Stage 0"))
        self.assertIn('parallel_std_sources = "C:/SDK/Stage 0/std"', changed)
        self.assertEqual(manifest, 'version = "1.0.0-alpha.1"\nparallel_std_sources = "std"\n')
        with self.assertRaisesRegex(RuntimeError, "selection drifted"):
            bootstrap_manifest("unexpected manifest", Path("sdk"))

    def test_streamed_llvm_extraction_filters_unused_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "llvm.tar.xz"
            with tarfile.open(archive, "w:xz") as bundle:
                for name in ("include/llvm/IR/Module.h", "lib/LLVMCore.lib", "bin/clang.exe",
                             "bin/lld-link.exe", "bin/llvm-ar.exe", "lib/clang/22/include/stddef.h",
                             "bin/unused.exe", "lib/LLVMCore.pdb", "share/doc/index.html"):
                    member = tarfile.TarInfo("llvm-root/" + name)
                    member.size = 1
                    bundle.addfile(member, io.BytesIO(b"x"))
                link = tarfile.TarInfo("llvm-root/bin/clang++.exe")
                link.type, link.linkname = tarfile.LNKTYPE, "llvm-root/bin/clang.exe"
                bundle.addfile(link)
            extract_development(archive, root / "llvm")
            self.assertTrue((root / "llvm/bin/clang-cl.exe").is_file())
            self.assertEqual((root / "llvm/bin/clang++.exe").read_bytes(), b"x")
            self.assertFalse((root / "llvm/bin/unused.exe").exists())
            self.assertFalse((root / "llvm/lib/LLVMCore.pdb").exists())
            for unsafe in ("llvm/../escape", "/absolute/path", "C:/escape", "llvm/..\\escape"):
                with self.assertRaises(ValueError):
                    relative_member(unsafe)

    def test_macos_brew_paths_are_resolved_by_formula(self):
        prefix = Path("/opt/homebrew")
        self.assertEqual(
            brew_formula_and_relative(prefix / "opt/z3/lib/libz3.4.16.dylib", prefix),
            ("z3", Path("lib/libz3.4.16.dylib")))
        self.assertEqual(
            brew_formula_and_relative(prefix / "Cellar/zstd/1.5.6/lib/libzstd.1.dylib", prefix),
            ("zstd", Path("lib/libzstd.1.dylib")))
        self.assertIsNone(brew_formula_and_relative(Path("/usr/lib/libc++.1.dylib"), prefix))

    def test_macos_bottle_placeholders_prefer_sdk_paths(self):
        destination = Path("/tmp/llvm-22")
        prefix = Path("/opt/homebrew")
        rewritten_llvm = dependency_rewrite(
            "@@HOMEBREW_PREFIX@@/opt/llvm/lib/libLLVM.dylib", destination, prefix)
        self.assertTrue(rewritten_llvm.replace("\\", "/").endswith("/tmp/llvm-22/lib/libLLVM.dylib"))
        rewritten_zstd = dependency_rewrite(
            "@@HOMEBREW_PREFIX@@/opt/zstd/lib/libzstd.1.dylib", destination, prefix)
        self.assertTrue(rewritten_zstd.replace("\\", "/").endswith("/opt/homebrew/opt/zstd/lib/libzstd.1.dylib"))
        rewritten_installed_llvm = dependency_rewrite(
            prefix.as_posix().replace("\\", "/") + "/opt/llvm/lib/libLLVM.dylib",
            destination, prefix)
        self.assertTrue(rewritten_installed_llvm.replace("\\", "/").endswith("/tmp/llvm-22/lib/libLLVM.dylib"))

    def test_macos_bottle_excludes_unused_lldb_and_mlir_trees(self):
        self.assertTrue(selected_member(Path("lib/libLLVM.dylib")))
        self.assertTrue(selected_member(Path("lib/clang/22/lib/darwin/libclang_rt.asan_osx_dynamic.dylib")))
        self.assertFalse(selected_member(Path("lib/liblldb.22.1.8.dylib")))
        self.assertFalse(selected_member(Path("lib/libMLIR.dylib")))

    def test_termux_upgrade_from_historically_misnumbered_package(self):
        now = datetime(2026, 10, 23, 12, 0, tzinfo=timezone.utc)
        version = debian_version("1.0.0-alpha.1", "1.0.5~android21.20261009", now=now,
                                 compare=lambda left, right: False)
        self.assertEqual(version, "1:1.0.0~alpha.1+20261023120000")
        version = debian_version("1.0.0-alpha.1", "1:1.0.0~alpha.1+20261022120000", now=now,
                                 compare=lambda left, right: True)
        self.assertEqual(version, "1:1.0.0~alpha.1+20261023120000")

    def test_actual_version_and_same_day_suffix(self):
        day = date(2026, 10, 23)
        self.assertEqual(tag_candidate("1.0.0-alpha.1", day, 1), "1.0.0-alpha.1.26.10.23")
        self.assertEqual(tag_candidate("1.0.0-alpha.1", day, 2), "1.0.0-alpha.1.26.10.23.2")
        self.assertEqual(tag_candidate("2.1.0", day, 12), "2.1.0.26.10.23.12")

    def test_existing_and_concurrently_created_tags_are_preserved(self):
        existing = {"1.0.0-alpha.1.26.10.23"}
        posted = []

        def request(path, method="GET", data=None):
            if method == "GET":
                if path.rsplit("/", 1)[1] not in existing:
                    raise urllib.error.HTTPError(path, 404, "missing", {}, None)
                return {}
            tag = data["ref"].removeprefix("refs/tags/")
            posted.append(tag)
            existing.add(tag)
            if tag.endswith(".2"):
                raise urllib.error.HTTPError(path, 422, "race", {}, None)
            return {}

        result = reserve_tag("1.0.0-alpha.1", date(2026, 10, 23), "a" * 40, request)
        self.assertEqual(result, "1.0.0-alpha.1.26.10.23.3")
        self.assertEqual(len(existing), 3)
        self.assertEqual(len(posted), 2)

    def test_complete_set_and_source_identity_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            commit = "a" * 40
            for name, record in zip(SDK_NAMES, RECORD_NAMES):
                package = directory / name
                package.write_bytes(b"fixture sdk")
                checksum_sidecar(package)
                (directory / record).write_text(json.dumps({"source_commit": commit,
                    "archive_sha256": sha256(package), "archive_bytes": package.stat().st_size}))
            self.assertEqual(len(validate_assets(directory, commit)), 18)
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                validate_assets(directory, "b" * 40)
            (directory / SDK_NAMES[0]).write_bytes(b"corrupt sdk")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                validate_assets(directory, commit)

    def test_missing_platform_cannot_publish(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(FileNotFoundError):
                validate_assets(Path(temporary), "a" * 40)


if __name__ == "__main__":
    unittest.main()

"""Linux bootstrap LLVM must use the verified SDK's exact dependency closure."""
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

import prepare_linux_llvm as linux


class LinuxPreparationTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("pwsh"), "PowerShell native argument forwarding")
    def test_workflow_forwards_both_llvm_selections_as_whole_arguments(self):
        workflow = Path(__file__).resolve().parents[3] / ".github/workflows/sdk-release.yml"
        expression = next(line.strip() for line in workflow.read_text(encoding="utf-8").splitlines()
                          if "$llvmInput = if" in line)
        python = "'" + sys.executable.replace("'", "''") + "'"
        for windows, expected in ((True, ["--llvm-root", "C:/LLVM with spaces"]),
                                  (False, ["--use-sdk-llvm"])):
            with self.subTest(windows=windows):
                selection = expression.replace("$IsWindows", "$true" if windows else "$false")
                command = ("$env:LLVM_ROOT = 'C:/LLVM with spaces'; " + selection + "; & " + python
                           + " -c 'import json,sys; print(json.dumps(sys.argv[1:]))' @llvmInput")
                result = subprocess.run([shutil.which("pwsh"), "-NoProfile", "-NonInteractive",
                                         "-Command", command], capture_output=True, text=True, check=True, timeout=30)
                self.assertEqual(json.loads(result.stdout), expected)

    def fixture(self, root):
        sdk = root / "sdk"
        files = ["SDK-MANIFEST.json", "toolchain/include/llvm/IR/Module.h",
                 "toolchain/include/llvm/IR/LLVMContext.h", "toolchain/include/llvm/Config/llvm-config.h",
                 "toolchain/include/llvm-c/Core.h", "toolchain/lib/clang/22/include/stddef.h",
                 "bin/libvyx_compiler_backend.so", "lib/libLLVM.so.22.1",
                 "lib/libclang-cpp.so.22.1", "lib/libdep.so.1", "lib/libtbb.so.12",
                 "lib/libz.so.1", "lib/libzstd.so.1"]
        files += ["toolchain/bin/" + name for name in linux.TOOLS]
        for name in files:
            target = sdk / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(name.encode())
        (sdk / "SDK-MANIFEST.json").write_text(json.dumps({"bundled_compiler_llvm_toolchain": True}))
        return sdk.resolve()

    def dependencies(self, source):
        return {
            "libvyx_compiler_backend.so": ["libLLVM.so.22.1", "libtbb.so.12"],
            "clang": ["libclang-cpp.so.22.1"],
            "libLLVM.so.22.1": ["libdep.so.1", "libz.so.1", "libzstd.so.1", "libc.so.6"],
            "libclang-cpp.so.22.1": ["libLLVM.so.22.1"],
        }.get(source.name, [])

    def test_failed_ubuntu_update_stops_package_installation(self):
        failure = subprocess.CalledProcessError(100, ["apt-get", "update"])
        with patch.object(linux, "run", side_effect=failure) as run, redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                linux.install_system_dependencies()
        self.assertEqual(run.call_count, 1)

    def test_missing_sdk_dependency_is_not_resolved_from_host(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = self.fixture(root)
            (sdk / "lib/libdep.so.1").unlink()
            with patch.object(linux, "needed", side_effect=self.dependencies):
                with self.assertRaises(FileNotFoundError):
                    linux.prepare_sdk_llvm(sdk, root / "llvm")
            self.assertFalse((root / "llvm").exists())

    def test_ambiguous_llvm_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            sdk = self.fixture(Path(temporary))
            with patch.object(linux, "needed", return_value=["libLLVM.so.22.1", "libLLVM-23.so"]):
                with self.assertRaisesRegex(RuntimeError, "exactly one LLVM"):
                    linux.llvm_dependency(sdk)

    def test_dependency_with_absolute_path_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = self.fixture(root)
            def needed(source):
                if source.name == "libLLVM.so.22.1": return ["/usr/lib/libdep.so.1"]
                return self.dependencies(source)
            with patch.object(linux, "needed", side_effect=needed):
                with self.assertRaisesRegex(RuntimeError, "Non-relocatable"):
                    linux.prepare_sdk_llvm(sdk, root / "llvm")

    @unittest.skipUnless(os.name == "posix", "Linux SDK directory symlinks")
    def test_exact_closure_excludes_stage0_application_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = self.fixture(root)
            with patch.object(linux, "needed", side_effect=self.dependencies), \
                 patch.object(linux, "verify", return_value="22.1.8"), redirect_stdout(io.StringIO()):
                destination = linux.prepare_sdk_llvm(sdk, root / "llvm")
            record = json.loads((destination / "VYX-LLVM-INPUT.json").read_text())
            self.assertEqual(set(record["libraries"]), {"libLLVM.so.22.1", "libclang-cpp.so.22.1",
                                                       "libdep.so.1", "libz.so.1", "libzstd.so.1"})
            self.assertEqual((destination / "lib/libLLVM.so").resolve(), (sdk / "lib/libLLVM.so.22.1").resolve())
            self.assertFalse((destination / "lib/libtbb.so.12").exists())

    @unittest.skipUnless(os.name == "posix", "Linux SDK directory symlinks")
    def test_version_probe_failure_does_not_mark_inputs_verified(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = self.fixture(root)
            with patch.object(linux, "needed", side_effect=self.dependencies), \
                 patch.object(linux, "verify", side_effect=subprocess.CalledProcessError(1, ["input-check"])):
                with self.assertRaises(subprocess.CalledProcessError):
                    linux.prepare_sdk_llvm(sdk, root / "llvm")
            self.assertFalse((root / "llvm/VYX-LLVM-INPUT.json").exists())


if __name__ == "__main__":
    unittest.main()

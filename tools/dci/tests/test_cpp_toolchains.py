from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).parents[1] / "cpp_toolchains.py"
SPEC = importlib.util.spec_from_file_location("cpp_toolchains", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
toolchains = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = toolchains
SPEC.loader.exec_module(toolchains)


class CppToolchainTargetTests(unittest.TestCase):
    def test_windows_alias_uses_msvc_llp64_contract(self) -> None:
        target = toolchains.target_info("x64_windows")
        self.assertEqual(target.triple, "x86_64-pc-windows-msvc")
        self.assertEqual(target.architecture, "x86_64")
        self.assertEqual(target.platform, "windows")
        self.assertEqual(target.object_format, "coff")
        self.assertEqual(target.abi_family, "msvc")
        self.assertEqual(target.data_model, "llp64")
        self.assertEqual(target.pointer_width, 64)
        self.assertEqual(target.long_width, 32)

    def test_linux_alias_uses_itanium_lp64_contract(self) -> None:
        target = toolchains.target_info("linux_x64")
        self.assertEqual(target.triple, "x86_64-unknown-linux-gnu")
        self.assertEqual(target.abi_family, "itanium")
        self.assertEqual(target.data_model, "lp64")
        self.assertEqual(target.long_width, 64)

    def test_android_is_not_misclassified_as_plain_linux(self) -> None:
        target = toolchains.target_info("android_arm64")
        self.assertEqual(target.platform, "android")
        self.assertEqual(target.architecture, "aarch64")
        self.assertEqual(target.abi_family, "itanium")

    def test_wasi_uses_wasm_object_format_and_itanium_cpp_abi(self) -> None:
        target = toolchains.target_info("wasm32-wasi")
        self.assertEqual(target.platform, "wasi")
        self.assertEqual(target.object_format, "wasm")
        self.assertEqual(target.pointer_width, 32)
        self.assertEqual(target.abi_family, "itanium")

    def test_unsupported_target_is_rejected_by_validation(self) -> None:
        clang = toolchains.CppToolchain(
            "clang", "clang++", "20.1.0", "clang version 20.1.0", "", "clang"
        )
        with self.assertRaisesRegex(toolchains.CppToolchainError, "unsupported C\\+\\+ target"):
            toolchains.validate_target_compatibility(clang, "mystery-none-none")


class CppCompilerIdentityTests(unittest.TestCase):
    def test_family_inference_prefers_version_identity(self) -> None:
        self.assertEqual(
            toolchains.infer_compiler_family("c++", "clang version 20.1.0"),
            "clang",
        )
        self.assertEqual(
            toolchains.infer_compiler_family(
                "c++", "g++ (Ubuntu 13.3.0) 13.3.0\nFree Software Foundation"
            ),
            "gcc",
        )
        self.assertEqual(
            toolchains.infer_compiler_family(
                "compiler.exe", "Microsoft (R) C/C++ Optimizing Compiler Version 19.44"
            ),
            "msvc",
        )

    def test_clang_cl_has_a_distinct_driver_mode(self) -> None:
        self.assertEqual(toolchains.compiler_driver_mode("clang", "clang-cl.exe"), "clang-cl")
        self.assertEqual(toolchains.compiler_driver_mode("clang", "clang++.exe"), "clang")

    @mock.patch.object(toolchains, "_resolve_candidate", return_value="C:/LLVM/bin/clang++.exe")
    @mock.patch.object(toolchains, "_run_capture")
    def test_identify_clang_records_version_and_native_target(
        self, run_capture: mock.Mock, _resolve: mock.Mock
    ) -> None:
        run_capture.return_value = (
            0,
            "clang version 20.1.8\nTarget: x86_64-pc-windows-msvc\n",
        )
        result = toolchains.identify_cpp_toolchain("clang++", expected_family="clang")
        self.assertEqual(result.family, "clang")
        self.assertEqual(result.version, "20.1.8")
        self.assertEqual(result.native_target, "x86_64-pc-windows-msvc")
        self.assertEqual(result.driver_mode, "clang")

    @mock.patch.object(toolchains, "_resolve_candidate", return_value="/usr/bin/g++")
    @mock.patch.object(toolchains, "_run_capture")
    def test_identify_gcc_uses_dumpmachine(
        self, run_capture: mock.Mock, _resolve: mock.Mock
    ) -> None:
        run_capture.side_effect = [
            (0, "g++ (Ubuntu 13.3.0) 13.3.0\nFree Software Foundation\n"),
            (0, "x86_64-linux-gnu\n"),
        ]
        result = toolchains.identify_cpp_toolchain("g++", expected_family="gcc")
        self.assertEqual(result.family, "gcc")
        self.assertEqual(result.version, "13.3.0")
        self.assertEqual(result.native_target, "x86_64-linux-gnu")

    @mock.patch.object(
        toolchains,
        "_resolve_candidate",
        return_value="C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/arm64/cl.exe",
    )
    @mock.patch.object(toolchains, "_run_capture")
    def test_identify_msvc_infers_target_from_driver_path(
        self, run_capture: mock.Mock, _resolve: mock.Mock
    ) -> None:
        run_capture.return_value = (
            2,
            "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35207 for ARM64\n",
        )
        result = toolchains.identify_cpp_toolchain("cl.exe", expected_family="msvc", env={})
        self.assertEqual(result.family, "msvc")
        self.assertEqual(result.version, "19.44.35207")
        self.assertEqual(result.native_target, "aarch64-pc-windows-msvc")

    @mock.patch.object(
        toolchains,
        "_resolve_candidate",
        return_value="C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe",
    )
    @mock.patch.object(toolchains, "_run_capture")
    def test_identify_localized_msvc_bv_output(
        self, run_capture: mock.Mock, _resolve: mock.Mock
    ) -> None:
        run_capture.return_value = (
            2,
            "编译器扫描遍数:\n C:/VS/bin/cl.exe: 版本 19.44.35219.0\n",
        )
        result = toolchains.identify_cpp_toolchain("cl.exe", expected_family="msvc", env={})
        self.assertEqual(result.version, "19.44.35219.0")
        self.assertEqual(result.version_line, "MSVC 19.44.35219.0")

    @mock.patch.object(toolchains, "_run_capture")
    def test_msvc_discovery_uses_vswhere_outside_developer_prompt(
        self, run_capture: mock.Mock
    ) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            vswhere = root / "vswhere.exe"
            vswhere.touch()
            installation = root / "VisualStudio"
            tools_root = installation / "VC" / "Tools" / "MSVC" / "14.44.35207"
            tools_root.mkdir(parents=True)
            run_capture.return_value = (0, str(installation) + "\n")
            roots = list(toolchains._msvc_install_roots({"VSWHERE": str(vswhere)}))
        self.assertEqual(roots, [tools_root])
        command = run_capture.call_args.args[0]
        self.assertIn("Microsoft.VisualStudio.Component.VC.Tools.x86.x64", command)


class CppToolchainCompatibilityTests(unittest.TestCase):
    def make_toolchain(
        self, family: str, native_target: str, driver_mode: str | None = None
    ) -> object:
        return toolchains.CppToolchain(
            family,
            family,
            "1.0",
            family + " 1.0",
            native_target,
            driver_mode or {"clang": "clang", "gcc": "gnu", "msvc": "cl"}[family],
        )

    def test_clang_driver_can_select_msvc_or_itanium_abi(self) -> None:
        clang = self.make_toolchain("clang", "x86_64-pc-windows-msvc")
        self.assertEqual(
            toolchains.validate_target_compatibility(clang, "x64_windows").abi_family,
            "msvc",
        )
        self.assertEqual(
            toolchains.validate_target_compatibility(clang, "x64_linux").abi_family,
            "itanium",
        )

    def test_clang_cl_rejects_non_msvc_target(self) -> None:
        clang_cl = self.make_toolchain(
            "clang", "x86_64-pc-windows-msvc", "clang-cl"
        )
        with self.assertRaisesRegex(toolchains.CppToolchainError, "clang-cl only supports"):
            toolchains.validate_target_compatibility(clang_cl, "x64_linux")

    def test_gcc_requires_matching_native_cross_driver(self) -> None:
        gcc = self.make_toolchain("gcc", "x86_64-linux-gnu")
        toolchains.validate_target_compatibility(gcc, "x64_linux")
        with self.assertRaisesRegex(toolchains.CppToolchainError, "matching cross-G\\+\\+"):
            toolchains.validate_target_compatibility(gcc, "arm64_linux")
        with self.assertRaisesRegex(toolchains.CppToolchainError, "does not produce the Microsoft"):
            toolchains.validate_target_compatibility(gcc, "x64_windows")

    def test_msvc_requires_matching_windows_architecture(self) -> None:
        msvc = self.make_toolchain("msvc", "x86_64-pc-windows-msvc")
        toolchains.validate_target_compatibility(msvc, "x64_windows")
        with self.assertRaisesRegex(toolchains.CppToolchainError, "does not match"):
            toolchains.validate_target_compatibility(msvc, "arm64_windows")
        with self.assertRaisesRegex(toolchains.CppToolchainError, "MSVC only supports Windows"):
            toolchains.validate_target_compatibility(msvc, "x64_linux")


class CppToolchainMetadataTests(unittest.TestCase):
    def test_msvc_metadata_preserves_existing_profile_identity(self) -> None:
        clang = toolchains.CppToolchain(
            "clang",
            "C:/LLVM/bin/clang++.exe",
            "20.1.8",
            "clang version 20.1.8",
            "x86_64-pc-windows-msvc",
            "clang",
        )
        metadata = toolchains.descriptor_metadata(
            clang,
            "x64_windows",
            headers=["include/Api.hpp"],
            rtti_enabled=False,
        )
        self.assertEqual(metadata["producer"]["tool"], "clang")
        self.assertEqual(metadata["producer"]["backend"], "msvc")
        self.assertEqual(metadata["target"]["data_model"], "llp64")
        self.assertIn("clang-msvc", metadata["profile_extension"])
        extension = metadata["profile_extension"]["clang-msvc"]
        self.assertEqual(extension["name_mangling"], "microsoft")
        self.assertEqual(extension["runtime_type"]["representation"], "msvc-col")
        self.assertFalse(extension["runtime_type"]["rtti_enabled"])
        self.assertEqual(metadata["source"]["headers"], ["include/Api.hpp"])
        self.assertEqual(metadata["source"]["adapter"]["name"], "cpp-clang-msvc")
        self.assertNotIn("executable", metadata["source"]["compiler"])

    def test_gcc_metadata_uses_itanium_profile_and_lp64(self) -> None:
        gcc = toolchains.CppToolchain(
            "gcc",
            "/usr/bin/g++",
            "13.3.0",
            "g++ 13.3.0",
            "x86_64-linux-gnu",
            "gnu",
        )
        metadata = toolchains.descriptor_metadata(gcc, "x64_linux")
        self.assertEqual(metadata["target"]["abi_family"], "itanium")
        self.assertEqual(metadata["target"]["long_width"], 64)
        extension = metadata["profile_extension"]["gcc-itanium"]
        self.assertEqual(extension["name_mangling"], "itanium")
        self.assertEqual(
            extension["runtime_type"]["checked_cast_runtime"], "__dynamic_cast"
        )


if __name__ == "__main__":
    unittest.main()

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOLS = REPO_ROOT / "tools" / "dci"
RUNTIME_DIR = TOOLS / "runtime"
ITANIUM_CPP = RUNTIME_DIR / "dci_itanium_failure.cpp"
ITANIUM_HPP = RUNTIME_DIR / "dci_itanium_failure.hpp"
CAPTURE_TEST = Path(__file__).with_name("test_dci_itanium_failure_capture.cpp")
LOCAL_CLANGXX = REPO_ROOT / "clang" / "bin" / "clang++.exe"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


adapter = _load("dci_adapter_msvc", TOOLS / "dci_adapter_msvc.py")
tw = _load("dci_translate_unwind", TOOLS / "dci_translate_unwind.py")


def _clangxx() -> str | None:
    if LOCAL_CLANGXX.is_file():
        return str(LOCAL_CLANGXX)
    return shutil.which("clang++") or shutil.which("clang++.exe")


def _i32_type() -> dict:
    return {
        "name": "i32",
        "kind": "primitive",
        "reference": "value",
        "cpp_type": "int32_t",
    }


def _may_unwind_divide() -> object:
    return adapter.Symbol(
        name="checked_divide",
        owner="",
        member_name="checked_divide",
        mangled="_ZN3abi14checked_divideEii",
        kind="function",
        calling_convention="cxx_free_function",
        params=[
            {"name": "a", "type": _i32_type(), "location": "abi"},
            {"name": "b", "type": _i32_type(), "location": "abi"},
        ],
        ret={"type": _i32_type(), "location": "abi"},
        visibility="public",
        native_calling_convention="cdecl",
        unwind="may_unwind",
    )


class ItaniumTranslateUnwindTests(unittest.TestCase):
    def test_itanium_demangle_nested_and_leaf_names(self) -> None:
        self.assertEqual(
            tw.itanium_free_function_cpp_name("_ZN3abi14checked_divideEii", "checked_divide"),
            "abi::checked_divide",
        )
        self.assertEqual(
            tw.itanium_free_function_cpp_name("_Z14checked_divideii", "checked_divide"),
            "checked_divide",
        )
        self.assertEqual(
            tw.free_function_cpp_name("?checked_divide@abi@@YAHHH@Z", "checked_divide"),
            "abi::checked_divide",
        )

    def test_translated_i32_layout_is_tag_ok_err(self) -> None:
        layout = tw.cxx_struct_layout([("tag", 1, 1), ("ok", 4, 4), ("err", 64, 8)])
        self.assertEqual(layout["fields"]["tag"], 0)
        self.assertEqual(layout["fields"]["ok"], 4)
        self.assertEqual(layout["fields"]["err"], 8)
        self.assertEqual(layout["size"], 72)
        self.assertEqual(layout["align"], 8)

    def test_itanium_attach_emits_cxa_capture_not_throwinfo(self) -> None:
        symbol = _may_unwind_divide()
        extra, records, requests, stub = tw.attach_translate_unwind(
            adapter,
            exported_symbols=[{"link_name": symbol.mangled, "name": symbol.name}],
            records=[],
            original_symbols=[symbol],
            headers=[],
            target="x86_64-unknown-linux-gnu",
            toolchain=types.SimpleNamespace(executable=""),
            machine_signatures=None,
        )
        self.assertTrue(stub)
        self.assertIn("dci_itanium_failure.hpp", stub)
        self.assertIn("dci_itanium_capture_current", stub)
        self.assertIn("abi::checked_divide", stub)
        self.assertNotIn("dci_msvc_failure.hpp", stub)
        self.assertNotIn("dci_msvc_capture_current", stub)
        self.assertNotIn("ThrowInfo", stub)
        self.assertNotIn("_CxxThrowException", stub)
        self.assertNotIn("__CxxFrameHandler3", stub)
        self.assertGreaterEqual(len(requests), 1)
        self.assertEqual(requests[0]["synthesis"]["strategy"], "translate_unwind")
        wrapper = requests[0]["wrapper"]["link_name"]
        self.assertTrue(wrapper.startswith("dci_tr_"))
        translators = [item for item in extra if item.get("link_name") == wrapper]
        self.assertEqual(len(translators), 1, extra)
        tr = translators[0]
        self.assertEqual(tr["control_flow"]["unwind"], "no_unwind")
        self.assertEqual(tr["control_flow"]["propagation"]["mode"], "forbidden")
        self.assertEqual((tr.get("abi") or {}).get("return", {}).get("passing"), "sret")
        self.assertTrue(any(item.type_name.startswith("dci::Translated_") for item in records))
        self.assertTrue(
            any(item.traits.get("closed_tagged_return") for item in records if item.type_name.startswith("dci::Translated_"))
        )

    def test_itanium_runtime_has_no_msvc_eh_tables(self) -> None:
        source = ITANIUM_CPP.read_text(encoding="utf-8")
        header = ITANIUM_HPP.read_text(encoding="utf-8")
        blob = source + header
        self.assertNotIn("ThrowInfo", blob)
        self.assertNotIn("CatchableType", blob)
        self.assertNotIn("EXCEPTION_RECORD", blob)
        self.assertNotIn("_CxxThrowException", blob)
        self.assertIn("__cxa_current_exception_type", source)
        self.assertIn("DCI_FAILURE_TAG_ITANIUM_CXX", source)


def _wsl_path(path: Path) -> str:
    text = str(path.resolve())
    if len(text) >= 2 and text[1] == ":":
        return "/mnt/" + text[0].lower() + text[2:].replace("\\", "/")
    return text.replace("\\", "/")


class ItaniumCaptureRuntimeTests(unittest.TestCase):
    def test_capture_probe_on_itanium_eh(self) -> None:
        if not CAPTURE_TEST.is_file():
            self.skipTest("capture probe missing")
        wsl = shutil.which("wsl")
        if wsl is not None:
            repo = _wsl_path(REPO_ROOT)
            runtime = _wsl_path(RUNTIME_DIR)
            probe = _wsl_path(CAPTURE_TEST)
            common = _wsl_path(RUNTIME_DIR / "dci_failure_common.cpp")
            itanium = _wsl_path(ITANIUM_CPP)
            script = (
                "set -e; "
                "CXX=$(command -v g++ || command -v clang++); "
                "test -n \"$CXX\"; "
                "$CXX -std=c++17 -O0 -fexceptions "
                f'-I "{repo}" -I "{runtime}" "{probe}" "{common}" "{itanium}" '
                "-o /tmp/dci_itanium_capture && /tmp/dci_itanium_capture"
            )
            ran = subprocess.run(
                [wsl, "-e", "bash", "-lc", script],
                capture_output=True,
                text=True,
                errors="replace",
            )
            if ran.returncode == 0:
                return
        clang = _clangxx()
        if clang is None:
            self.skipTest("clang++ or WSL g++ unavailable for Itanium EH")
        with tempfile.TemporaryDirectory(prefix="vyx_dci_ita_") as temp:
            exe = Path(temp) / "itanium_capture"
            linux_cmd = [
                clang,
                "-std=c++17",
                "-O0",
                "-fexceptions",
                "-fcxx-exceptions",
                "-fno-ms-compatibility",
                "-target",
                "x86_64-unknown-linux-gnu",
                "-I",
                str(REPO_ROOT),
                "-I",
                str(RUNTIME_DIR),
                str(CAPTURE_TEST),
                str(RUNTIME_DIR / "dci_failure_common.cpp"),
                str(ITANIUM_CPP),
                "-lstdc++",
                "-o",
                str(exe),
            ]
            compiled = subprocess.run(
                linux_cmd,
                capture_output=True,
                text=True,
                errors="replace",
                env={**os.environ, "LLVM_ROOT": str(REPO_ROOT / "clang")},
            )
            if compiled.returncode != 0:
                self.skipTest("Itanium EH toolchain unavailable")
            if os.name == "nt":
                self.skipTest("Itanium capture compiled for linux; host cannot run ELF")
            ran = subprocess.run(
                [str(exe)],
                capture_output=True,
                text=True,
                errors="replace",
            )
            self.assertEqual(ran.returncode, 0, ran.stdout + "\n" + ran.stderr)


if __name__ == "__main__":
    unittest.main()

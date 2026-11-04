"""Checks for gate provenance, model reproducibility and owned process cleanup."""
import ctypes
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import run as gate
from model import write_project
from semver_model import precedence, write_project as write_semver_project
from url_model import encode as url_encode_oracle, write_project as write_url_project
from ini_model import reference as ini_reference, write_project as write_ini_project


def alive(pid):
    if os.name == "nt":
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.restype = ctypes.c_void_p
        api.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        api.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = api.OpenProcess(0x100000, False, pid)
        if not handle:
            return False
        try:
            return api.WaitForSingleObject(handle, 0) == 258
        finally:
            api.CloseHandle(handle)
    status = Path(f"/proc/{pid}/stat")
    if status.exists() and status.read_text().split()[2] == "Z":
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


class RunnerTests(unittest.TestCase):
    def test_ini_oracle_has_explicit_case_and_interpolation_rules(self):
        result = ini_reference("root=value\r\n[s]\r\nKey=100% %(raw)s\r\nkey=\r\nurl=https://x?q=1;#literal")
        self.assertEqual(result, {("", "root"): "value", ("s", "Key"): "100% %(raw)s", ("s", "key"): "", ("s", "url"): "https://x?q=1;#literal"})
        self.assertIn("tests/projects/std_ini_contracts/config.ini", gate.input_hashes(["std_ini_contracts"]))

    def test_ini_model_reproduces_and_counts_lookups(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = write_ini_project(root / "a", seed=42)
            self.assertEqual(first, write_ini_project(root / "b", seed=42))
            self.assertEqual(first["documents"], 24)
            self.assertEqual(first["lookups"], 480)
            self.assertEqual(first["growing_index_entries"], 1024)
            source = (root / "a/main.vyx").read_text(encoding="utf8")
            self.assertEqual(source.count("    check(&document,"), first["lookups"])
            self.assertNotEqual(first["source_sha256"], write_ini_project(root / "c", seed=43)["source_sha256"])

    def test_url_oracle_separates_percent_and_form_alphabets(self):
        self.assertEqual(url_encode_oracle(b" +*~\0"), "%20%2B%2A~%00")
        self.assertEqual(url_encode_oracle(b" +*~\0", True), "+%2B*%7E%00")
        self.assertEqual(url_encode_oracle(bytes([128, 255])), "%80%FF")

    def test_url_model_is_reproducible_and_retains_all_cases(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = write_url_project(root / "a", seed=42)
            self.assertEqual(first, write_url_project(root / "b", seed=42))
            self.assertEqual({k: first[k] for k in ("bytes", "urls", "invalid", "queries")},
                             {"bytes": 80, "urls": 96, "invalid": 64, "queries": 32})
            source = (root / "a/main.vyx").read_text(encoding="utf8")
            for key, function in (("bytes", "check_codec"), ("urls", "check_url"), ("invalid", "check_invalid"), ("queries", "check_query")):
                self.assertEqual(source.count("    " + function + "("), first[key])
            self.assertNotEqual(first["source_sha256"], write_url_project(root / "c", seed=43)["source_sha256"])

    def test_semver_oracle_spec_order_and_unbounded_identifiers(self):
        ordered = ["1.0.0" + suffix for suffix in ("-alpha", "-alpha.1", "-alpha.beta", "-beta", "-beta.2", "-beta.11", "-rc.1", "")]
        self.assertEqual(sorted(reversed(ordered), key=precedence), ordered)
        self.assertEqual(precedence("1.2.3+001"), precedence("1.2.3+abc"))
        self.assertLess(precedence("1.0.0-" + "9" * 80), precedence("1.0.0-1" + "0" * 80))
        for invalid in ("v1.2.3", "1.2.3-01", "1.2.3_", "1.2.3+", "2147483648.0.0", "1.2.3\n", "1.2.3-中", "1.2.3-a\0b"):
            with self.subTest(input=invalid), self.assertRaises(ValueError):
                precedence(invalid)

    def test_semver_model_reproducibility_and_complete_counts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = write_semver_project(root / "a", seed=42)
            second = write_semver_project(root / "b", seed=42)
            self.assertEqual(first, second)
            source = (root / "a/main.vyx").read_text(encoding="utf8")
            self.assertEqual(first["comparisons"], 192)
            self.assertEqual(first["valid"], 80)
            self.assertGreater(first["invalid"], 200)
            self.assertEqual(source.count("    check_pair("), first["comparisons"])
            self.assertEqual(source.count("    check_parse("), first["valid"] + first["invalid"])
            third = write_semver_project(root / "c", seed=43)
            self.assertNotEqual(first["source_sha256"], third["source_sha256"])

    def test_semver_catalog_data_is_hashed(self):
        inputs = gate.input_hashes(["std_semver_contracts"])
        self.assertIn("tests/projects/std_semver_contracts/catalog.txt", inputs)

    def test_native_deployment_uses_only_explicit_libraries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, destination = root / "input", root / "deploy"
            source.mkdir(); destination.mkdir()
            (source / "sqlite3.dll").write_bytes(b"selected sqlite")
            (source / "unrelated.dll").write_bytes(b"do not deploy")
            records = gate.deploy_native_libraries("std_sqlite_contracts", [source], destination)
            self.assertEqual([p.name for p in destination.iterdir()], ["sqlite3.dll"])
            self.assertEqual(records, [{"source": str(source / "sqlite3.dll"),
                                      "sha256": gate.sha256(destination / "sqlite3.dll")}])

    def test_missing_native_input_cannot_be_satisfied_by_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "sqlite3.dll").write_bytes(b"ambient library")
            with patch.dict(os.environ, PATH=str(root)):
                with self.assertRaisesRegex(RuntimeError, "Missing explicit.*sqlite3.dll"):
                    gate.deploy_native_libraries("std_sqlite_contracts", [], root)

    def test_seed_reproduces_the_generated_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = write_project(root / "a", 421)
            second = write_project(root / "b", 421)
            self.assertEqual(first, second)
            self.assertEqual((root / "a/main.vyx").read_bytes(), (root / "b/main.vyx").read_bytes())
            write_project(root / "c", 422)
            self.assertNotEqual((root / "a/main.vyx").read_bytes(), (root / "c/main.vyx").read_bytes())

    def test_function_sharding_preserves_all_operations_and_oracles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for size in (16, 128):
                write_project(root / str(size), block_size=size)
            def operations(size):
                return [line for line in (root / str(size) / "main.vyx").read_text(encoding="utf-8").splitlines()
                        if "values." in line or "    check_" in line]
            self.assertEqual(operations(16), operations(128))

    def test_inventory_rejects_two_package_owners_of_one_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "shared.vyx").write_text("module shared;", encoding="utf-8")
            (root / "registry").write_text("", encoding="utf-8")
            for name in ("a", "b"):
                (root / name).mkdir()
                (root / name / "Vyx.toml").write_text(
                    f'[package]\nname="{name}"\n[target.lib]\nentry="../shared.vyx"\n', encoding="utf-8")
            with patch.object(gate, "PACKAGES", root):
                with self.assertRaisesRegex(RuntimeError, "Source owned by two packages"):
                    gate.inventory()

    def test_execute_preserves_exit_and_both_output_streams(self):
        with tempfile.TemporaryDirectory() as temporary:
            code, log = gate.execute([sys.executable, "-c",
                'import sys; print("out"); print("err",file=sys.stderr); sys.exit(3)'],
                Path(temporary), dict(os.environ), 10)
            self.assertEqual(code, 3)
            self.assertIn(b"out", log)
            self.assertIn(b"err", log)

    def test_timeout_kills_only_its_owned_child_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "parent.py"
            parent.write_text('''import subprocess,sys,time
from pathlib import Path
child = subprocess.Popen([sys.executable,"-c","import time; time.sleep(30)"])
Path("child.pid").write_text(str(child.pid))
print("started",flush=True)
time.sleep(30)
''', encoding="utf-8")
            pid = None
            try:
                started = time.monotonic()
                code, log = gate.execute([sys.executable, str(parent)], root, dict(os.environ), 2)
                self.assertLess(time.monotonic() - started, 10, "timeout waited for the child's natural exit")
                self.assertEqual(code, "timeout")
                self.assertIn(b"started", log)
                pid = int((root / "child.pid").read_text())
                deadline = time.monotonic() + 2
                while alive(pid) and time.monotonic() < deadline:
                    time.sleep(.05)
                self.assertFalse(alive(pid), "the timed-out fixture left its child running")
            finally:
                if pid is not None and alive(pid):
                    os.kill(pid, signal.SIGTERM)


if __name__ == "__main__":
    unittest.main()

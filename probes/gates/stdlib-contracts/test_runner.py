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

"""Exercise process startup, resource enforcement and descendant cleanup."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import posix_process


class ProcessExecutorTests(unittest.TestCase):
    def test_policies_do_not_claim_the_same_memory_bound(self):
        self.assertEqual(posix_process.memory_policy("darwin"), "Darwin process-group sampled RSS")
        self.assertEqual(posix_process.memory_policy("linux"), "per-process RLIMIT_AS")

    def test_rss_counts_only_our_group_and_rejects_bad_measurements(self):
        self.assertEqual(posix_process.parse_group_rss(" 41 1024\n 99 4096\n 41 512\n", 41), 1536 * 1024)
        self.assertEqual(posix_process.parse_group_rss("99 -\n41 512\n", 41), 512 * 1024)
        for text in ("41 -1", "not a-number", "41", "41 100 200"):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                posix_process.parse_group_rss(text, 41)

    def run_child(self, source, platform, memory_mb=256, timeout=5):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with (directory / "stdout").open("wb") as out, (directory / "stderr").open("wb") as err:
                result = posix_process.execute([sys.executable, "-c", source], cwd=directory,
                    env=dict(os.environ), stdout=out, stderr=err, timeout=timeout,
                    memory_mb=memory_mb, platform=platform)
            return result, (directory / "stdout").read_text(), (directory / "stderr").read_text()

    def assert_stopped(self, pid):
        # A killed descendant may remain a zombie until its parent/init reaps it.
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            result = subprocess.run(["/bin/ps", "-o", "stat=", "-p", str(pid)],
                                    capture_output=True, text=True, timeout=2)
            if not result.stdout.strip() or result.stdout.strip().startswith("Z"):
                return
            time.sleep(0.02)
        self.fail(f"owned descendant {pid} survived process group cleanup")

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_darwin_startup_does_not_use_a_preexec_hook(self):
        real_popen = subprocess.Popen
        commands = []

        def launch(command, **kwargs):
            commands.append((command, kwargs))
            return real_popen(command, **kwargs)

        with patch.object(posix_process.subprocess, "Popen", side_effect=launch):
            result, out, err = self.run_child("print('started')", "darwin")
        self.assertEqual((result["ExitCode"], out, err), (0, "started\n", ""))
        self.assertIsNone(commands[0][1]["preexec_fn"])
        self.assertFalse(result["TimedOut"] or result["MemoryExceeded"])
        self.assertEqual(result["MemoryPolicy"], "Darwin process-group sampled RSS")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux RLIMIT_AS")
    def test_linux_child_receives_the_address_space_bound(self):
        result, out, err = self.run_child(
            "import resource; print(resource.getrlimit(resource.RLIMIT_AS)[0])", "linux")
        self.assertEqual(result["ExitCode"], 0)
        self.assertEqual(out.strip(), str(256 * 1024 * 1024))
        self.assertEqual(err, "")

    @unittest.skipUnless(os.name == "posix", "POSIX executor integration")
    def test_suite_version_request_uses_the_darwin_policy(self):
        from suite import Executor, compiler_identity
        with tempfile.TemporaryDirectory() as temporary, patch.object(posix_process.sys, "platform", "darwin"):
            executor = Executor(sys.executable, 5, 256)
            try:
                result = executor.execute(sys.executable, ["--version"], temporary, "version", "default")
                self.assertEqual(result["result"]["ExitCode"], 0)
                self.assertEqual(result["result"]["MemoryPolicy"], "Darwin process-group sampled RSS")
                self.assertEqual(compiler_identity(executor)["memory_policy"], result["result"]["MemoryPolicy"])
                self.assertIn("Python", (Path(temporary) / "version.stdout.log").read_text())
            finally:
                executor.close()

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_rss_limit_includes_children_and_kills_the_group(self):
        child = ("import pathlib,time\n"
                 "while not pathlib.Path('ready').exists(): time.sleep(0.01)\n"
                 "data=bytearray(64*1024*1024)\n"
                 "data[::4096]=b'x'*(len(data)//4096)\ntime.sleep(30)\n")
        source = ("import pathlib,subprocess,sys,time\n"
                  f"children=[subprocess.Popen([sys.executable,'-c',{child!r}]) for _ in range(2)]\n"
                  "print(*(p.pid for p in children),flush=True)\n"
                  "pathlib.Path('ready').touch()\ntime.sleep(30)\n")
        result, out, _ = self.run_child(source, "darwin", memory_mb=112)
        self.assertTrue(result["MemoryExceeded"])
        self.assertEqual(result["ExitCode"], -902)
        self.assertFalse(result["TimedOut"])
        self.assertGreater(result["SampledPeakGroupRssMB"], 112)
        pids = list(map(int, out.split()))
        self.assertEqual(len(pids), 2)
        for pid in pids:
            self.assert_stopped(pid)

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_timeout_kills_descendants(self):
        source = ("import subprocess,sys,time\n"
                  "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'])\n"
                  "print(child.pid,flush=True)\ntime.sleep(30)\n")
        result, out, _ = self.run_child(source, "darwin", timeout=1)
        self.assertTrue(result["TimedOut"])
        self.assertFalse(result["MemoryExceeded"])
        self.assertEqual(result["ExitCode"], -901)
        self.assert_stopped(int(out.strip()))

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_exited_root_cannot_leave_a_child_running(self):
        source = ("import subprocess,sys\n"
                  "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'])\n"
                  "print(p.pid,flush=True)\n")
        result, out, _ = self.run_child(source, "darwin")
        self.assertEqual(result["ExitCode"], 0)
        self.assert_stopped(int(out.strip()))

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_monitor_failure_fails_closed_and_reaps_the_root(self):
        children = []
        real_popen = subprocess.Popen

        def launch(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            children.append(proc)
            return proc

        with patch.object(posix_process.subprocess, "Popen", side_effect=launch), \
             patch.object(posix_process, "sample_group_rss", side_effect=RuntimeError("monitor unavailable")):
            with self.assertRaisesRegex(RuntimeError, "monitor unavailable"):
                self.run_child("import time; time.sleep(30)", "darwin")
        self.assertTrue(children)
        self.assertIsNotNone(children[0].poll())


if __name__ == "__main__":
    unittest.main()

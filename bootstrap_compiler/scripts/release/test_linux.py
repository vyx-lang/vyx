"""Dependency preparation must stop before using failed or invalid downloads."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import prepare_linux_llvm as linux


class LinuxPreparationTests(unittest.TestCase):
    def invoke(self, fail_at=None):
        commands = []
        installed = {}

        def command(argv, *, check):
            self.assertTrue(check)
            commands.append(argv)
            if argv[0] == "curl":
                # A failed transfer may leave bytes on disk; they must not be
                # imported just because the destination file exists.
                Path(argv[argv.index("--output") + 1]).write_bytes(b"downloaded key")
            if fail_at and fail_at(argv):
                raise subprocess.CalledProcessError(6 if argv[0] == "curl" else 1, argv)
            if argv[0] == "gpg":
                Path(argv[argv.index("--output") + 1]).write_bytes(b"validated keyring")
            if argv[:2] == ["sudo", "install"]:
                installed[argv[-1]] = Path(argv[-2]).read_bytes()
            return subprocess.CompletedProcess(argv, 0)

        error = None
        with patch.object(linux.subprocess, "run", side_effect=command), redirect_stdout(io.StringIO()):
            try:
                linux.prepare()
            except subprocess.CalledProcessError as caught:
                error = caught
        return commands, installed, error

    def test_dns_failure_does_not_import_partial_key_or_change_system(self):
        commands, installed, error = self.invoke(lambda argv: argv[0] == "curl")
        self.assertEqual(error.returncode, 6)
        self.assertEqual([argv[0] for argv in commands], ["curl"])
        self.assertEqual(installed, {})
        self.assertFalse(Path(commands[0][commands[0].index("--output") + 1]).exists())

    def test_invalid_key_does_not_change_system(self):
        commands, installed, error = self.invoke(lambda argv: argv[0] == "gpg")
        self.assertIsNotNone(error)
        self.assertEqual([argv[0] for argv in commands], ["curl", "gpg"])
        self.assertEqual(installed, {})

    def test_failed_update_does_not_install_packages(self):
        commands, installed, error = self.invoke(lambda argv: argv[-1] == "update")
        self.assertIsNotNone(error)
        self.assertEqual(commands[-1][-1], "update")
        self.assertFalse(any(argv[:2] == ["sudo", "apt-get"] and "install" in argv for argv in commands))

    def test_success_installs_only_validated_key_and_signed_source(self):
        commands, installed, error = self.invoke()
        self.assertIsNone(error)
        self.assertEqual(installed[linux.KEYRING], b"validated keyring")
        source = installed[linux.SOURCE].decode()
        self.assertIn(f"signed-by={linux.KEYRING}", source)
        self.assertIn("llvm-toolchain-noble-22 main", source)
        self.assertTrue(set(linux.PACKAGES).issubset(commands[-1]))
        # Lock in the failure mode this fix addresses: DNS must be retried and
        # APT must not accept a partial index update as success.
        self.assertIn("--retry-all-errors", commands[0])
        self.assertIn("--retry-max-time", commands[0])
        self.assertIn("--max-time", commands[0])
        self.assertIn("--error-on=any", commands[-2])


if __name__ == "__main__":
    unittest.main()

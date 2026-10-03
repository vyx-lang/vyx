"""Regression tests for the industrial DCI pressure runner scheduler."""

from __future__ import annotations

import importlib.util
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
RUNNER_PATH = ROOT / "probes" / "gates" / "dci-industrial" / "run.py"


def _load_runner():
    spec = importlib.util.spec_from_file_location("dci_industrial_runner", RUNNER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {RUNNER_PATH}")
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves postponed annotations through sys.modules.
    import sys

    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class IndustrialRunnerTests(unittest.TestCase):
    def test_repeated_case_runs_are_serialized(self) -> None:
        runner = _load_runner()
        runner._CASE_LOCKS.clear()
        active = 0
        maximum = 0
        state = threading.Lock()

        def fake_run(case, iteration, timeout, out_dir):
            nonlocal active, maximum
            with state:
                active += 1
                maximum = max(maximum, active)
            time.sleep(0.04)
            with state:
                active -= 1
            return (case.name, iteration)

        case = runner.Case("same-fixture", ("python", "fixture"))
        with mock.patch.object(runner, "_run_one_unlocked", side_effect=fake_run):
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(runner._run_one, case, n, 10, Path("out")) for n in (1, 2)]
                self.assertEqual([future.result() for future in futures], [("same-fixture", 1), ("same-fixture", 2)])
        self.assertEqual(maximum, 1)

    def test_shared_toolchain_cases_are_serialized(self) -> None:
        runner = _load_runner()
        runner._CASE_LOCKS.clear()
        active = 0
        maximum = 0
        state = threading.Lock()

        def fake_run(case, iteration, timeout, out_dir):
            nonlocal active, maximum
            with state:
                active += 1
                maximum = max(maximum, active)
            time.sleep(0.04)
            with state:
                active -= 1
            return (case.name, iteration)

        zig_a = runner.Case("zig-a", ("python", "a"), resource_key="zig-toolchain")
        zig_b = runner.Case("zig-b", ("python", "b"), resource_key="zig-toolchain")
        with mock.patch.object(runner, "_run_one_unlocked", side_effect=fake_run):
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [
                    pool.submit(runner._run_one, zig_a, 1, 10, Path("out")),
                    pool.submit(runner._run_one, zig_b, 1, 10, Path("out")),
                ]
                [future.result() for future in futures]
        self.assertEqual(maximum, 1)


if __name__ == "__main__":
    unittest.main()

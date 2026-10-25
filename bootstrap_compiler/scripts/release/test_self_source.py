"""Source rechecks use committed build records, independently of telemetry."""
import importlib.util
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from unittest.mock import patch


GATE = Path(__file__).resolve().parents[3] / "probes/gates/compiler-self-source/run.py"
spec = importlib.util.spec_from_file_location("compiler_self_source", GATE)
self_source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(self_source)


class SelfSourcePlans(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="macOS build ")
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        (self.build / "src").mkdir()
        self.cache = self.build / ".cache"
        self.cache.mkdir()
        (self.build / "Vyx.toml").write_text(
            '[build]\ncache_dir = ".cache"\n[target.boot]\n'
            'entry = "src/main.vyx"\nsources = ["src/peer.vyx"]\n', encoding="utf-8")
        (self.build / "src/main.vyx").write_text(
            "fn main() -> i32 { return peer(); }\n", encoding="utf-8")
        (self.build / "src/peer.vyx").write_text(
            "fn peer() -> i32 { return 0; }\n", encoding="utf-8")
        self.unit = self.cache / "unit_boot.sources"
        self.unit.write_text("src/main.vyx\nsrc/peer.vyx\n", encoding="utf-8")
        self.artifact = self.cache / "module_bootstrap.o"
        self.artifact.write_bytes(b"completed object")
        self.database = self.cache / ".vyx_cache"

    def row(self, *, unit_flag="--unit-sources", artifact=".cache/module_bootstrap.o"):
        # Same quoting and record format as the macOS Stage 0 builder. Its
        # executable does not need to run when reading the completed plan.
        command = ('"/Users/runner/work/_temp/vyx-macos/native-stage0/vyxc" '
                   '--emit=obj --src=file "src/main.vyx" '
                   f'-o "{artifact}.tmp" -L "out" -l vyx_runtime -O2 '
                   '--interface-root "out/.vyx_interfaces/boot" '
                   f'{unit_flag} "{self.unit.as_posix()}" '
                   '--export-top-level-roots --export-root-names "main|peer"')
        return f"src/main.vyx\t{artifact}\tsrc=bytes:42;\t{command}\n"

    def test_macos_completed_plan_without_cost_history(self):
        self.database.write_text(self.row(), encoding="utf-8")
        self.assertFalse(list(self.cache.glob("*.vyx-cost")))
        plans, count = self_source.build_plan(self.build, "boot")
        self.assertEqual(count, 2)
        self.assertEqual(len(plans), 1)
        self.assertEqual(plans[0]["declared_function_bodies"], 2)
        self.assertEqual(len(plans[0]["sources"]), 2)
        self.assertEqual(plans[0]["arguments"][-1], "main|peer")
        self.assertEqual(plans[0]["record"], str(self.database))
        self.assertEqual(plans[0]["record_line"], 1)

    def test_warm_build_does_not_need_scheduled_tasks(self):
        self.database.write_text(self.row(), encoding="utf-8")
        (self.cache / "tasks_boot.tmp").write_text("", encoding="utf-8")
        self.assertEqual(len(self_source.build_plan(self.build, "boot")[0]), 1)

    def test_posix_command_parser_preserves_quoted_lists_and_paths(self):
        self.database.write_text(self.row(), encoding="utf-8")
        with patch.object(self_source, "split_command", side_effect=shlex.split):
            plans, count = self_source.build_plan(self.build, "boot")
        self.assertEqual(count, 2)
        self.assertEqual(plans[0]["arguments"][-1], "main|peer")
        self.assertIn(self.unit.as_posix(), plans[0]["arguments"])

    def test_all_recorded_unit_source_modes_account_for_peers(self):
        for flag in ("--unit-sources", "--project-unit-sources", "--interface-unit-sources"):
            with self.subTest(flag=flag):
                self.database.write_text(self.row(unit_flag=flag), encoding="utf-8")
                self.assertEqual(len(self_source.build_plan(self.build, "boot")[0][0]["sources"]), 2)

    def test_cost_history_cannot_substitute_for_completed_cache(self):
        (self.cache / "module_bootstrap.o.vyx-cost").write_text(self.row(), encoding="utf-8")
        with self.assertRaises(FileNotFoundError):
            self_source.build_plan(self.build, "boot")

    def test_scheduled_task_cannot_substitute_for_completed_cache(self):
        (self.cache / "tasks_boot.tmp").write_text(self.row(), encoding="utf-8")
        with self.assertRaises(FileNotFoundError):
            self_source.build_plan(self.build, "boot")

    def test_missing_object_cannot_count_as_a_completed_plan(self):
        self.database.write_text(self.row(), encoding="utf-8")
        self.artifact.unlink()
        with self.assertRaisesRegex(ValueError, "No completed object plan"):
            self_source.build_plan(self.build, "boot")

    def test_uncovered_manifest_source_is_rejected(self):
        self.database.write_text(self.row(), encoding="utf-8")
        self.unit.write_text("src/main.vyx\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "peer.vyx"):
            self_source.build_plan(self.build, "boot")

    def test_record_source_must_match_the_command(self):
        self.database.write_text(self.row().replace("src/main.vyx\t", "src/peer.vyx\t", 1),
                                 encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Source/command mismatch"):
            self_source.build_plan(self.build, "boot")

    def test_malformed_cache_is_rejected(self):
        self.database.write_text("src/main.vyx\tmissing command\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Malformed build cache record"):
            self_source.build_plan(self.build, "boot")

    def test_link_and_interface_record_schemas_are_not_object_plans(self):
        self.database.write_text(
            '__link:boot\tout/boot\t123\t"clang++" -o out/boot\tlibraries\tstrip-exe\n'
            '__vyi:src/main.vyx\tout/boot.vyi\tstamp\t"vyxc" --emit=vyi\n'
            + self.row(), encoding="utf-8")
        self.assertEqual(len(self_source.build_plan(self.build, "boot")[0]), 1)

    def test_newest_completed_artifact_selects_the_current_plan(self):
        other = self.cache / "module_bootstrap.obj"
        other.write_bytes(b"old object format")
        os.utime(other, ns=(1_000_000_000, 1_000_000_000))
        self.database.write_text(self.row(artifact=".cache/module_bootstrap.obj") + self.row(),
                                 encoding="utf-8")
        plans, _ = self_source.build_plan(self.build, "boot")
        self.assertEqual(plans[0]["artifact"], str(self.artifact))

    def interface_row(self, *, root="boot"):
        artifact = self.build / f"out/.vyx_interfaces/{root}/bootstrap.vyi"
        artifact.parent.mkdir(parents=True, exist_ok=True)
        artifact.write_bytes(b"Stage 0 interface")
        command = ('"vyxc" --emit=vyi --vyi-shallow --src=file "src/main.vyx" '
                   f'--unit-sources "{self.unit.as_posix()}" -o "{artifact.as_posix()}.tmp"')
        return f"__vyi:src/main.vyx\t{artifact.as_posix()}\tstamp\t{command}\n"

    def test_fresh_interface_plan_keeps_module_inputs_and_selects_the_target(self):
        self.database.write_text(self.row() + self.interface_row() + self.interface_row(root="lsp"),
                                 encoding="utf-8")
        plans, _ = self_source.build_plan(self.build, "boot")
        root, interfaces = self_source.interface_plan(self.build, plans)
        self.assertEqual(root, (self.build / "out/.vyx_interfaces/boot").resolve())
        self.assertEqual(len(interfaces), 1)
        self.assertEqual(interfaces[0]["relative_output"], "bootstrap.vyi")
        self.assertIn(self.unit.as_posix(), interfaces[0]["arguments"])

    def test_missing_interface_record_is_rejected(self):
        self.database.write_text(self.row(), encoding="utf-8")
        plans, _ = self_source.build_plan(self.build, "boot")
        with self.assertRaisesRegex(ValueError, "No completed interface plan"):
            self_source.interface_plan(self.build, plans)

    def test_missing_published_interface_is_rejected(self):
        self.database.write_text(self.row() + self.interface_row(), encoding="utf-8")
        (self.build / "out/.vyx_interfaces/boot/bootstrap.vyi").unlink()
        plans, _ = self_source.build_plan(self.build, "boot")
        with self.assertRaisesRegex(ValueError, "Missing published compiler interface"):
            self_source.interface_plan(self.build, plans)

    def test_identical_interface_commands_from_different_compilers_are_deduplicated(self):
        row = self.interface_row()
        self.database.write_text(self.row() + row + row.replace('"vyxc"', '"Stage 0/vyxc"'),
                                 encoding="utf-8")
        plans, _ = self_source.build_plan(self.build, "boot")
        self.assertEqual(len(self_source.interface_plan(self.build, plans)[1]), 1)

    def test_conflicting_interface_commands_are_rejected(self):
        row = self.interface_row()
        self.database.write_text(self.row() + row + row.replace('--vyi-shallow', '--vyi-shallow --vyi-project-private'),
                                 encoding="utf-8")
        plans, _ = self_source.build_plan(self.build, "boot")
        with self.assertRaisesRegex(ValueError, "Conflicting compiler interface plans"):
            self_source.interface_plan(self.build, plans)


if __name__ == "__main__":
    unittest.main()

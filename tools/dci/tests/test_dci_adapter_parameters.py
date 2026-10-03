"""`dci adapter` has one parameter set, and every adapter hook gets one object.

These tests pin the unification itself, not the individual language adapters:
a canonical spelling and a legacy alias must reach the same parameter, a
spelling owned by another language must be rejected without any hand-written
rule, and a third-party adapter must gain all of that just by declaring it.
"""

from __future__ import annotations

import contextlib
import io
import inspect
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS_DIR = Path(__file__).parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import dci as dci_cli  # noqa: E402
import dci_plugin  # noqa: E402
import sdk_adapters  # noqa: E402  registers the built-in adapters
import sdk_parameters  # noqa: E402

PROBE_LANGUAGE = "probe"
PROBE_SOURCE = "pub fn probe() {}\n"

_RUST_MODULE = {"rust": "dci_adapter_rust", "cpp": "dci_adapter_cpp", "zig": "dci_adapter_zig"}


def _options() -> dict[str, dci_plugin.AdapterOption]:
    return {option.dest: option for option in dci_plugin.adapter_options()}


def _run_adapter(argv: list[str], module):
    with mock.patch.object(module, "main", return_value=0) as run:
        rc = dci_cli.main(argv)
    return rc, (run.call_args.args[0] if run.called else None)


def _module_for(language: str):
    return getattr(dci_cli, _RUST_MODULE[language])


class UnifiedParameterTests(unittest.TestCase):
    def test_table_merges_by_dest_and_records_ownership(self) -> None:
        options = _options()
        compiler = options["compiler"]
        self.assertEqual(
            compiler.flags, ("--compiler", "--cxx", "--rustc", "--zig")
        )
        self.assertEqual(compiler.languages, ("cpp", "rust", "zig"))
        for language in ("cpp", "rust", "zig"):
            self.assertTrue(compiler.accepted_by(language), language)

        extractor = options["extractor"]
        self.assertEqual(extractor.flags, ("--extractor", "--clang"))
        self.assertEqual(extractor.languages, ("cpp",))
        self.assertFalse(extractor.accepted_by("rust"))

        namespace = options["namespace"]
        self.assertEqual(namespace.flags, ("--namespace", "--crate-name"))
        self.assertEqual(namespace.languages, ("rust", "zig"))

        # language-neutral parameters belong to no language in particular
        self.assertTrue(options["output"].universal())
        self.assertTrue(options["triplet"].universal())

    def test_every_declared_parameter_is_reachable_from_the_parser(self) -> None:
        args = dci_cli.build_parser().parse_args(["adapter", "api.hpp"])
        for dest in _options():
            with self.subTest(dest=dest):
                self.assertTrue(hasattr(args, dest))

    def test_unified_and_legacy_spellings_produce_the_same_argv(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "lib.rs"
            source.write_text(PROBE_SOURCE, encoding="utf-8")
            output = Path(td) / "contract.dcib"
            legacy = [
                "adapter", "--language", "rust", str(source),
                "--rustc", "custom-rustc", "--crate-name", "native",
                "--rustc-arg=-Copt-level=3", "-o", str(output),
            ]
            unified = [
                "adapter", "--language", "rust", str(source),
                "--compiler", "custom-rustc", "--namespace", "native",
                "--compiler-arg=-Copt-level=3", "-o", str(output),
            ]
            legacy_rc, legacy_argv = _run_adapter(legacy, dci_cli.dci_adapter_rust)
            unified_rc, unified_argv = _run_adapter(unified, dci_cli.dci_adapter_rust)
        self.assertEqual((legacy_rc, unified_rc), (0, 0))
        self.assertIsNotNone(legacy_argv)
        self.assertIn("--rustc", legacy_argv)
        self.assertNotIn("--compiler", legacy_argv)
        self.assertIn("--rustc-arg=-Copt-level=3", unified_argv)
        self.assertEqual(legacy_argv, unified_argv)

    def test_option_owned_by_another_language_is_rejected(self) -> None:
        cases = (
            ("rust", ["--extractor", "clang++"], "--extractor", "cpp"),
            ("rust", ["--toolchain", "gcc"], "--toolchain", "cpp"),
            ("cpp", ["--edition", "2021"], "--edition", "rust"),
            ("cpp", ["--crate-name", "native"], "--crate-name", "rust"),
            ("zig", ["--rustc-arg=-O2"], "--rustc-arg", "rust"),
            ("cpp", ["--deny-rejected"], "--deny-rejected", "rust"),
        )
        with tempfile.TemporaryDirectory() as td:
            sources = {
                "rust": (Path(td) / "lib.rs", PROBE_SOURCE),
                "cpp": (Path(td) / "api.hpp", "extern int api();\n"),
                "zig": (Path(td) / "lib.zig", "export fn a() i32 { return 1; }\n"),
            }
            for path, text in sources.values():
                path.write_text(text, encoding="utf-8")
            for language, extra, expected_flag, owner in cases:
                with self.subTest(language=language, option=expected_flag):
                    error = io.StringIO()
                    with contextlib.redirect_stderr(error):
                        rc = dci_cli.main([
                            "adapter", "--language", language,
                            str(sources[language][0]), *extra,
                            "-o", str(Path(td) / "out.dcib"),
                        ])
                    self.assertEqual(rc, 2)
                    message = error.getvalue()
                    self.assertIn(expected_flag, message)
                    self.assertIn("belong to another language adapter", message)
                    self.assertIn(f"--language {owner}", message)

    def test_shared_spelling_is_accepted_by_every_language(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "api.hpp"
            source.write_text("extern int api();\n", encoding="utf-8")
            for language, compiler in (("cpp", "g++"), ("rust", "rustc"), ("zig", "zig")):
                with self.subTest(language=language):
                    rc, argv = _run_adapter(
                        ["adapter", "--language", language, str(source),
                         "--compiler", compiler, "-o", str(Path(td) / "out.dcib")],
                        _module_for(language),
                    )
                    self.assertEqual(rc, 0)
                    self.assertIn(compiler, argv)

    def test_input_rule_is_declared_not_hard_coded(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            first = Path(td) / "lib.rs"
            second = Path(td) / "other.rs"
            for path in (first, second):
                path.write_text(PROBE_SOURCE, encoding="utf-8")
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                rc = dci_cli.main([
                    "adapter", "--language", "rust", str(first), str(second),
                    "-o", str(Path(td) / "out.dcib"),
                ])
        self.assertEqual(rc, 2)
        self.assertIn(
            "the rust Adapter requires exactly one crate-root .rs input",
            error.getvalue(),
        )

    def test_dash_valued_options_derive_from_append_parameters(self) -> None:
        spells = dci_plugin.dash_valued_options()
        self.assertIn("--compile_flags", spells)
        self.assertIn("--compiler-arg", spells)
        self.assertIn("--rustc-arg", spells)
        # a bare `--debug-json` must never swallow the next token
        self.assertNotIn("--debug-json", spells)


class HookSignatureTests(unittest.TestCase):
    def test_every_hook_takes_one_request(self) -> None:
        for language, plugin in dci_plugin.ADAPTERS.items():
            for name in ("build_argv", "run", "doctor"):
                hook = getattr(plugin, name)
                if hook is None:
                    continue
                with self.subTest(language=language, hook=name):
                    parameters = list(inspect.signature(hook).parameters.values())
                    self.assertEqual(len(parameters), 1)
                    self.assertEqual(parameters[0].name, "request")

    def test_adapter_request_carries_the_unified_view(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "lib.rs"
            source.write_text(PROBE_SOURCE, encoding="utf-8")
            args = dci_cli.build_parser().parse_args([
                "adapter", "--language", "rust", str(source),
                "--compiler", "custc", "--compiler-arg=-Cx", "--compiler-arg=-Cy",
                "--namespace", "native", "--debug-json", "--artifact", "libnative.a",
                "-o", str(Path(td) / "out.dcib"),
            ])
            request = dci_plugin.adapter_request(
                args, language="rust",
                sources=dci_plugin.resolve_sources(args.headers, args.include),
                target=sdk_adapters.canonical_target(args.triplet),
            )
            argv = dci_plugin.ADAPTERS["rust"].build_argv(request)
        self.assertIsInstance(request, dci_plugin.AdapterRequest)
        self.assertEqual(request.compiler, "custc")
        self.assertEqual(request.compiler_args, ("-Cx", "-Cy"))
        self.assertEqual(request.namespace, "native")
        self.assertEqual(request.artifacts, ("libnative.a",))
        # a bare --debug-json means "beside the output"
        self.assertEqual(request.debug_json.name, "out.dci.json")
        self.assertEqual(argv[argv.index("--rustc") + 1], "custc")


class ThirdPartyAdapterTests(unittest.TestCase):
    """A new language gets parsing, help and diagnostics from declarations."""

    def setUp(self) -> None:
        self.seen: dict[str, object] = {}

        def build_argv(request):
            self.seen["build"] = request
            return [str(request.sources[0]), "--output", str(request.output)]

        def run(request):
            self.seen["run"] = request
            return 0

        def doctor(request):
            self.seen["doctor"] = request
            return 0

        self.plugin = dci_plugin.AdapterPlugin(
            language=PROBE_LANGUAGE,
            parameters=(
                sdk_parameters.param("compiler", "--probecc"),
                sdk_parameters.OptionSpec(
                    dest="emit_ir", flags=("--emit-ir",), kind="flag",
                    help="emit IR alongside the contract",
                ),
            ),
            build_argv=build_argv,
            run=run,
            doctor=doctor,
            max_sources=1,
            input_noun="crate-root .probe",
        )
        dci_plugin.register_adapter(self.plugin)

    def tearDown(self) -> None:
        dci_plugin.unregister_adapter(PROBE_LANGUAGE)

    def test_declared_parameters_compose_the_cli(self) -> None:
        compiler = _options()["compiler"]
        self.assertIn("--probecc", compiler.flags)
        self.assertTrue(compiler.accepted_by(PROBE_LANGUAGE))
        args = dci_cli.build_parser().parse_args(
            ["adapter", "--language", PROBE_LANGUAGE, "api.probe", "--emit-ir"]
        )
        self.assertEqual(args.language, PROBE_LANGUAGE)
        self.assertTrue(args.emit_ir)

    def test_dispatch_hands_one_request_to_every_hook(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "api.probe"
            source.write_text(PROBE_SOURCE, encoding="utf-8")
            rc = dci_cli.main([
                "adapter", "--language", PROBE_LANGUAGE, str(source),
                "--probecc", "probefront", "--emit-ir",
                "-o", str(Path(td) / "api.dcib"),
            ])
        self.assertEqual(rc, 0)
        built = self.seen["build"]
        ran = self.seen["run"]
        self.assertIsInstance(built, dci_plugin.AdapterRequest)
        self.assertIsInstance(ran, dci_plugin.AdapterRequest)
        self.assertEqual(built.language, PROBE_LANGUAGE)
        self.assertEqual(ran.language, PROBE_LANGUAGE)
        self.assertIs(ran.options, built.options)
        self.assertEqual(built.compiler, "probefront")  # canonical name
        self.assertTrue(built.has("emit_ir"))
        self.assertEqual(built.argv, ())
        self.assertEqual(
            ran.argv, (str(source), "--output", str(Path(td) / "api.dcib"))
        )

    def test_declared_ownership_rejects_cross_language_use(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "lib.rs"
            source.write_text(PROBE_SOURCE, encoding="utf-8")
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                rc = dci_cli.main([
                    "adapter", "--language", "rust", str(source),
                    "--probecc", "probefront", "-o", str(Path(td) / "out.dcib"),
                ])
        self.assertEqual(rc, 2)
        self.assertIn("--probecc", error.getvalue())
        self.assertIn(f"--language {PROBE_LANGUAGE}", error.getvalue())

    def test_declared_input_rule_is_enforced(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            first = Path(td) / "a.probe"
            second = Path(td) / "b.probe"
            for path in (first, second):
                path.write_text(PROBE_SOURCE, encoding="utf-8")
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                rc = dci_cli.main([
                    "adapter", "--language", PROBE_LANGUAGE, str(first), str(second),
                    "-o", str(Path(td) / "out.dcib"),
                ])
        self.assertEqual(rc, 2)
        self.assertIn(
            "the probe Adapter requires exactly one crate-root .probe input",
            error.getvalue(),
        )

    def test_declared_doctor_hook_receives_the_request(self) -> None:
        """EXTENDING.md documents three hooks; the third one is reachable too.

        `dci doctor` must pick the third-party language up from the registry
        (its ``--language`` choices are derived, not hard-coded) and hand the
        hook the same single `AdapterRequest`.
        """
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            rc = dci_cli.main(["doctor", "--language", PROBE_LANGUAGE])
        self.assertEqual(rc, 0, output.getvalue())
        request = self.seen.get("doctor")
        self.assertIsInstance(request, dci_plugin.AdapterRequest)
        self.assertEqual(request.language, PROBE_LANGUAGE)
        # --triplet's default is normalised on the way into the request
        self.assertEqual(
            request.target, sdk_adapters.canonical_target("x64_windows")
        )


if __name__ == "__main__":
    unittest.main()

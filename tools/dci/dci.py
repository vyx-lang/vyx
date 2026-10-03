#!/usr/bin/env python3
"""Unified command line for the Vyx DCI SDK.

``dci.py`` is now a thin shell over the SDK plugin registry
(:mod:`dci_plugin`): built-in language adapters live in :mod:`sdk_adapters`,
built-in commands in :mod:`sdk_commands`, and third parties add new
languages / commands / stub backends by dropping a module into
``plugins/`` or shipping an entry point (see ``EXTENDING.md``).

The `dci adapter` parameter set is unified: each parameter has one canonical
spelling plus per-language aliases, all declared by the adapters themselves
(:mod:`sdk_parameters`), so the parser, the help text and the cross-language
diagnostics have a single source.

Historical, intentionally preserved interfaces:

* this exact path is discovered by ``vyxc dci`` and ``VYX_DCI_TOOLS``;
* the CLI surface (``adapter`` / ``validate`` / ``inspect`` / ``encode`` /
  ``decode`` / ``doctor``, incl. aliases and every historical option
  spelling) is unchanged;
* ``python tools/dci/dci_adapter_msvc.py`` and the other flat modules remain
  independently callable — gates invoke them directly.

    python tools/dci/dci.py doctor
    python tools/dci/dci.py adapter include/api.hpp -o contracts/api.dcib
    python tools/dci/dci.py adapter --language rust lib.rs --compiler rustc \\
        --namespace native_api -o contracts/native.dcib
    python tools/dci/dci.py validate --strict contracts/api.dcib
    python tools/dci/dci.py inspect contracts/api.dcib
    python tools/dci/dci.py plugins          # what is registered right now
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

TOOL_DIR = Path(__file__).resolve().parent

if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

try:
    import dci_plugin
    from dci_plugin import (
        ADAPTERS, COMMANDS, DciCliError, adapter_input_error,
        adapter_option_conflicts, adapter_request, dash_valued_options,
        resolve_sources,
    )
except ImportError:  # installed as a package
    from tools.dci.dci_plugin import (  # type: ignore
        ADAPTERS, COMMANDS, DciCliError, adapter_input_error,
        adapter_option_conflicts, adapter_request, dash_valued_options,
        resolve_sources,
    )
    import tools.dci.dci_plugin as dci_plugin  # type: ignore

try:
    import sdk_adapters
    import sdk_commands
    import stub_backend  # noqa: F401  registers built-in stub backends
except ImportError:  # installed as a package
    from tools.dci import sdk_adapters, sdk_commands, stub_backend  # type: ignore

# Load third-party plugins (plugins/ dir + installed entry points) before the
# parser is composed, so a plugin command shows up in `dci -h`.
dci_plugin.load_external_plugins()

# Backwards-compatible module attributes: tests and integrations patch
# `dci_adapter_rust.main` / `dci_adapter_zig.main` / `dci_adapter_cpp.main`
# on this namespace's module objects.
try:
    import dci_adapter_cpp  # noqa: F401
    import dci_adapter_rust  # noqa: F401
    import dci_adapter_zig  # noqa: F401
    import cpp_toolchains  # noqa: F401
    import dci_validate  # noqa: F401
    import dcib  # noqa: F401
except ImportError:  # installed as a package
    from tools.dci import (  # type: ignore
        cpp_toolchains, dcib, dci_adapter_cpp, dci_adapter_rust,
        dci_validate, dci_adapter_zig,
    )

# Backwards-compatible re-exports (tests use canonical_target, load_contract,
# _contract_summary; historical callers may use the command_* functions).
canonical_target = sdk_adapters.canonical_target
load_contract = sdk_commands.load_contract
write_json = sdk_commands.write_json
_contract_summary = sdk_commands._contract_summary
DEFAULT_SCHEMA = sdk_commands.DEFAULT_SCHEMA
command_validate = sdk_commands.command_validate
command_inspect = sdk_commands.command_inspect
command_encode = sdk_commands.command_encode
command_decode = sdk_commands.command_decode
command_doctor = sdk_commands.command_doctor
command_plugins = sdk_commands.command_plugins


def command_adapter(args: argparse.Namespace) -> int:
    """Dispatch `dci adapter` through the language-adapter registry.

    The only per-language knowledge here is the registry lookup: which
    parameters a language accepts, which of them belong to another language,
    and how many sources it takes are all declared by the plugin.
    """
    language = args.language
    plugin = ADAPTERS.get(language)
    if plugin is None:
        raise DciCliError(
            f"no adapter registered for --language {language!r}; "
            f"registered: {', '.join(sorted(ADAPTERS))}"
        )
    sources = resolve_sources(args.headers, args.include)
    rejection = (
        adapter_input_error(language, sources)
        or adapter_option_conflicts(language, getattr(args, "raw_argv", ()))
    )
    if rejection:
        raise DciCliError(rejection)

    request = adapter_request(
        args,
        language=language,
        sources=sources,
        target=canonical_target(args.triplet),
        passthrough=getattr(args, "adapter_extra", ()),
    )
    if plugin.validate is not None:
        rejection = plugin.validate(request)
        if rejection:
            raise DciCliError(rejection)
    argv = plugin.build_argv(request)
    return plugin.run(request.with_argv(argv))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dci",
        description="Generate, validate and inspect Vyx DCI contracts.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "quick start (5 commands):\n"
            "  dci doctor                 verify compiler/extractor/schema discovery\n"
            "  dci adapter include/api.hpp -o contracts/api.dcib\n"
            "  dci validate --strict contracts/api.dcib\n"
            "  dci inspect contracts/api.dcib\n"
            "  dci decode contracts/api.dcib api.dci.json\n"
            "\n"
            "Rust/Zig producers: add --language rust|zig with the crate/root source.\n"
            "One parameter, one canonical spelling: --compiler / --compiler-arg /\n"
            "--extractor / --extractor-arg / --namespace, with the per-language\n"
            "aliases (--cxx|--rustc|--zig, --clang, --crate-name, ...) accepted too.\n"
            "Run `dci <command> -h` for per-command options. `vyxc dci ...`\n"
            "discovers this tool beside an SDK or from VYX_DCI_TOOLS.\n"
            "`dci plugins` lists every registered adapter/command/backend.\n"
            "\n"
            "exit codes: 0 ok, 1 validation failure, 2 usage/input error"
        ),
    )
    parser.add_argument(
        "--version", action="version",
        version=(f"vyx-dci-sdk {dci_plugin.SDK_VERSION} "
                 f"(DCI contract format {dci_plugin.CONTRACT_FORMAT})"),
    )
    sub = parser.add_subparsers(dest="command", required=True)

    adapter = sub.add_parser("adapter", aliases=["adapt"], help="generate a native ABI contract")
    sdk_adapters.add_shared_arguments(adapter)
    adapter.set_defaults(handler=command_adapter)

    for name, plugin in COMMANDS.items():
        command = sub.add_parser(name, aliases=list(plugin.aliases),
                                 help=plugin.help)
        plugin.add_arguments(command)
        command.set_defaults(handler=plugin.handler, command=name)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    adapter_extra: list[str] = []
    if raw_argv and raw_argv[0] in {"adapter", "adapt"} and "--" in raw_argv:
        marker = raw_argv.index("--")
        adapter_extra = raw_argv[marker + 1 :]
        raw_argv = raw_argv[:marker]
    raw_argv = dci_adapter_cpp.normalize_dash_valued_options(
        raw_argv, dash_valued_options()
    )
    args = parser.parse_args(raw_argv)
    setattr(args, "adapter_extra", adapter_extra)
    # Keep the surviving command line: the declared parameter ownership turns
    # it into the cross-language diagnostic (see adapter_option_conflicts).
    setattr(args, "raw_argv", raw_argv)
    try:
        return int(args.handler(args))
    except (DciCliError, cpp_toolchains.CppToolchainError,
            dci_adapter_cpp.AdapterContractError,
            dci_adapter_zig.ZigAdapterError,
            dci_validate.DciValidationError, dcib.DcibError) as exc:
        print(f"dci: error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("dci: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

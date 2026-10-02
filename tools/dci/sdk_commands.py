#!/usr/bin/env python3
"""Built-in DCI SDK commands, registered on the plugin registry.

Pure extract-method refactor from ``dci.py`` — outputs, exit codes and error
messages are byte-identical.  New code should add commands through
``dci_plugin.register_command`` instead of editing the CLI core.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any

try:
    from dci_plugin import (
        ADAPTERS, COMMANDS, CommandPlugin, DciCliError, adapter_option_conflicts,
        add_adapter_parameters, register_command, snapshot,
    )
except ImportError:  # installed as a package / imported as tools.dci.*
    from tools.dci.dci_plugin import (  # type: ignore
        ADAPTERS, COMMANDS, CommandPlugin, DciCliError, adapter_option_conflicts,
        add_adapter_parameters, register_command, snapshot,
    )

try:
    import dcib
    import dci_validate
    import cpp_toolchains
except ImportError:
    from tools.dci import dcib, dci_validate, cpp_toolchains  # type: ignore


TOOL_DIR = Path(__file__).resolve().parent
DEFAULT_SCHEMA = TOOL_DIR / "schema" / "dci-1.0.schema.json"


def load_contract(path: Path) -> Any:
    if not path.is_file():
        raise DciCliError(f"contract not found: {path}")
    if path.suffix.lower() == ".dcib":
        try:
            return dcib.decode(path.read_bytes())
        except dcib.DcibError as exc:
            raise DciCliError(f"invalid DCIB contract {path}: {exc}") from exc
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise DciCliError(f"invalid JSON contract {path}: {exc}") from exc


def write_json(path: Path, document: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def _contract_summary(path: Path, document: Any) -> dict[str, Any]:
    if not isinstance(document, dict):
        raise DciCliError(f"contract root must be an object: {path}")
    profile = document.get("profile") if isinstance(document.get("profile"), dict) else {}
    source = document.get("source") if isinstance(document.get("source"), dict) else {}
    exports = document.get("exports") if isinstance(document.get("exports"), dict) else {}
    target_value = profile.get("target", document.get("target", ""))
    if isinstance(target_value, dict):
        target_value = target_value.get("triple", target_value.get("name", ""))

    def count(name: str) -> int:
        value = exports.get(name)
        return len(value) if isinstance(value, list) else 0

    producer = document.get("producer") if isinstance(document.get("producer"), dict) else {}
    compiler = source.get("compiler")
    if not isinstance(compiler, dict):
        compiler = producer.get("compiler") if isinstance(producer, dict) else {}
    if not isinstance(compiler, dict):
        compiler = {}
    flags = compiler.get("flags")
    if not isinstance(flags, list):
        flags = []

    return {
        "path": str(path),
        "format": "dcib" if path.suffix.lower() == ".dcib" else "json",
        "dci": document.get("dci", ""),
        "kind": document.get("kind", ""),
        "target": target_value,
        "profile": profile.get("name", profile.get("id", "")),
        "source_language": source.get("language", ""),
        "compiler_vendor": compiler.get("vendor", compiler.get("name", "")),
        "compiler_version": compiler.get("version", ""),
        "compiler_triplet": compiler.get("target_triplet", ""),
        "compiler_flags": flags,
        "layouts": count("layouts"),
        "symbols": count("symbols"),
        "dispatch_tables": count("dispatch_tables") or count("vtables"),
        "type_aliases": count("type_aliases") or count("aliases"),
        "rejected_symbols": count("rejected_symbols"),
    }


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def command_validate(args: argparse.Namespace) -> int:
    schema_path = Path(args.schema).expanduser()
    try:
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise DciCliError(f"cannot load DCI schema {schema_path}: {exc}") from exc
    failed = False
    for raw_path in args.contracts:
        path = Path(raw_path).expanduser()
        document = load_contract(path)
        errors = dci_validate.schema_errors(schema, document)
        if isinstance(document, dict):
            errors.extend(dci_validate.semantic_errors(document, args.strict))
        if errors:
            failed = True
            print(f"DCI validation failed: {path}", file=sys.stderr)
            for error in errors:
                print(f"  {error}", file=sys.stderr)
        else:
            print(f"DCI validation OK: {path}")
    return 1 if failed else 0


def command_inspect(args: argparse.Namespace) -> int:
    path = Path(args.contract).expanduser()
    document = load_contract(path)
    if args.full:
        print(json.dumps(document, ensure_ascii=False, indent=2))
        return 0
    summary = _contract_summary(path, document)
    if args.json:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return 0
    print(f"DCI contract: {summary['path']}")
    for key in (
        "format",
        "dci",
        "kind",
        "target",
        "profile",
        "source_language",
        "compiler_vendor",
        "compiler_version",
        "compiler_triplet",
        "compiler_flags",
        "layouts",
        "symbols",
        "dispatch_tables",
        "type_aliases",
        "rejected_symbols",
    ):
        value = summary[key]
        if key == "compiler_flags":
            value = " ".join(value) if value else "(none)"
        print(f"  {key.replace('_', ' ')}: {value}")
    return 0


def command_encode(args: argparse.Namespace) -> int:
    source = Path(args.input).expanduser()
    output = Path(args.output).expanduser()
    document = load_contract(source)
    dcib.validate_cjson_compatible(document)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(dcib.encode(document))
    print(f"DCIB encoded: {output}")
    return 0


def command_decode(args: argparse.Namespace) -> int:
    source = Path(args.input).expanduser()
    output = Path(args.output).expanduser()
    write_json(output, load_contract(source))
    print(f"DCIB decoded: {output}")
    return 0


def command_doctor(args: argparse.Namespace) -> int:
    failed = False
    print(f"Python: {sys.executable} ({sys.version.split()[0]})")
    print(f"Schema: {DEFAULT_SCHEMA}")
    if not DEFAULT_SCHEMA.is_file():
        print("  error: schema is missing", file=sys.stderr)
        failed = True
    try:
        from sdk_adapters import canonical_target
    except ImportError:
        from tools.dci.sdk_adapters import canonical_target  # type: ignore
    try:
        from dci_plugin import ADAPTERS, adapter_request
    except ImportError:
        from tools.dci.dci_plugin import (  # type: ignore
            ADAPTERS, adapter_request,
        )
    language = args.language
    plugin = ADAPTERS.get(language)
    if plugin is None:
        raise DciCliError(
            f"no adapter registered for --language {language!r}; "
            f"registered: {', '.join(sorted(ADAPTERS))}"
        )
    if plugin.doctor is None:
        raise DciCliError(f"the {language} Adapter has no doctor entry point")
    rejection = adapter_option_conflicts(language, getattr(args, "raw_argv", ()))
    if rejection:
        raise DciCliError(rejection)
    request = adapter_request(
        args, language=language, target=canonical_target(args.triplet)
    )
    rc = plugin.doctor(request)
    return 1 if (failed or rc != 0) else 0


def command_plugins(args: argparse.Namespace) -> int:
    info = snapshot()
    if args.json:
        print(json.dumps(info, ensure_ascii=False, indent=2))
        return 0
    print(f"DCI SDK {info['sdk_version']} (contract format {info['contract_format']})")
    print("language adapters (--language):")
    for lang, meta in info["adapters"].items():
        print(f"  {lang:<10} [{meta['origin']}]")
    print("commands:")
    for name, meta in info["commands"].items():
        print(f"  {name:<10} [{meta['origin']}] {meta['help']}")
    print("stub compile backends:")
    for name, meta in info["stub_backends"].items():
        langs = ",".join(meta["languages"])
        print(f"  {name:<10} [{meta['origin']}] languages: {langs}")
    if info["external_plugins"]:
        print("external plugins loaded:")
        for origin in info["external_plugins"]:
            print(f"  {origin}")
    else:
        print("external plugins loaded: (none)")
    print("extend the SDK: see tools/dci/EXTENDING.md")
    return 0


register_command(CommandPlugin(
    name="validate",
    aliases=("check",),
    help="validate JSON or DCIB contracts",
    add_arguments=lambda sub: (
        sub.add_argument("contracts", nargs="+"),
        sub.add_argument("--schema", default=str(DEFAULT_SCHEMA)),
        sub.add_argument("--strict", action="store_true"),
    ),
    handler=command_validate,
))

register_command(CommandPlugin(
    name="inspect",
    aliases=("info",),
    help="show contract metadata and export counts",
    add_arguments=lambda sub: (
        sub.add_argument("contract"),
        sub.add_argument("--json", action="store_true",
                         help="emit the summary as JSON"),
        sub.add_argument("--full", action="store_true",
                         help="decode and print the complete document"),
    ),
    handler=command_inspect,
))

register_command(CommandPlugin(
    name="encode",
    help="encode diagnostic JSON as canonical DCIB",
    add_arguments=lambda sub: (
        sub.add_argument("input"),
        sub.add_argument("output"),
    ),
    handler=command_encode,
))

register_command(CommandPlugin(
    name="decode",
    help="decode DCIB as readable JSON",
    add_arguments=lambda sub: (
        sub.add_argument("input"),
        sub.add_argument("output"),
    ),
    handler=command_decode,
))

def _add_doctor_arguments(sub) -> None:
    """`dci doctor` exposes the parameters the adapters flagged for probing."""
    sub.add_argument(
        "--language", choices=sorted(ADAPTERS) or ("cpp", "rust", "zig"), default="cpp",
        help="source language Adapter to verify (default: cpp)",
    )
    add_adapter_parameters(sub, doctor=True)


register_command(CommandPlugin(
    name="doctor",
    help="verify Adapter prerequisites",
    add_arguments=_add_doctor_arguments,
    handler=command_doctor,
))

register_command(CommandPlugin(
    name="plugins",
    help="list registered adapters, commands and stub backends",
    add_arguments=lambda sub: sub.add_argument(
        "--json", action="store_true", help="emit the registry snapshot as JSON"),
    handler=command_plugins,
))

assert COMMANDS  # built-ins registered on import

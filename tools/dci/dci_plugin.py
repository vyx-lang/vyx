#!/usr/bin/env python3
"""Vyx DCI SDK plugin core: parameters, registries and extension discovery.

The DCI toolchain is shipped as a small SDK with three extension points.
Everything built into the tree is registered through the same API a third
party would use, so "what can I extend" and "how do built-ins work" have a
single answer.

Extension points
----------------
* **Language adapters** (`register_adapter`) — produce a `.dcib` contract from
  a producer source.  Keyed by `--language` value; the built-ins are
  ``cpp`` / ``rust`` / ``zig``.
* **CLI commands** (`register_command`) — additional ``dci <command>``
  subcommands.  Built-ins: ``validate`` / ``inspect`` / ``encode`` / ``decode``
  / ``doctor`` / ``plugins``.
* **Stub compile backends** (`register_stub_backend`) — strategies that turn
  emitted shim source into a real object file (name -> compile(args) -> rc).

Adapter parameters
------------------
`dci adapter` has exactly one parameter set.  A parameter is keyed by its
``dest`` — the concept it sets — and every spelling that sets that concept
declares the same ``dest``:

    param("compiler", "--rustc")   # rust plugin: --compiler and --rustc
    param("compiler", "--cxx")     # cpp  plugin: --compiler and --cxx

The parser is composed from those declarations, and the declaration also
carries the diagnostic: a spelling owned only by another language adapter is
rejected when it shows up under a different ``--language``.  Consequences:

* one canonical spelling per concept (``--compiler``, ``--compiler-arg``,
  ``--extractor``, ``--extractor-arg``, ``--namespace``) plus legacy aliases;
* no hand-written per-language option list, no hand-written rejection rules;
* a new adapter contributes its parameters by declaring them, nothing else.

Every adapter hook receives exactly one argument, an :class:`AdapterRequest`.

Discovery sources
-----------------
1. Built-ins, registered at import of :mod:`sdk_adapters` / :mod:`sdk_commands`
   / :mod:`stub_backend`.
2. ``plugins/*.py`` beside this file — every module found is imported once;
   a module registers itself by calling the decorators at import time.
3. Installed distributions via importlib entry points (groups
   ``vyx_dci.adapters`` / ``vyx_dci.commands`` / ``vyx_dci.backends``), for
   SDK consumers that install the toolchain with pip.

Plugin loading is fail-open: a broken third-party plugin is reported on
stderr and skipped; core functionality is never taken down by it.
"""

from __future__ import annotations

import argparse
import importlib
import importlib.util
import sys
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

SDK_VERSION = "1.1.0"
CONTRACT_FORMAT = "1.0"

ENTRY_POINT_GROUPS = {
    "adapters": "vyx_dci.adapters",
    "commands": "vyx_dci.commands",
    "backends": "vyx_dci.backends",
}

# Owner marker for a spelling every adapter accepts (the SDK's own
# language-neutral parameters such as ``-o/--output``).
ALL_LANGUAGES = "*"

# How an option consumes the command line.  ``optional`` mirrors
# argparse's ``nargs="?"`` (bare flag means "use the derived default").
PARAMETER_KINDS = ("value", "append", "flag", "optional")


class DciCliError(RuntimeError):
    """User-facing CLI error; `dci` prints it and exits 2."""


# --------------------------------------------------------------------------
# adapter parameters
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class OptionSpec:
    """One adapter's declaration of one parameter spelling set.

    ``dest`` is the concept (the unified parameter a language adapter reads);
    ``flags`` are the spellings this declaration contributes.  Declarations
    from different adapters are merged by ``dest`` and each spelling keeps the
    language that declared it — that ownership is what makes cross-language
    misuse a derived diagnostic instead of a hand-maintained string.
    """

    dest: str
    flags: tuple[str, ...]
    kind: str = "value"
    metavar: str | None = None
    choices: tuple[str, ...] | None = None
    default: Any = None
    help: str = ""
    doctor: bool = False
    convert: Callable[[str], Any] | None = None

    def __post_init__(self) -> None:
        if self.kind not in PARAMETER_KINDS:
            raise ValueError(
                f"parameter {self.dest!r}: unknown kind {self.kind!r} "
                f"(expected one of {', '.join(PARAMETER_KINDS)})"
            )
        if not self.flags:
            raise ValueError(f"parameter {self.dest!r} declares no spelling")
        for flag in self.flags:
            if not flag.startswith("-"):
                raise ValueError(
                    f"parameter {self.dest!r}: spelling {flag!r} must start with '-'"
                )


@dataclass
class AdapterOption:
    """Merged view of one parameter: its shape plus who owns each spelling."""

    dest: str
    kind: str = "value"
    metavar: str | None = None
    choices: tuple[str, ...] | None = None
    default: Any = None
    help: str = ""
    doctor: bool = False
    convert: Callable[[str], Any] | None = None
    spellings: dict[str, tuple[str, ...]] = field(default_factory=dict)

    @property
    def flags(self) -> tuple[str, ...]:
        return tuple(self.spellings)

    @property
    def languages(self) -> tuple[str, ...]:
        owners: list[str] = []
        for spelled_by in self.spellings.values():
            for language in spelled_by:
                if language not in owners:
                    owners.append(language)
        return tuple(owners)

    def universal(self) -> bool:
        return ALL_LANGUAGES in self.languages

    def accepted_by(self, language: str) -> bool:
        return self.universal() or language in self.languages

    def unrecognized_spellings(self, language: str) -> list[str]:
        return [
            flag for flag, owners in self.spellings.items()
            if language not in owners and ALL_LANGUAGES not in owners
        ]

    def help_text(self) -> str:
        # The note is only informative when several languages spell the same
        # parameter differently; plain hyphen/underscore variants are already
        # visible in argparse's own rendering of the option strings.
        if len(self.languages) < 2 or len(self.spellings) < 2:
            return self.help
        listed = ", ".join(self.spellings)
        note = f"[one parameter: {listed}]"
        return f"{self.help} {note}" if self.help else note

    def effective_default(self) -> Any:
        if self.default is not None:
            return self.default
        if self.kind == "append":
            return []
        if self.kind == "flag":
            return False
        return None

    def add_to(self, parser: argparse.ArgumentParser) -> None:
        kwargs: dict[str, Any] = {
            "dest": self.dest,
            "default": self.effective_default(),
            "help": self.help_text(),
        }
        if self.kind == "append":
            kwargs["action"] = "append"
        elif self.kind == "flag":
            kwargs["action"] = "store_true"
        elif self.kind == "optional":
            kwargs["nargs"] = "?"
            kwargs["const"] = ""
        if self.metavar:
            kwargs["metavar"] = self.metavar
        if self.choices:
            kwargs["choices"] = self.choices
        if self.convert is not None:
            kwargs["type"] = self.convert
        parser.add_argument(*self.flags, **kwargs)


_PARAMETER_SPECS: list[tuple[str | None, OptionSpec]] = []


def register_adapter_parameters(specs: Iterable[OptionSpec], *,
                                language: str | None = None) -> None:
    """Declare the parameter spellings one adapter owns.

    ``language=None`` marks a spelling every adapter accepts (the SDK's own
    language-neutral parameters such as ``-o/--output``).
    """
    for spec in specs:
        _PARAMETER_SPECS.append((language, spec))


def adapter_options() -> list[AdapterOption]:
    """Every declared parameter, merged by ``dest``, in declaration order."""
    merged: dict[str, AdapterOption] = {}
    for language, spec in _PARAMETER_SPECS:
        option = merged.get(spec.dest)
        if option is None:
            option = AdapterOption(
                dest=spec.dest, kind=spec.kind, metavar=spec.metavar,
                choices=spec.choices, default=spec.default, help=spec.help,
                doctor=spec.doctor, convert=spec.convert,
            )
            merged[spec.dest] = option
        elif option.kind != spec.kind:
            raise DciCliError(
                f"parameter {spec.dest!r} is declared as both {option.kind!r} "
                f"and {spec.kind!r}"
            )
        # `doctor` exposure is additive: any adapter that wants the parameter
        # in `dci doctor` gets it there, whichever declaration came first.
        option.doctor = option.doctor or spec.doctor
        owner = language if language is not None else ALL_LANGUAGES
        for flag in spec.flags:
            owners = option.spellings.get(flag, ())
            if owner not in owners:
                option.spellings[flag] = owners + (owner,)
    return list(merged.values())


def add_adapter_parameters(parser: argparse.ArgumentParser, *,
                           doctor: bool = False) -> None:
    """Attach the merged parameter table to a parser (the whole composition)."""
    for option in adapter_options():
        if doctor and not option.doctor:
            continue
        option.add_to(parser)


def dash_valued_options() -> set[str]:
    """Long repeatable spellings whose value may itself start with ``-``.

    ``--compile_flags -Iinclude`` would otherwise look like a stray option to
    argparse; ``dci`` joins such pairs into ``--compile_flags=-Iinclude``
    before parsing.  Derived from the declarations, never hand-listed.
    """
    spells: set[str] = set()
    for option in adapter_options():
        if option.kind != "append":
            continue
        spells.update(flag for flag in option.spellings if flag.startswith("--"))
    return spells


def adapter_option_conflicts(language: str, argv: Sequence[str]) -> str | None:
    """Report spellings in ``argv`` that belong to another language adapter."""
    used = {token.split("=", 1)[0] for token in argv}
    groups: dict[tuple[str, ...], list[str]] = {}
    for option in adapter_options():
        for flag, owners in option.spellings.items():
            if flag not in used or language in owners or ALL_LANGUAGES in owners:
                continue
            groups.setdefault(owners, []).append(flag)
    if not groups:
        return None
    detail = "; ".join(
        f"{', '.join(flags)} (--language {'/'.join(owners)})"
        for owners, flags in groups.items()
    )
    return (
        f"adapter option(s) belong to another language adapter: {detail}; "
        f"this run is --language {language}"
    )


# --------------------------------------------------------------------------
# adapter request
# --------------------------------------------------------------------------

def _is_set(value: Any) -> bool:
    if value is None or value is False:
        return False
    if isinstance(value, (str, list, tuple, dict)):
        return len(value) > 0
    return True


@dataclass
class AdapterRequest:
    """Everything one language adapter is handed — its only hook parameter.

    ``options`` maps every declared parameter ``dest`` to its parsed value, so
    an adapter reads ``request.options["compiler"]`` regardless of which
    spelling the caller used.  ``argv`` is filled in after ``build_argv`` ran,
    which is why ``run`` still receives the same single request object.
    """

    language: str
    sources: tuple[Path, ...] = ()
    output: Path | None = None
    target: str = ""
    debug_json: Path | None = None
    stub_out: Path | None = None
    artifacts: tuple[str, ...] = ()
    passthrough: tuple[str, ...] = ()
    options: Mapping[str, Any] = field(default_factory=dict)
    argv: tuple[str, ...] = ()

    def get(self, dest: str, default: Any = None) -> Any:
        return self.options.get(dest, default)

    def has(self, dest: str) -> bool:
        return _is_set(self.options.get(dest))

    def many(self, dest: str) -> tuple[str, ...]:
        value = self.options.get(dest)
        if value is None:
            return ()
        if isinstance(value, (list, tuple)):
            return tuple(value)
        return (value,)

    @property
    def compiler(self) -> str | None:
        """The producer compiler, whatever spelling selected it."""
        return self.get("compiler")

    @property
    def compiler_args(self) -> tuple[str, ...]:
        return self.many("compiler_arg")

    @property
    def extractor(self) -> str | None:
        return self.get("extractor")

    @property
    def extractor_args(self) -> tuple[str, ...]:
        return self.many("extractor_arg")

    @property
    def namespace(self) -> str | None:
        return self.get("namespace")

    def with_argv(self, argv: Sequence[str]) -> "AdapterRequest":
        return replace(self, argv=tuple(argv))


def resolve_sources(headers: Sequence[str], include: Sequence[str] = ()) -> tuple[Path, ...]:
    """Expand and verify the adapter's source inputs, keeping their order."""
    paths = tuple(Path(value).expanduser() for value in (*headers, *include))
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        raise DciCliError("Adapter input not found: " + ", ".join(missing))
    return paths


def adapter_input_error(language: str, sources: Sequence[Path]) -> str | None:
    """The declared input rule of the selected adapter, or ``None``."""
    if not sources:
        nouns = sorted({
            plugin.input_noun for plugin in ADAPTERS.values() if plugin.input_noun
        })
        listed = ", ".join(nouns) if nouns else "producer source file"
        return f"adapter requires a source input ({listed})"
    plugin = ADAPTERS.get(language)
    if plugin is None or plugin.max_sources is None:
        return None
    if len(sources) > plugin.max_sources:
        return (
            f"the {language} Adapter requires exactly one "
            f"{plugin.input_noun} input"
        )
    return None


def adapter_request(args: argparse.Namespace, *, language: str,
                    sources: Sequence[Path] = (), target: str = "",
                    passthrough: Sequence[str] = ()) -> AdapterRequest:
    """Wrap parsed shared-CLI args into the object every hook receives."""
    options: dict[str, Any] = {
        option.dest: getattr(args, option.dest, option.effective_default())
        for option in adapter_options()
    }
    output_value = options.get("output")
    if output_value:
        output = Path(output_value).expanduser()
    elif sources:
        output = Path(sources[0].stem + ".dcib")
    else:
        output = None

    debug_value = options.get("debug_json")
    debug_json: Path | None = None
    if debug_value is not None and output is not None:
        debug_json = (
            Path(debug_value).expanduser() if debug_value
            else output.with_suffix(".dci.json")
        )

    stub_value = options.get("stub_out")
    return AdapterRequest(
        language=language,
        sources=tuple(sources),
        output=output,
        target=target,
        debug_json=debug_json,
        stub_out=Path(stub_value).expanduser() if stub_value else None,
        artifacts=tuple(options.get("artifact") or ()),
        passthrough=tuple(passthrough),
        options=options,
    )


# --------------------------------------------------------------------------
# registries
# --------------------------------------------------------------------------

@dataclass
class AdapterPlugin:
    """Produce a DCIB contract from one producer language.

    Every hook takes exactly one :class:`AdapterRequest`:

    * ``build_argv(request) -> argv`` assembles the concrete adapter module's
      command line;
    * ``run(request) -> exit code`` executes it (``request.argv`` is set);
    * ``doctor(request) -> exit code`` implements the ``--language`` branch of
      ``dci doctor``.

    ``parameters`` declares the spellings this adapter owns — the parser, the
    help text and the cross-language diagnostics are all derived from it.
    ``validate(request)`` is the escape hatch for rules a declaration cannot
    express.  ``max_sources``/``input_noun`` state the input rule.
    """

    language: str
    build_argv: Callable[[AdapterRequest], list[str]]
    run: Callable[[AdapterRequest], int]
    parameters: tuple[OptionSpec, ...] = ()
    doctor: Callable[[AdapterRequest], int] | None = None
    validate: Callable[[AdapterRequest], str | None] | None = None
    max_sources: int | None = None
    input_noun: str = "source input"
    origin: str = "builtin"


@dataclass
class CommandPlugin:
    """An extra ``dci <name>`` subcommand."""

    name: str
    handler: Callable[[Any], int]
    add_arguments: Callable[[Any], None]
    help: str = ""
    aliases: tuple[str, ...] = ()
    origin: str = "builtin"


@dataclass
class StubBackendPlugin:
    """Compile emitted shim source into an object file.

    ``compile(args) -> int`` receives the parsed ``compile`` subcommand args
    (``--source``/``--output``/``--include-args``/``--compile-args`` already
    resolved) plus the resolved producer ``lang`` on ``args.lang``.
    """

    name: str
    compile: Callable[[Any], int]
    languages: tuple[str, ...] = ("rust", "cpp")
    origin: str = "builtin"


ADAPTERS: dict[str, AdapterPlugin] = {}
COMMANDS: dict[str, CommandPlugin] = {}
STUB_BACKENDS: dict[str, StubBackendPlugin] = {}
PLUGIN_ORIGINS: list[str] = []  # human-readable provenance of loaded plugins


def register_adapter(plugin: AdapterPlugin) -> AdapterPlugin:
    ADAPTERS[plugin.language] = plugin
    register_adapter_parameters(plugin.parameters, language=plugin.language)
    return plugin


def unregister_adapter(language: str) -> None:
    """Drop an adapter together with its parameter declarations."""
    ADAPTERS.pop(language, None)
    _PARAMETER_SPECS[:] = [
        entry for entry in _PARAMETER_SPECS if entry[0] != language
    ]


def register_command(plugin: CommandPlugin) -> CommandPlugin:
    COMMANDS[plugin.name] = plugin
    return plugin


def register_stub_backend(plugin: StubBackendPlugin) -> StubBackendPlugin:
    STUB_BACKENDS[plugin.name] = plugin
    return plugin


def adapter_plugin(language: str, *, origin: str = "builtin") -> Callable:
    """Decorator turning a class (or any callable factory) into an adapter."""

    def wrap(factory: Callable[[], AdapterPlugin]) -> Callable[[], AdapterPlugin]:
        plugin = factory()
        plugin.language = language
        plugin.origin = origin
        register_adapter(plugin)
        return factory

    return wrap


# --------------------------------------------------------------------------
# external plugin discovery
# --------------------------------------------------------------------------

def _load_entry_points() -> None:
    try:
        if sys.version_info >= (3, 10):
            from importlib.metadata import entry_points
            eps = entry_points()
            for kind, group in ENTRY_POINT_GROUPS.items():
                for ep in eps.select(group=group):
                    obj = ep.load()
                    if kind == "commands" and isinstance(obj, CommandPlugin):
                        register_command(obj)
                    elif kind == "adapters" and isinstance(obj, AdapterPlugin):
                        register_adapter(obj)
                    elif kind == "backends" and isinstance(obj, StubBackendPlugin):
                        register_stub_backend(obj)
                    elif callable(obj):  # a module-level register function
                        obj()
                    PLUGIN_ORIGINS.append(f"entry-point {ep.value}")
    except Exception as exc:  # fail-open: a bad installed plugin must not
        print(f"dci: warning: entry-point plugin discovery failed: {exc}",
              file=sys.stderr)


def load_plugins_dir(directory: Path | None = None) -> None:
    """Import every ``*.py`` directly inside ``directory`` (default: plugins/)."""
    root = Path(directory) if directory else Path(__file__).resolve().parent / "plugins"
    if not root.is_dir():
        return
    for path in sorted(root.glob("*.py")):
        if path.name.startswith("_"):
            continue
        mod_name = f"dci_plugin_ext_{path.stem}"
        if mod_name in sys.modules:
            continue
        try:
            spec = importlib.util.spec_from_file_location(mod_name, path)
            if spec is None or spec.loader is None:
                continue
            module = importlib.util.module_from_spec(spec)
            sys.modules[mod_name] = module
            spec.loader.exec_module(module)
            PLUGIN_ORIGINS.append(f"plugins/{path.name}")
        except Exception as exc:  # fail-open
            print(f"dci: warning: plugin {path} failed to load and was "
                  f"skipped: {exc}", file=sys.stderr)


def load_external_plugins(plugins_dir: Path | None = None) -> None:
    """Load all third-party plugins (local plugins/ dir, then entry points)."""
    load_plugins_dir(plugins_dir)
    _load_entry_points()


def snapshot() -> dict[str, Any]:
    """Registry introspection for `dci plugins`."""
    return {
        "sdk_version": SDK_VERSION,
        "contract_format": CONTRACT_FORMAT,
        "adapters": {
            lang: {
                "origin": p.origin,
                "parameters": [flag for opt in p.parameters for flag in opt.flags],
            }
            for lang, p in ADAPTERS.items()
        },
        "parameters": {
            opt.dest: {
                "spellings": list(opt.spellings),
                "languages": list(opt.languages),
                "kind": opt.kind,
            }
            for opt in adapter_options()
        },
        "commands": {
            name: {"origin": p.origin, "help": p.help}
            for name, p in COMMANDS.items()
        },
        "stub_backends": {
            name: {"origin": p.origin, "languages": list(p.languages)}
            for name, p in STUB_BACKENDS.items()
        },
        "external_plugins": list(PLUGIN_ORIGINS),
    }

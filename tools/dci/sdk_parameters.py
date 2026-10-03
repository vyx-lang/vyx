#!/usr/bin/env python3
"""The single `dci adapter` parameter vocabulary.

One parameter = one ``dest`` = one concept.  This module owns the *canonical*
spelling and help text of every parameter that more than one language adapter
understands, and `param()` builds the declaration a language contributes:

    param("compiler", "--rustc")   # rust: --compiler (canonical) + --rustc
    param("compiler", "--cxx")     # cpp : --compiler (canonical) + --cxx

Both declarations merge by ``dest``, so ``--compiler``, ``--cxx`` and
``--rustc`` are the same parameter with three accepted names, and
``request.options["compiler"]`` is where every adapter reads it.  A spelling
only one language declares stays that language's; the parser and the
cross-language diagnostic are both derived from that ownership.

Adding a concept is one row in :data:`UNIFIED`.  Adding a language is calling
:func:`param` for the concepts it supports plus plain :class:`OptionSpec`
entries for its private parameters — no core change, no CLI edit.
"""

from __future__ import annotations

from typing import Any

try:
    from dci_plugin import OptionSpec, register_adapter_parameters
except ImportError:  # installed as a package / imported as tools.dci.*
    from tools.dci.dci_plugin import (  # type: ignore
        OptionSpec, register_adapter_parameters,
    )


# ``dest`` -> canonical spelling + shape/help shared by every adapter that
# supports the concept.  A language adds its legacy alias when declaring it.
UNIFIED: dict[str, dict[str, Any]] = {
    "compiler": {
        "metavar": "PATH",
        "help": "producer compiler to run; the per-language aliases are "
                "--cxx (cpp), --rustc (rust) and --zig (zig)",
    },
    "compiler_arg": {
        "kind": "append",
        "metavar": "ARG",
        "help": "one extra argument for that compiler; repeatable",
    },
    "extractor": {
        "metavar": "PATH",
        "help": "structured fact extractor (clang++/clang-cl); C++ only",
    },
    "extractor_arg": {
        "kind": "append",
        "metavar": "ARG",
        "help": "one extra argument for the extractor; repeatable",
    },
    "namespace": {
        "metavar": "NAME",
        "help": "DCI namespace recorded in the contract",
    },
}


def unified_flag(dest: str) -> str:
    """The canonical spelling of ``dest`` (``compiler_arg`` -> ``--compiler-arg``)."""
    return "--" + dest.replace("_", "-")


def param(dest: str, *aliases: str, **overrides: Any) -> OptionSpec:
    """Declare unified parameter ``dest``, with this language's legacy aliases."""
    shape: dict[str, Any] = {
        "kind": "value",
        "metavar": None,
        "help": "",
        **UNIFIED.get(dest, {}),
    }
    shape.update(overrides)
    return OptionSpec(dest=dest, flags=(unified_flag(dest), *aliases), **shape)


# Parameters every adapter accepts, whatever the ``--language`` value: they
# describe the contract and its output, not the producer toolchain.
SHARED_PARAMETERS: tuple[OptionSpec, ...] = (
    OptionSpec(
        dest="output", flags=("-o", "--output"), metavar="PATH",
        help="output .dcib path (default: <first-input>.dcib)",
    ),
    OptionSpec(
        dest="triplet", flags=("--triplet", "--target"), default="x64_windows",
        metavar="TRIPLE", doctor=True,
        help="LLVM triple or convenience alias (--target is a compatibility alias)",
    ),
    OptionSpec(
        dest="debug_json", flags=("--debug-json",), kind="optional", metavar="PATH",
        help="write diagnostic JSON beside the output (bare flag: <output>.dci.json)",
    ),
    OptionSpec(
        dest="stub_out", flags=("--stub-out",), metavar="PATH",
        help="write generated translate_unwind stub source (C++ or Rust)",
    ),
    OptionSpec(
        dest="artifact", flags=("--artifact",), kind="append", metavar="PATH",
        help="native object/library represented by the contract; repeatable",
    ),
)


def register_shared_parameters() -> None:
    """Register the language-neutral rows (idempotent; called by sdk_adapters)."""
    register_adapter_parameters(SHARED_PARAMETERS, language=None)

#!/usr/bin/env python3
"""Example DCI SDK plugin: adds a `dci hello` subcommand.

Drop a module like this into tools/dci/plugins/ (or ship it as a
`vyx_dci.commands` entry point) and it appears in the CLI — no core changes:

    $ dci hello --name SDK
    hello SDK (from plugins/example_hello.py, DCI SDK 1.1.0)
    $ dci plugins          # the new command is listed with origin=plugins

Delete this file to remove the command; it exists purely as a working
template for EXTENDING.md.
"""

from __future__ import annotations

import argparse

try:
    from dci_plugin import CommandPlugin, SDK_VERSION, register_command
except ImportError:  # installed as a package
    from tools.dci.dci_plugin import (  # type: ignore
        CommandPlugin, SDK_VERSION, register_command,
    )


def _run(args: argparse.Namespace) -> int:
    print(f"hello {args.name} (from plugins/example_hello.py, DCI SDK {SDK_VERSION})")
    return 0


register_command(CommandPlugin(
    name="hello",
    help="example plugin command (see plugins/example_hello.py)",
    add_arguments=lambda sub: sub.add_argument(
        "--name", default="world", help="who to greet"),
    handler=_run,
    origin="plugins/example_hello.py",
))

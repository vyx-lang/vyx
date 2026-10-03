#!/usr/bin/env python3
"""Canonical entry point for the multi-frontend Vyx C++ DCI Adapter.

``dci_adapter_msvc.py`` remains the implementation module and compatibility
entry point because existing SDKs invoke it directly.  New integrations should
use this ABI-neutral name.
"""

from __future__ import annotations

import sys

try:
    from .dci_adapter_msvc import *  # noqa: F401,F403
    from .dci_adapter_msvc import cli, main
except ImportError:
    from dci_adapter_msvc import *  # type: ignore # noqa: F401,F403
    from dci_adapter_msvc import cli, main  # type: ignore


if __name__ == "__main__":
    raise SystemExit(cli(sys.argv[1:]))

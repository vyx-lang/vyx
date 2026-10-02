# DCI SDK plugins directory

Every `*.py` directly inside this directory is imported once when the DCI CLI
starts (files starting with `_` are skipped).  A plugin registers itself at
import time through the decorators in `dci_plugin.py`; anything it registers
shows up in `dci plugins` and in `dci -h` immediately.

A broken plugin is reported on stderr and skipped — it can never take the
core CLI down (fail-open).

See `../EXTENDING.md` for the full API, including pip-installed plugins via
the `vyx_dci.commands` / `vyx_dci.adapters` / `vyx_dci.backends` entry-point
groups.

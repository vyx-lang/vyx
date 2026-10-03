# Repository scripts

[Documentation](../docs/README.md) · [Bootstrap and verification](../docs/TESTING_GUIDE.md)

Root scripts are repository-level maintenance tools. `ir_diff.ps1` and
`run_gates.ps1` operate on the historical host IR snapshot corpus; they are not
current Release-seed bootstrap acceptance gates until that corpus is migrated.
Compiler implementation probes belong under `bootstrap_compiler/scripts/`; Vyx
contract checks belong under `tests/checks/` and are dispatched by
`tests/checks/run_checks.ps1`.

The development toolchain is the repository-root `clang/` SDK. Bootstrap and
verification commands should set `LLVM_ROOT` to that directory (or pass the
equivalent script parameter); do not silently select a machine-wide LLVM.
Use a compatible compiler from [Releases latest](https://github.com/vyx-lang/vyx/releases/latest)
as Stage 0; set `VYX_BOOTSTRAP_VYXC` to its executable.

| Script | Purpose |
| --- | --- |
| `setup_llvm22.sh` | Provision an LLVM 22.x SDK when the repository `clang/` SDK is unavailable; it does not replace the repository SDK used for development. |
| `install_vyxc.sh` | Unpack `dist/my-folder.tar.xz` to `/opt/vyx` and install a `vyxc` PATH wrapper. |
| `run_gates.ps1` | IR snapshot and repository gate orchestration. |
| `ir_diff.ps1` | Compare normalized IR against stable snapshot IDs. |
| `run_newmir_focus.ps1` | Focused MIR regression matrix. |
| `check_std_drift.ps1` | Verify host/bootstrap standard-library mirror drift. |
| `verify_selfhost_readiness.ps1` | Preflight checks before a self-host run. |
| `clean_outputs.ps1` | Dry-run-first cleanup for ignored caches and outputs. |

Do not add ad-hoc `tmp_*`, `probe_*`, or timestamped scripts here.  Put a
durable check in the appropriate `tests/checks/` suite and register it in the
dispatcher, or document a compiler-only gate in
`bootstrap_compiler/scripts/README.md`.

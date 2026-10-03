#!/usr/bin/env python3
"""Built-in language adapters registered on the DCI SDK plugin registry.

Each plugin declares, in one table, which parameters it accepts — the unified
spelling plus its legacy aliases — and implements three hooks that all take
the same single argument, an :class:`~dci_plugin.AdapterRequest`:

    parameters   -> the `dci adapter` CLI surface (and its diagnostics)
    build_argv   -> that module's argv
    run          -> execute it
    doctor       -> the `--language` branch of `dci doctor`

Because the declarations merge by ``dest``, ``--compiler`` / ``--cxx`` /
``--rustc`` / ``--zig`` are one parameter with four names, and a spelling only
one language declares (``--extractor``, ``--edition``, ``--toolchain``, …) is
rejected under another ``--language`` without a hand-written reject rule.

The adapter implementation modules (``dci_adapter_msvc.py`` /
``dci_adapter_rust.py`` / ``dci_adapter_zig.py``) stay independently callable
with their own argv; gates invoke them directly.
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from dci_plugin import (
        ADAPTERS, AdapterPlugin, AdapterRequest, DciCliError,
        add_adapter_parameters, register_adapter,
    )
except ImportError:  # installed as a package / imported as tools.dci.*
    from tools.dci.dci_plugin import (  # type: ignore
        ADAPTERS, AdapterPlugin, AdapterRequest, DciCliError,
        add_adapter_parameters, register_adapter,
    )

try:
    from sdk_parameters import OptionSpec, param, register_shared_parameters
except ImportError:
    from tools.dci.sdk_parameters import (  # type: ignore
        OptionSpec, param, register_shared_parameters,
    )

try:
    import dci_adapter_rust
    import dci_adapter_zig
    import dci_adapter_cpp
except ImportError:
    from tools.dci import dci_adapter_rust, dci_adapter_zig, dci_adapter_cpp  # type: ignore

try:
    import cpp_toolchains
except ImportError:
    from tools.dci import cpp_toolchains  # type: ignore


register_shared_parameters()


def canonical_target(value: str) -> str:
    return cpp_toolchains.canonical_target(value)


def _resolved_directory(value: str, label: str) -> str:
    path = Path(value).expanduser()
    if not path.is_dir():
        raise DciCliError(f"{label} not found: {path}")
    return str(path.resolve())


# --------------------------------------------------------------------------
# cpp
# --------------------------------------------------------------------------

_CPP_PARAMETERS: tuple[OptionSpec, ...] = (
    param("compiler", "--cxx", doctor=True,
          help="clang++/g++/cl.exe that is the ABI authority"),
    param("compiler_arg", "--cxx-arg", "--frontend-arg",
          help="one extra argument for the selected validation frontend; repeatable"),
    param("extractor", "--clang", doctor=True,
          help="clang++/clang-cl used as the structured helper"),
    param("extractor_arg", "--clang-arg"),
    OptionSpec(
        dest="toolchain", flags=("--toolchain",), default="clang", doctor=True,
        choices=("auto", "clang", "gcc", "msvc"),
        help="C++ frontend to validate (ABI is selected independently by --triplet)",
    ),
    OptionSpec(dest="std", flags=("--std",), default="c++17",
               help="C++ language standard"),
    OptionSpec(
        dest="jobs", flags=("-j", "--jobs"), metavar="N", convert=int,
        help="parallel Clang AST-dump workers (default: min(32, CPU count); "
             "also DCI_ADAPTER_JOBS)",
    ),
    OptionSpec(dest="include", flags=("--include",), kind="append", metavar="PATH",
               help="additional public header"),
    OptionSpec(dest="include_dir", flags=("-I", "--include-dir"), kind="append",
               metavar="DIR", help="Clang include search directory"),
    OptionSpec(dest="project_root", flags=("--project-root",), kind="append",
               metavar="DIR", help="root whose public headers are exported"),
    OptionSpec(dest="scan_public_root", flags=("--scan-public-root",), kind="flag",
               help="export every header below --project-root"),
    OptionSpec(dest="cmake_build_path", flags=("--cmake_build_path", "--cmake-build-path"),
               metavar="DIR", help="CMake build directory with compile_commands.json; "
               "real include dirs, defines, standard and layout flags are applied "
               "automatically"),
    OptionSpec(dest="compile_flags", flags=("--compile_flags", "--compile-flags"),
               kind="append", metavar="FLAG",
               help="compile_flags.txt-style file or an inline flag string; repeatable"),
)


def _cpp_build_argv(request: AdapterRequest) -> list[str]:
    argv: list[str] = [
        "--out", str(request.output),
        "--toolchain", request.get("toolchain"),
    ]
    if request.compiler:
        argv.extend(["--cxx", request.compiler])
    if request.extractor:
        argv.extend(["--clang", request.extractor])
    argv.extend(["--target", request.target, "--std", request.get("std")])

    jobs = request.get("jobs")
    if jobs is not None:
        if jobs < 1:
            raise DciCliError("--jobs must be >= 1")
        argv.extend(["--jobs", str(jobs)])
    if request.debug_json is not None:
        argv.extend(["--debug-json-out", str(request.debug_json)])
    if request.stub_out is not None:
        argv.extend(["--stub-out", str(request.stub_out)])

    for header in request.sources:
        argv.extend(["--include", str(header.resolve())])
    for root in request.many("project_root"):
        argv.extend(["--project-root", _resolved_directory(root, "project root")])
    if request.has("scan_public_root"):
        argv.append("--scan-public-root")

    clang_args = list(request.extractor_args)
    for include_dir in request.many("include_dir"):
        clang_args.extend(["-I", _resolved_directory(include_dir, "include directory")])

    for compiler_arg in request.compiler_args:
        argv.extend(["--cxx-arg", compiler_arg])
    if request.has("cmake_build_path"):
        argv.extend([
            "--cmake_build_path",
            _resolved_directory(request.get("cmake_build_path"), "CMake build path"),
        ])
    for compile_flag in request.many("compile_flags"):
        argv.append(f"--compile_flags={compile_flag}")
    for artifact in request.artifacts:
        argv.extend(["--artifact", artifact])

    clang_args.extend(request.passthrough)
    if clang_args:
        argv.append("--")
        argv.extend(clang_args)
    return argv


def _cpp_run(request: AdapterRequest) -> int:
    rc = dci_adapter_cpp.main(list(request.argv))
    if rc == 0:
        print(f"DCI adapter OK: {request.output}")
        if request.debug_json is not None:
            print(f"DCI diagnostic JSON: {request.debug_json}")
    return rc


def _cpp_doctor(request: AdapterRequest) -> int:
    import subprocess

    failed = False
    try:
        toolchain = cpp_toolchains.discover_cpp_toolchain(
            request.get("toolchain"),
            executable=request.compiler,
            target=request.target,
        )
        print(
            f"C++ frontend: {toolchain.family} {toolchain.version or '(unknown version)'}"
        )
        print(f"  driver: {toolchain.executable}")
        print(f"  mode: {toolchain.driver_mode}")
        print(f"  native target: {toolchain.native_target or '(unknown)'}")
        if toolchain.family != "clang" or toolchain.driver_mode != "clang":
            extractor = cpp_toolchains.discover_cpp_toolchain(
                "clang", executable=request.extractor, target=request.target
            )
            if extractor.driver_mode != "clang":
                raise cpp_toolchains.CppToolchainError(
                    "structured extraction requires clang++, not clang-cl"
                )
            print(f"C++ fact extractor: {extractor.executable}")
    except (cpp_toolchains.CppToolchainError, OSError, subprocess.SubprocessError) as exc:
        print(f"C++ frontend: error: {exc}", file=sys.stderr)
        failed = True
    print(f"Default triplet: {request.target}")
    return 1 if failed else 0


register_adapter(AdapterPlugin(
    language="cpp",
    parameters=_CPP_PARAMETERS,
    build_argv=_cpp_build_argv,
    run=_cpp_run,
    doctor=_cpp_doctor,
    input_noun="C++ public header",
))


# --------------------------------------------------------------------------
# rust
# --------------------------------------------------------------------------

_RUST_PARAMETERS: tuple[OptionSpec, ...] = (
    param("compiler", "--rustc", doctor=True, help="rustc to probe (default: discovered)"),
    param("compiler_arg", "--rustc-arg"),
    param("namespace", "--crate-name"),
    OptionSpec(
        dest="edition", flags=("--edition",), choices=("2015", "2018", "2021", "2024"),
        help="Rust edition (defaults to Cargo.toml or 2021)",
    ),
    OptionSpec(
        dest="deny_rejected", flags=("--deny-rejected",), kind="flag",
        help="fail when an attempted native export is not in the stable ABI subset",
    ),
    OptionSpec(
        dest="export_active_requests", flags=("--export-active-requests",), kind="flag",
        help="export Active Adapter requests for open generics instead of "
             "rejecting them",
    ),
    OptionSpec(
        dest="export_instance", flags=("--export-instance",), kind="append",
        metavar="TYPE<ARGS>",
        help="export a closed generic instance the host references "
             "(e.g. 'Pair<i32>'); repeatable",
    ),
)


def _rust_build_argv(request: AdapterRequest) -> list[str]:
    argv: list[str] = [str(request.sources[0].resolve()), "--output", str(request.output)]
    argv.extend(["--target", request.target])
    if request.has("edition"):
        argv.extend(["--edition", request.get("edition")])
    if request.compiler:
        argv.extend(["--rustc", request.compiler])
    if request.namespace:
        argv.extend(["--crate-name", request.namespace])
    if request.debug_json is not None:
        argv.extend(["--debug-json-out", str(request.debug_json)])
    if request.stub_out is not None:
        argv.extend(["--stub-out", str(request.stub_out)])
    for artifact in request.artifacts:
        argv.extend(["--artifact", artifact])
    for instance in request.many("export_instance"):
        argv.extend(["--export-instance", instance])
    for compiler_arg in request.compiler_args:
        argv.append("--rustc-arg=" + compiler_arg)
    argv.extend(request.passthrough)
    if request.has("deny_rejected"):
        argv.append("--deny-rejected")
    if request.has("export_active_requests"):
        argv.append("--export-active-requests")
    return argv


def _rust_run(request: AdapterRequest) -> int:
    # Late-binding module attribute: integrations (and tests) patch
    # `dci_adapter_rust.main` on the module, so resolve it at call time.
    return dci_adapter_rust.main(list(request.argv))


def _rust_doctor(request: AdapterRequest) -> int:
    failed = False
    try:
        rustc = dci_adapter_rust.discover_rustc(request.compiler)
        identity = dci_adapter_rust.rustc_identity(rustc)
        rust_target = dci_adapter_rust.target_info(rustc, request.target)
        print(f"Rust frontend: rustc {identity.get('version', '(unknown version)')}")
        print(f"  driver: {rustc}")
        print(f"  LLVM: {identity.get('llvm', '(unknown)')}")
        print(f"  target: {rust_target.triple}")
        print(f"  ABI family: {rust_target.abi_family}")
    except dci_adapter_rust.RustAdapterError as exc:
        print(f"Rust frontend: error: {exc}", file=sys.stderr)
        failed = True
    print(f"Default triplet: {dci_adapter_rust.canonical_rust_target(request.target)}")
    return 1 if failed else 0


register_adapter(AdapterPlugin(
    language="rust",
    parameters=_RUST_PARAMETERS,
    build_argv=_rust_build_argv,
    run=_rust_run,
    doctor=_rust_doctor,
    max_sources=1,
    input_noun="crate-root .rs",
))


# --------------------------------------------------------------------------
# zig
# --------------------------------------------------------------------------

_ZIG_PARAMETERS: tuple[OptionSpec, ...] = (
    param("compiler", "--zig", doctor=True, help="zig to probe (default: discovered)"),
    param("compiler_arg", "--zig-arg"),
    param("namespace", "--crate-name"),
    OptionSpec(
        dest="deny_rejected", flags=("--deny-rejected",), kind="flag",
        help="fail when an attempted native export is not in the stable ABI subset",
    ),
)


def _zig_build_argv(request: AdapterRequest) -> list[str]:
    argv: list[str] = [str(request.sources[0].resolve()), "--output", str(request.output)]
    argv.extend(["--target", request.target])
    if request.compiler:
        argv.extend(["--zig", request.compiler])
    if request.namespace:
        argv.extend(["--crate-name", request.namespace])
    if request.debug_json is not None:
        argv.extend(["--debug-json-out", str(request.debug_json)])
    for artifact in request.artifacts:
        argv.extend(["--artifact", artifact])
    for compiler_arg in request.compiler_args:
        argv.append("--zig-arg=" + compiler_arg)
    argv.extend(request.passthrough)
    if request.has("deny_rejected"):
        argv.append("--deny-rejected")
    return argv


def _zig_run(request: AdapterRequest) -> int:
    return dci_adapter_zig.main(list(request.argv))  # late-binding, see _rust_run


def _zig_doctor(request: AdapterRequest) -> int:
    failed = False
    try:
        zig = dci_adapter_zig.discover_zig(request.compiler)
        identity = dci_adapter_zig.zig_identity(zig)
        zig_target = dci_adapter_zig.canonical_zig_target(request.target)
        print(f"Zig frontend: zig {identity.get('version', '(unknown version)')}")
        print(f"  driver: {zig}")
        print(f"  target: {zig_target}")
        print(f"  host: {identity.get('host', '(unknown)')}")
    except dci_adapter_zig.ZigAdapterError as exc:
        print(f"Zig frontend: error: {exc}", file=sys.stderr)
        failed = True
    print(f"Default triplet: {dci_adapter_zig.canonical_zig_target(request.target)}")
    return 1 if failed else 0


register_adapter(AdapterPlugin(
    language="zig",
    parameters=_ZIG_PARAMETERS,
    build_argv=_zig_build_argv,
    run=_zig_run,
    doctor=_zig_doctor,
    max_sources=1,
    input_noun="crate-root .zig",
))


# --------------------------------------------------------------------------
# shared `dci adapter` CLI surface
# --------------------------------------------------------------------------

def add_shared_arguments(adapter) -> None:
    """Compose `dci adapter` from the registry: inputs, --language, parameters."""
    adapter.add_argument(
        "headers", nargs="*",
        help="public C/C++ header(s), Rust crate-root .rs, or Zig root .zig",
    )
    adapter.add_argument(
        "--language", choices=sorted(ADAPTERS) or ("cpp", "rust", "zig"), default="cpp",
        help="source language Adapter (default: cpp)",
    )
    add_adapter_parameters(adapter)

#!/usr/bin/env python3
"""Build an SDK on its native Windows/Linux host, then package it."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time

from common import REPO, bootstrap_asset, checksum_sidecar, sha256, unpack_sdk


def run(command, *, cwd=REPO, env=None):
    print("+ " + " ".join(map(str, command)), flush=True)
    argv = list(map(str, command))
    if sys.platform == "linux" and shutil.which("stdbuf"):
        argv = ["stdbuf", "-oL", "-eL"] + argv
    started = time.monotonic()
    with subprocess.Popen(argv, cwd=cwd, env=env) as child:
        while True:
            try:
                code = child.wait(timeout=30)
                break
            except subprocess.TimeoutExpired:
                print(f"[release] {Path(str(command[0])).name} still running: "
                      f"{time.monotonic() - started:.0f}s, pid={child.pid}", flush=True)
        if code:
            raise subprocess.CalledProcessError(code, argv)
    print(f"[release] completed in {time.monotonic() - started:.1f}s", flush=True)


def check_standard_library(compiler, work, environment):
    output = work / "stdlib-contracts"
    run([sys.executable, "-m", "unittest", "discover", "-s",
         REPO / "probes/gates/stdlib-contracts", "-p", "test_runner.py"], env=environment)
    run([sys.executable, REPO / "probes/gates/stdlib-contracts/run.py",
         "--compiler", compiler, "--output", output, "--negative", "--model"], env=environment)
    report = output / "verification.json"
    result = json.loads(report.read_text(encoding="utf-8"))
    return {"checks": len(result["results"]), "passed": sum(row["passed"] for row in result["results"]),
            "compiler_sha256": result["compiler_sha256"], "report_sha256": sha256(report)}


def check_semantic_composition(compiler, work, environment):
    gate = REPO / "probes/gates/language-composition/suite.py"
    output = work / "semantic-composition"
    corpus = output / "corpus"
    results = output / "run"
    run([sys.executable, gate, "generate", "--rules",
         "operator_overload,operator_generic,index_overload,pointer_index_owner,unary_operator,short_circuit_scan,large_function,overloaded_iterator,overloaded_array_clone,dynamic_overload,generic_function_value",
         "--depth", "2", "--no-types", "--no-negative", "--out", corpus], env=environment)
    run([sys.executable, gate, "run", "--suite", corpus / "suite.json", "--compiler", compiler,
         "--layouts", "file,project,modules,library", "--optimizations", "0,2", "--cgus", "default",
         "--kind", "positive", "--out", results], env=environment)
    report = results / "results.json"
    result = json.loads(report.read_text(encoding="utf-8"))
    expected = 798 * 8
    if (result["generated"] != 798 or result["selected"] != 798 or len(result["matrix"]) != 8
            or result["compiler"]["sha256"] != sha256(compiler)
            or result["status"] != "pass" or not result["complete"]
            or result["passed_case_cell_count"] != expected):
        raise RuntimeError("semantic composition gate did not execute and pass every selected case/configuration")
    return {"generated": result["generated"], "passed_case_cells": result["passed_case_cell_count"],
            "configurations": len(result["matrix"]), "report_sha256": sha256(report)}


ITERATION_PROTOCOL_CASES = (
    "for_collection_delimiters", "bad_iteration_has_next_return", "bad_iteration_next_args",
    "str_array_iteration", "owned_array_clone", "nested_array_clone", "bad_iterator_clone", "bad_array_clone",
)


def check_iteration_protocol(compiler, work, environment):
    return check_protocol_edges(compiler, work, environment, ITERATION_PROTOCOL_CASES, "protocol-edges")


DYNAMIC_DISPATCH_CASES = (
    "dyn_overload", "dyn_overload_return", "dyn_generic_overload", "dyn_owned_arguments",
    "dyn_inherited", "dyn_inherited_overload", "dyn_structural", "bad_dyn_missing_member",
)


def check_dynamic_dispatch(compiler, work, environment):
    return check_protocol_edges(compiler, work, environment, DYNAMIC_DISPATCH_CASES, "dynamic-edges")


VALUE_BINDING_CASES = (
    "generic_function_values", "generic_function_owned", "generic_function_overloads", "generic_function_containers",
    "bad_generic_function_arity", "bad_generic_function_open", "bad_generic_function_argument",
    "bad_function_value_ambiguous", "bad_generic_function_bound", "bad_generic_function_type",
    "await_borrow", "await_owned_lifetime", "bad_await_scalar", "bad_await_void",
    "bad_await_poll", "bad_await_payload", "for_direct_ranges", "await_taken_result",
)


def check_value_bindings(compiler, work, environment):
    return check_protocol_edges(compiler, work, environment, VALUE_BINDING_CASES, "value-bindings")


def check_dynamic_library(compiler, work, environment):
    gate = REPO / "probes/gates/language-composition/suite.py"
    output = work / "dynamic-library"
    corpus = output / "corpus"
    run([sys.executable, gate, "generate", "--rules", "dynamic_overload,operator_overload,overloaded_iterator",
         "--depth", "2", "--no-types", "--no-negative", "--out", corpus], env=environment)
    run([sys.executable, gate, "run", "--suite", corpus / "suite.json", "--compiler", compiler,
         "--layouts", "library", "--optimizations", "0,2", "--cgus", "1,4",
         "--out", output / "run"], env=environment)
    report = output / "run/results.json"
    result = json.loads(report.read_text(encoding="utf-8"))
    matrix = {tuple(cell) for cell in result["matrix"]}
    required = {("library", opt, cgu) for opt in ("0", "2") for cgu in ("1", "4")}
    if (result["compiler"]["sha256"] != sha256(compiler) or result["generated"] != 78
            or result["selected"] != 78 or matrix != required or len(result["matrix"]) != 4
            or result["status"] != "pass" or not result["complete"] or result["passed_case_cell_count"] != 312):
        raise RuntimeError("dynamic library gate did not pass every selected case/configuration")
    return {"generated": 78, "passed_case_cells": 312, "report_sha256": sha256(report)}


def check_protocol_edges(compiler, work, environment, cases, directory):
    output = work / directory
    run([sys.executable, REPO / "probes/gates/language-composition/check_edges.py",
         "--compiler", compiler, "--out", output, "--optimizations", "0,2", "--cgus", "default",
         "--cases", ",".join(cases)],
        env=environment)
    report = output / "results.json"
    result = json.loads(report.read_text(encoding="utf-8"))
    required = {(name, opt, "default") for name in cases for opt in ("0", "2")}
    actual = {(row.get("name"), row.get("opt"), row.get("cgu")) for row in result["results"]}
    if (result["compiler"]["sha256"] != sha256(compiler) or result["passed"] != len(required)
            or result["failed"] != 0 or len(result["results"]) != len(required) or actual != required
            or not all(row["pass"] for row in result["results"])):
        raise RuntimeError("protocol gate did not pass every required case/configuration")
    return {"checks": result["passed"], "report_sha256": sha256(report)}


def bootstrap_manifest(manifest, sdk):
    setting = 'parallel_std_sources = "std_packages"'
    if manifest.count(setting) != 1:
        raise RuntimeError("Bootstrap standard-library source selection drifted")
    return manifest.replace(setting, "parallel_std_sources = " + json.dumps((sdk / "std").as_posix()))


def stage_windows_tbb(build):
    """Finish Stage 0's handoff before starting any executable from out/.

    Older bootstrap compilers overwrite the source-built TBB DLL with their SDK
    copy after linking boot. The current runtime's CMake output owns this pair.
    """
    outputs = build.parent / "runtime/vendor/oneTBB/cmake-build"
    names = ("tbb12.dll", "tbb12.lib")
    producer = next((outputs / leaf for leaf in ("tbb_out_release", "clang_22.1_cxx_64_md_release")
                     if all((outputs / leaf / name).is_file() for name in names)), None)
    if producer is None:
        raise RuntimeError(f"Current oneTBB producer output is incomplete: {outputs}")
    for name in names:
        if (producer / name).stat().st_size == 0:
            raise RuntimeError(f"Current oneTBB producer output is empty: {producer / name}")
    record = {"producer": str(producer), "files": {}}
    for name in names:
        source, destination = producer / name, build / "out" / name
        expected = sha256(source)
        changed = not destination.is_file() or sha256(destination) != expected
        if changed:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        if sha256(destination) != expected:
            raise RuntimeError(f"Current oneTBB handoff hash mismatch: {destination}")
        record["files"][name] = {"source": str(source), "destination": str(destination),
                                "sha256": expected, "replaced_stage0_copy": changed}
    return record


def verify_debug_adapter(adapter, platform_name, patch):
    manifest = json.loads((adapter / "MANIFEST.json").read_text(encoding="utf-8"))
    if manifest.get("platform") != platform_name:
        raise RuntimeError(f"CodeLLDB platform mismatch: expected {platform_name}")
    expected_patch = sha256(patch)
    if manifest.get("patch_sha256") != expected_patch:
        raise RuntimeError(f"CodeLLDB patch hash mismatch: manifest={manifest.get('patch_sha256')}, "
                           f"source={expected_patch}; check patch checkout line endings or rebuild the adapter")
    if sha256(adapter / "VYX-CHANGES.patch") != expected_patch:
        raise RuntimeError("CodeLLDB bundled patch differs from the source patch")
    executable = adapter / "adapter" / ("codelldb.exe" if platform_name.startswith("windows-") else "codelldb")
    if sha256(executable) != manifest.get("binary_sha256"):
        raise RuntimeError("CodeLLDB executable hash differs from its manifest; rebuild the adapter")


def bootstrap_with_current_backend(sdk, backend, work, windows):
    """Keep Stage 0's executable/runtime with the current ABI-compatible bridge.

    Released Stage 0 bridges can emit build-CPU instructions. Merely linking
    the new bridge into S1 does not change the bridge that emitted S1's code.
    A separate loader directory makes every Stage 0 child use the new bridge.
    """
    overlay = work / "stage0-bridge"
    overlay.mkdir(exist_ok=False)
    name = "vyxc.exe" if windows else ".vyxc-bin"
    shutil.copy2(sdk / "bin" / name, overlay / name)
    shutil.copy2(backend, overlay / backend.name)
    if windows:
        # Keep seed DLL dependencies away from out/: the native TBB hook must
        # be able to replace its output without a loaded-DLL sharing violation.
        for dependency in (sdk / "bin").glob("*.dll"):
            if dependency.name.lower() != backend.name.lower():
                shutil.copy2(dependency, overlay / dependency.name)
    return overlay / name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap-tag", default="latest")
    parser.add_argument("--bootstrap-sdk", type=Path)
    llvm_input = parser.add_mutually_exclusive_group(required=True)
    llvm_input.add_argument("--llvm-root", type=Path)
    llvm_input.add_argument("--use-sdk-llvm", action="store_true",
                           help="Linux: use LLVM development inputs from the verified Stage 0 SDK")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    windows = os.name == "nt"
    arch = {"AMD64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64",
            "aarch64": "aarch64"}.get(platform.machine())
    if arch is None or (windows and arch != "x86_64") or (not windows and sys.platform != "linux"):
        parser.error("Supported hosts: Windows x64, Linux x64 and Linux AArch64")
    host = "windows" if windows else "linux"
    if windows and args.use_sdk_llvm:
        parser.error("--use-sdk-llvm is supported on native Linux hosts")
    leaf = f"vyx-sdk-{host}-{arch}-llvm22"
    name = leaf + (".zip" if windows else ".tar.gz")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = args.bootstrap_sdk.resolve() if args.bootstrap_sdk else unpack_sdk(
        bootstrap_asset(args.bootstrap_tag, name, work / "downloads"), work / "bootstrap")
    if args.use_sdk_llvm:
        from prepare_linux_llvm import prepare_sdk_llvm
        args.llvm_root = prepare_sdk_llvm(sdk, work / "llvm-22")
    tools = ["vyxc-lsp"] + (["vyxc-dap"] if arch == "x86_64" else [])
    adapter = sdk / "bin/debugger" if "vyxc-dap" in tools else None
    if adapter is not None:
        verify_debug_adapter(adapter, f"{host}-{arch}",
                             REPO / "tools/debugger/patches/codelldb-1.12.3-vyx.patch")
        print("CodeLLDB source patch and executable verified before compilation", flush=True)
    # Invoke the ELF payload directly so its shell launcher cannot replace the
    # explicitly selected source-build LLVM_ROOT with the bundled SDK root.
    compiler = sdk / "bin" / ("vyxc.exe" if windows else ".vyxc-bin")
    environment = os.environ.copy()
    environment.update(LLVM_ROOT=str(args.llvm_root.resolve()),
        LLVM_BIN=str(args.llvm_root.resolve() / "bin"),
        VYX_STD_PACKAGES=str(sdk / "std_packages"),
        VYX_BUILD_VYX_SLOTS="1", VYX_BUILD_MEMORY_SLOTS="2", VYX_CGU_EMIT_THREADS="1",
        VYX_CODEGEN_UNITS="1", VYX_BUILD_PROFILE="1")
    environment["PATH"] = str(args.llvm_root.resolve() / "bin") + os.pathsep + environment["PATH"]
    if not windows:
        environment["LD_LIBRARY_PATH"] = str(args.llvm_root.resolve() / "lib") + ":" + str(sdk / "bin") + ":" + str(sdk / "lib") + (
            ":" + environment["LD_LIBRARY_PATH"] if environment.get("LD_LIBRARY_PATH") else "")
    # Keep bootstrap-only std selection out of the checkout and out of SDK
    # source assets. The release always builds the exact committed source.
    source = work / "source"
    source.mkdir(exist_ok=False)
    source_archive = work / "source.tar"
    run(["git", "archive", "--format=tar", f"--output={source_archive}", "HEAD"])
    with tarfile.open(source_archive) as bundle:
        bundle.extractall(source, filter="data")
    build = source / "bootstrap_compiler"
    manifest_path = build / "Vyx.toml"
    original_manifest = manifest_path.read_text(encoding="utf-8")
    out = build / "out"
    record = {"host": host, "arch": arch, "source_commit": os.environ.get("VYX_RELEASE_SOURCE_COMMIT") or subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
        "bootstrap_tag": args.bootstrap_tag,
        "bootstrap_compiler_sha256": sha256(compiler),
        "bootstrap_manifest_sha256": sha256(sdk / "SDK-MANIFEST.json"),
        "bootstrap_version": subprocess.check_output([str(compiler), "--version"], env=environment, text=True).strip(),
        "llvm_root": str(args.llvm_root.resolve()), "jobs": args.jobs, "vyx_slots": 1}
    if args.use_sdk_llvm:
        record["llvm_input"] = json.loads((args.llvm_root / "VYX-LLVM-INPUT.json").read_text(encoding="utf-8"))
    manifest_path.write_text(bootstrap_manifest(original_manifest, sdk), encoding="utf-8")
    try:
        # Build the bridge with native C++ first, then use it for all emitted
        # Vyx objects. This also fixes ISA portability when Stage 0 is old.
        run([compiler, "build", "--target", "vyx_compiler_backend", f"-j{args.jobs}"], cwd=build, env=environment)
        backend = out / ("vyx_compiler_backend.dll" if windows else "libvyx_compiler_backend.so")
        compiler = bootstrap_with_current_backend(sdk, backend, work, windows)
        environment["VYX_RUNTIME_ROOT"] = str(sdk / "lib")
        if not windows:
            environment["LD_LIBRARY_PATH"] = str(compiler.parent) + ":" + environment["LD_LIBRARY_PATH"]
        record["bootstrap_codegen_backend_sha256"] = sha256(backend)
        run([sys.executable, REPO / "probes/gates/aot-portability/run.py", "--backend", backend,
             "--output", work / "baseline-isa"], env=environment)
        run([sys.executable, REPO / "probes/gates/runtime-string-scan/run.py", "--backend", backend,
             "--cxx", args.llvm_root.resolve() / "bin" / ("clang++.exe" if windows else "clang++"),
             "--output", work / "runtime-string-scan"], env=environment)
        run([compiler, "--version"], env=environment)
        run([compiler, "build", "--target", "boot", f"-j{args.jobs}"], cwd=build, env=environment)
    finally:
        manifest_path.write_text(original_manifest, encoding="utf-8")
    if windows:
        record["current_tbb_handoff"] = stage_windows_tbb(build)
    built_compiler = out / ("boot.exe" if windows else "boot")
    shutil.copy2(built_compiler, out / ("vyxc.exe" if windows else "vyxc"))
    environment["VYX_STD_PACKAGES"] = str(build / "std_packages")
    environment["VYX_RUNTIME_ROOT"] = str(out)
    if not windows:
        # The source hook copies the library payload as libtbb.so. The ELF
        # SONAME must also be resolvable before running the freshly built SDK.
        tbb = out / "libtbb.so"
        if tbb.exists():
            dynamic = subprocess.check_output(["readelf", "-d", str(tbb)], text=True)
            soname = re.search(r"\(SONAME\).*\[([^]]+)\]", dynamic)
            if soname and not (out / soname[1]).exists():
                (out / soname[1]).symlink_to(tbb.name)
        environment["LD_LIBRARY_PATH"] = str(out) + (":" + environment["LD_LIBRARY_PATH"]
                                                    if environment.get("LD_LIBRARY_PATH") else "")
    if windows:
        run([sys.executable, REPO / "probes/gates/runtime-companions/run.py",
             "--compiler", built_compiler, "--runtime", out,
             "--output", work / "runtime-companions"], env=environment)
    run([sys.executable, REPO / "probes/gates/compiler-self-source/run.py",
         "--compiler", built_compiler, "--build-root", build,
         "--output", work / "self-source"], env=environment)
    run([sys.executable, REPO / "probes/gates/literal-artifact-types/run.py",
         "--compiler", built_compiler, "--output", work / "literal-types"], env=environment)
    run([sys.executable, REPO / "probes/gates/compiler-root-lists/run.py",
         "--compiler", built_compiler, "--build-root", build,
         "--output-dir", work / "root-lists"], env=environment)
    # Build and exercise tools before the long semantic/package suites. They
    # rebuild the runtime with S1, so those suites test the final shipped pair.
    for target in tools:
        run([built_compiler, "build", "--target", target, f"-j{args.jobs}"], cwd=build, env=environment)
        if target == "vyxc-lsp":
            run([sys.executable, REPO / "tests/bootstrap/lsp_smoke/run.py",
                 out / (target + (".exe" if windows else "")), "--runs", "1",
                 "--diagnostics-dir", work / "lsp-smoke"], env=environment)
            run([sys.executable, REPO / "probes/gates/formatter/run.py",
                 "--compiler", built_compiler, "--lsp", out / (target + (".exe" if windows else "")),
                 "--output", work / "formatter"], env=environment)
    run([sys.executable, REPO / "probes/gates/test-runner/run.py",
         "--compiler", built_compiler, "--output", work / "test-runner"], env=environment)
    record["semantic_composition"] = check_semantic_composition(built_compiler, work, environment)
    record["iteration_protocol"] = check_iteration_protocol(built_compiler, work, environment)
    record["dynamic_dispatch"] = check_dynamic_dispatch(built_compiler, work, environment)
    record["value_bindings"] = check_value_bindings(built_compiler, work, environment)
    record["dynamic_library"] = check_dynamic_library(built_compiler, work, environment)
    record["stdlib_contracts"] = check_standard_library(built_compiler, work, environment)
    if not windows:
        run([sys.executable, REPO / "probes/gates/directory-enumeration/run.py",
             "--compiler", built_compiler, "--cc", args.llvm_root / "bin/clang",
             "--output", work / "directory-enumeration"], env=environment)
    arguments = ["pwsh", "-NoProfile", "-File", REPO / "bootstrap_compiler/scripts/package_sdk.ps1",
        "-CompilerPath", built_compiler, "-Destination", output / leaf,
        "-BootstrapRoot", build, "-OutDir", out,
        "-Arch", arch, "-BundleLlvm", "-Archive"]
    if adapter is not None:
        arguments += ["-DebugAdapterRoot", adapter]
    run(arguments, env=environment)
    packaged = output / leaf
    smoke_environment = environment.copy()
    smoke_environment.update(VYX_RUNTIME_ROOT=str(packaged / "lib"),
                             VYX_STD_PACKAGES=str(packaged / "std_packages"),
                             LLVM_ROOT=str(packaged / "toolchain"),
                             LLVM_BIN=str(packaged / "toolchain/bin"))
    if not windows:
        smoke_environment["LD_LIBRARY_PATH"] = str(packaged / "bin") + ":" + str(packaged / "lib")
    run([sys.executable, REPO / "probes/gates/aot-portability/run.py",
         "--backend", packaged / "bin" / backend.name,
         "--compiler", packaged / "bin" / ("vyxc.exe" if windows else "vyxc"),
         "--output", work / "packaged-sdk-smoke"], env=smoke_environment)
    archive = output / name
    checksum_sidecar(archive)
    record.update(compiler_sha256=sha256(built_compiler), archive_sha256=sha256(archive),
                  archive_bytes=archive.stat().st_size, tools=tools,
                  backend_sha256=sha256(out / ("vyx_compiler_backend.dll" if windows else "libvyx_compiler_backend.so")),
                  runtime_sha256=sha256(out / ("vyx_runtime.lib" if windows else "libvyx_runtime.a")))
    (output / f"SDK-BUILD.{host}-{arch}.json").write_text(json.dumps(record, indent=2) + "\n")
    print(archive, flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Export macOS compiler objects on another OS, or self-host an SDK on macOS."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tomllib

from build_native import bootstrap_manifest, run
from common import REPO, bootstrap_asset, checksum_sidecar, sha256, unpack_sdk
from macos_common import TRIPLES, collect_objects, executable_content, source_commit, source_snapshot, verify_bundle


def build_environment(llvm, std):
    environment = os.environ.copy()
    environment.update(LLVM_ROOT=str(llvm), LLVM_BIN=str(llvm / "bin"),
        VYX_STD_PACKAGES=str(std), VYX_BUILD_VYX_SLOTS="1", VYX_BUILD_MEMORY_SLOTS="2",
        VYX_CGU_EMIT_THREADS="1", VYX_CODEGEN_UNITS="1", VYX_BUILD_PROFILE="1",
        CMAKE_BUILD_PARALLEL_LEVEL="4")
    environment["PATH"] = str(llvm / "bin") + os.pathsep + environment["PATH"]
    return environment


def export_manifest(original, build):
    target = original["target"]["boot"]
    sources = [(build / path).resolve().as_posix() for path in target["sources"] if path.endswith(".vyx")]
    # C/C++ and link dependencies are built by Apple Clang on the native host.
    # The Vyx module graph and standard interfaces use the normal build driver.
    return ('[package]\nname = "boot"\nversion = ' + json.dumps(original["package"]["version"]) +
        '\n[build]\noutput_dir = "out"\ncache_dir = ".cache-macos"\nvyxflags = ["-O2", "-fPIC"]\n'
        '[target.boot]\ntype = "executable"\nentry = ' + json.dumps((build / target["entry"]).as_posix()) +
        '\nsources = ' + json.dumps(sources) + '\nauto_sources = false\nparallel_vyx = true\n'
        'parallel_std_sources = ' + json.dumps((build / "std").as_posix()) +
        '\nparallel_std_excludes = ' + json.dumps(target["parallel_std_excludes"]) + '\n')


def export(args):
    if sys.platform == "darwin":
        raise RuntimeError("The cross bootstrap exporter runs on Windows or Linux; use native on macOS")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    host = "windows" if os.name == "nt" else "linux"
    host_arch = {"AMD64": "x86_64", "x86_64": "x86_64", "aarch64": "aarch64"}.get(platform.machine())
    if host_arch is None or (host == "windows" and host_arch != "x86_64"):
        raise RuntimeError("Unsupported cross bootstrap host")
    name = f"vyx-sdk-{host}-{host_arch}-llvm22" + (".zip" if os.name == "nt" else ".tar.gz")
    sdk = args.sdk.resolve() if args.sdk else unpack_sdk(
        bootstrap_asset(args.bootstrap_tag, name, work / "downloads"), work / "bootstrap")
    build = source_snapshot(work) / "bootstrap_compiler"
    compiler = sdk / "bin" / ("vyxc.exe" if os.name == "nt" else ".vyxc-bin")
    environment = build_environment(sdk / "toolchain", sdk / "std_packages")
    environment["VYX_RUNTIME_ROOT"] = str(sdk / "lib")
    if os.name != "nt":
        environment["LD_LIBRARY_PATH"] = f"{sdk}/bin:{sdk}/lib"
    manifest = build / "Vyx.toml"
    original = manifest.read_text(encoding="utf-8")
    exported_manifest = export_manifest(tomllib.loads(original), build)
    exported_manifest = exported_manifest.replace(json.dumps((build / "std").as_posix()),
                                                  json.dumps((sdk / "std").as_posix()))
    manifest.write_text(exported_manifest, encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    try:
        artifact = work / "compiler.objects"
        run([compiler, "build", "--target", "boot", "--emit=obj", "--triplet=" + TRIPLES[args.arch],
             "-o", artifact, f"-j{args.jobs}"], cwd=build, env=environment)
        objects = collect_objects(artifact, output / "compiler", args.arch, "compiler")
        runtime = work / "vyx_runtime.o"
        run([compiler, "--src=file", build.parent / "runtime/src/vyx_runtime.vyx", "--emit=obj",
             "--triplet=" + TRIPLES[args.arch], "-fPIC", "--export-all", "--no-process-entry",
             "-O1", "-o", runtime], cwd=build, env=environment)
        objects += collect_objects(runtime, output / "runtime", args.arch, "runtime")
    finally:
        manifest.write_text(original, encoding="utf-8")
    record = {"schema": 1, "arch": args.arch, "triple": TRIPLES[args.arch],
              "source_commit": source_commit(), "objects": objects,
              "export_host": platform.platform(), "compiler_sha256": sha256(compiler),
              "sdk_manifest_sha256": sha256(sdk / "SDK-MANIFEST.json"),
              "compiler_version": subprocess.check_output([compiler, "--version"], env=environment, text=True).strip()}
    (output / "BOOTSTRAP.json").write_text(json.dumps(record, indent=2) + "\n")
    verify_bundle(output, args.arch, source_commit())
    print(f"Exported {len(objects)} verified {args.arch} Mach-O objects to {output}", flush=True)


def native_support(build, llvm, environment, jobs):
    """Build native support without depending on a running Vyx compiler."""
    manifest = tomllib.loads((build / "Vyx.toml").read_text(encoding="utf-8"))
    out = build / "out"
    out.mkdir(exist_ok=True)
    cxx, cc = llvm / "bin/clang++", llvm / "bin/clang"
    tbb = build.parent / "runtime/vendor/oneTBB"
    run(["git", "clone", "--depth", "1", "--branch", "v2022.1.0",
         "https://github.com/uxlfoundation/oneTBB.git", tbb], env=environment)
    tbb_build = tbb / "cmake-build"
    run(["cmake", "-S", tbb, "-B", tbb_build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_CXX_COMPILER=" + str(cxx), "-DTBB_TEST=OFF", "-DTBB4PY_BUILD=OFF",
         "-DTBBMALLOC_BUILD=OFF", "-DTBB_STRICT=OFF", "-DTBB_ENABLE_IPO=OFF",
         "-DTBB_OUTPUT_DIR_BASE=tbb_out", "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0"], env=environment)
    run(["cmake", "--build", tbb_build, "--parallel", str(jobs)], env=environment)
    tbb_library = tbb_build / "tbb_out_release/libtbb.12.dylib"
    for name in ("libtbb.12.dylib", "libtbb.dylib"):
        shutil.copy2(tbb_library, out / name)
    run(["install_name_tool", "-id", "@rpath/libtbb.12.dylib", out / "libtbb.12.dylib"], env=environment)
    run(["codesign", "--force", "--sign", "-", out / "libtbb.12.dylib"], env=environment)
    shutil.copy2(out / "libtbb.12.dylib", out / "libtbb.dylib")
    backend = manifest["target"]["vyx_compiler_backend"]
    run([cxx, "-dynamiclib", "-fPIC", "-std=c++17", "-mmacosx-version-min=15.0",
         *backend["cxxflags"], "-I" + str(build.parent / "vyx_codegen/include"),
         "-I" + str(llvm / "include"), *[build / path for path in backend["sources"]],
         "-L" + str(llvm / "lib"), "-lLLVM", "-Wl,-install_name,@rpath/libvyx_compiler_backend.dylib",
         "-Wl,-rpath,@loader_path", "-o", out / "libvyx_compiler_backend.dylib"], env=environment)
    native = out / "bootstrap-native"
    native.mkdir()
    objects = []
    for index, path in enumerate(manifest["target"]["vyx_runtime"]["sources"]):
        source, target = build / path, native / f"runtime-{index}.o"
        command = cxx if source.suffix == ".cpp" else cc
        run([command, "-c", source, "-o", target, "-O2", "-fPIC", "-mmacosx-version-min=15.0",
             "-DTBB_USE_EXCEPTIONS=0", "-D__TBB_NO_IMPLICIT_LINKAGE=1", "-I" + str(tbb / "include")], env=environment)
        objects.append(target)
    cjson = native / "cJSON.o"
    run([cc, "-c", build / "std_packages/serde/vendor/cJSON.c", "-O2", "-fPIC",
         "-mmacosx-version-min=15.0", "-o", cjson], env=environment)
    return out, objects, cjson


def hello_gate(compiler, environment, work):
    directory = work / "hello-gate"
    directory.mkdir()
    source = directory / "hello.vyx"
    source.write_text('fn main() -> i32 { print("hello"); return 0; }\n')
    result = subprocess.run([compiler, "--src=file", source, "--dump-mir2"], env=environment,
                            capture_output=True, text=True, check=True, timeout=120)
    (directory / "dump-mir2.log").write_text(result.stdout + result.stderr)
    main = re.search(r"(?m)^  fn #(\d+) .* name=main ret=#\d+:i32 .*block_count=1 ", result.stdout)
    if not main:
        raise RuntimeError("Hello gate lacks one-block i32 main")
    counts = {kind: len(re.findall(r"(?m)^  " + kind + r" #\d+ fn=#" + main[1] + " ", result.stdout))
              for kind in ("block", "local", "place", "value", "instr")}
    if counts != {"block": 1, "local": 1, "place": 1, "value": 6, "instr": 2}:
        raise RuntimeError(f"Hello MIR main counts drifted: {counts}")
    target = re.search(r"(?m)^  value #\d+ fn=#" + main[1] +
                       r" kind=11 .*target_kind=1 target=#(\d+) .*name=print::<str> ", result.stdout)
    if not target or not re.search(r"(?m)^  fn #" + target[1] +
                                   r" .*ret=#\d+:void params=Ts args=str .*first_block=#[1-9]\d* ", result.stdout):
        raise RuntimeError("Hello print is not an executable standard-library function")
    print(f"PASS native hello MIR gate: {counts}; ordinary library print", flush=True)
    return counts


def native(args):
    arch = {"arm64": "aarch64", "x86_64": "x86_64"}.get(platform.machine())
    if sys.platform != "darwin" or arch != args.arch:
        raise RuntimeError("Native macOS build must run on the matching architecture")
    if bool(args.cross_bundle) == bool(args.bootstrap_sdk or args.bootstrap_tag):
        raise ValueError("Select exactly one bootstrap input: --cross-bundle, --bootstrap-sdk, or --bootstrap-tag")
    if args.bootstrap_sdk and args.bootstrap_tag:
        raise ValueError("--bootstrap-sdk and --bootstrap-tag are mutually exclusive")
    work, llvm = args.work.resolve(), args.llvm_root.resolve()
    work.mkdir(parents=True, exist_ok=False)
    source = source_snapshot(work)
    build = source / "bootstrap_compiler"
    environment = build_environment(llvm, build / "std_packages")
    environment.update(SDKROOT=subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip(),
                       MACOSX_DEPLOYMENT_TARGET="15.0")
    environment["DYLD_LIBRARY_PATH"] = str(build / "out") + ":" + str(llvm / "lib")
    out, native_objects, cjson = native_support(build, llvm, environment, args.jobs)
    backend = out / "libvyx_compiler_backend.dylib"
    record = {"host": "macos", "arch": arch, "source_commit": source_commit(),
              "llvm_input": json.loads((llvm / "VYX-LLVM-INPUT.json").read_text()), "tools": ["vyxc-lsp"],
              "jobs": args.jobs, "vyx_slots": 1, "bootstrap_codegen_backend_sha256": sha256(backend)}
    manifest = build / "Vyx.toml"
    original = manifest.read_text(encoding="utf-8")
    if args.cross_bundle:
        provenance, objects = verify_bundle(args.cross_bundle, arch, source_commit())
        record.update(bootstrap_mode="cross", bootstrap_input=provenance)
        run([llvm / "bin/llvm-ar", "rcs", out / "libvyx_runtime.a", *objects["runtime"], *native_objects], env=environment)
        seed = work / "cross-stage0"
        seed.mkdir()
        compiler = seed / "vyxc"
        run([llvm / "bin/clang++", *objects["compiler"], cjson, "-L" + str(out),
             "-lvyx_runtime", "-lvyx_compiler_backend", "-ltbb", "-lpthread",
             "-mmacosx-version-min=15.0", "-Wl,-rpath,@loader_path", "-o", compiler], env=environment)
    else:
        name = f"vyx-sdk-macos-{arch}-llvm22.tar.gz"
        sdk = args.bootstrap_sdk.resolve() if args.bootstrap_sdk else unpack_sdk(
            bootstrap_asset(args.bootstrap_tag, name, work / "downloads"), work / "bootstrap")
        record.update(bootstrap_mode="native", bootstrap_tag=args.bootstrap_tag,
                      bootstrap_manifest_sha256=sha256(sdk / "SDK-MANIFEST.json"))
        seed = work / "native-stage0"
        seed.mkdir()
        compiler = seed / "vyxc"
        shutil.copy2(sdk / "bin/.vyxc-bin", compiler)
        manifest.write_text(bootstrap_manifest(original, sdk), encoding="utf-8")
        environment["VYX_STD_PACKAGES"] = str(sdk / "std_packages")
        environment["VYX_RUNTIME_ROOT"] = str(sdk / "lib")
    shutil.copy2(backend, seed / backend.name)
    shutil.copy2(out / "libtbb.12.dylib", seed / "libtbb.12.dylib")
    environment["DYLD_LIBRARY_PATH"] = f"{seed}:{out}:{llvm / 'lib'}"
    environment["VYX_RUNTIME_ROOT"] = environment.get("VYX_RUNTIME_ROOT", str(out))
    record["bootstrap_compiler_sha256"] = sha256(compiler)
    record["bootstrap_version"] = subprocess.check_output([compiler, "--version"], env=environment, text=True).strip()
    run([sys.executable, REPO / "probes/gates/runtime-string-scan/run.py", "--backend", backend,
         "--cxx", llvm / "bin/clang++", "--output", work / "runtime-string-scan"], env=environment)
    try:
        run([compiler, "build", "--target", "boot", f"-j{args.jobs}"], cwd=build, env=environment)
    finally:
        manifest.write_text(original, encoding="utf-8")
    environment["VYX_STD_PACKAGES"] = str(build / "std_packages")
    environment["VYX_RUNTIME_ROOT"] = str(out)
    # Separate cache directories force cold native generations. Keep source and
    # output paths fixed so their identities cannot create artificial drift.
    stages = []
    generation_objects = {}
    for stage in ("s1", "s2", "s3"):
        directory = work / stage
        directory.mkdir()
        shutil.copy2(out / "boot", directory / "vyxc")
        shutil.copy2(backend, directory / backend.name)
        shutil.copy2(out / "libtbb.12.dylib", directory / "libtbb.12.dylib")
        environment["DYLD_LIBRARY_PATH"] = f"{directory}:{out}:{llvm / 'lib'}"
        stages.append(directory / "vyxc")
        if stage != "s3":
            next_stage = "s2" if stage == "s1" else "s3"
            cache = ".cache-" + next_stage
            if original.count('cache_dir = ".cache"') != 1:
                raise RuntimeError("Self-host cache setting drifted")
            manifest.write_text(original.replace('cache_dir = ".cache"', f'cache_dir = "{cache}"'), encoding="utf-8")
            try:
                run([directory / "vyxc", "build", "--target", "boot", f"-j{args.jobs}"], cwd=build, env=environment)
            finally:
                manifest.write_text(original, encoding="utf-8")
            generation_objects[next_stage] = {path.name: sha256(path) for path in (build / cache).rglob("*.o")}
    if not generation_objects["s2"] or generation_objects["s2"] != generation_objects["s3"]:
        raise RuntimeError("macOS S2/S3 object fixed point failed")
    record["selfhost_objects"] = generation_objects
    record["selfhost_executable_equal"] = sha256(stages[1]) == sha256(stages[2])
    if not record["selfhost_executable_equal"]:
        # Account for every differing byte: only the measured LC_UUID and
        # code-signature ranges may differ. Also require fixed-corpus IR.
        content = [executable_content(path) for path in stages[1:]]
        ir = []
        probe = work / "fixpoint.vyx"
        probe.write_text('fn main() -> i32 { print("hello"); return 0; }\n')
        for executable in stages[1:]:
            target = executable.parent / "fixpoint.ll"
            run([executable, "--src=file", probe, "--emit=ir", "-o", target], env=environment)
            ir.append(sha256(target))
        if content[0]["content_sha256"] != content[1]["content_sha256"] or ir[0] != ir[1]:
            raise RuntimeError("macOS S2/S3 executable content or fixed-corpus IR differs")
        record["selfhost_metadata_drift"] = {"reason": "LC_UUID / LC_CODE_SIGNATURE only",
                                             "executables": content, "fixed_corpus_ir_sha256": ir[0]}
        print("S2/S3 differs only in measured UUID/signature ranges; objects and fixed-corpus IR match", flush=True)
    built = stages[-1]
    record["hello_gate"] = hello_gate(built, environment, work)
    record["selfhost_compilers"] = {path.parent.name: sha256(path) for path in stages}
    for target in record["tools"]:
        run([built, "build", "--target", target, f"-j{args.jobs}"], cwd=build, env=environment)
    run([sys.executable, REPO / "probes/gates/dci-native-export/run.py", "--compiler", out / "boot",
         "--triplet", TRIPLES[arch]], env=environment)
    record["dci_native_export"] = json.loads((REPO / "probes/gates/dci-native-export/.cache/verification.json").read_text())
    from package_macos import package
    archive = package(build, llvm, args.output.resolve(), arch, environment, work)
    checksum_sidecar(archive)
    record.update(compiler_sha256=sha256(out / "boot"), backend_sha256=sha256(backend),
                  runtime_sha256=sha256(out / "libvyx_runtime.a"), archive_sha256=sha256(archive),
                  archive_bytes=archive.stat().st_size)
    (args.output / f"SDK-BUILD.macos-{arch}.json").write_text(json.dumps(record, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    cross = commands.add_parser("export")
    cross.add_argument("--sdk", type=Path)
    cross.add_argument("--bootstrap-tag", default="latest")
    mac = commands.add_parser("native")
    mac.add_argument("--llvm-root", type=Path, required=True)
    mac.add_argument("--cross-bundle", type=Path)
    mac.add_argument("--bootstrap-sdk", type=Path)
    mac.add_argument("--bootstrap-tag")
    for command in (cross, mac):
        command.add_argument("--arch", choices=TRIPLES, required=True)
        command.add_argument("--work", type=Path, required=True)
        command.add_argument("--output", type=Path, required=True)
        command.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    (export if args.command == "export" else native)(args)


if __name__ == "__main__":
    main()

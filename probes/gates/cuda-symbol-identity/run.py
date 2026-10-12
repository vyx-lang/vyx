#!/usr/bin/env python3
"""Check the CUDA project's source identity against PTX and its embedded host."""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def cuda_devices() -> dict:
    if os.name != "nt":
        return {"status": "skip", "reason": "the existing project uses Windows CUDA driver linkage"}
    try:
        driver = ctypes.WinDLL("nvcuda.dll")
    except OSError as error:
        return {"status": "skip", "reason": str(error)}
    driver.cuInit.argtypes = [ctypes.c_uint]
    driver.cuInit.restype = ctypes.c_int
    rc = driver.cuInit(0)
    if rc == 100:
        return {"status": "skip", "reason": "CUDA_ERROR_NO_DEVICE", "cuInit": rc}
    if rc != 0:
        return {"status": "fail", "reason": "CUDA driver initialization failed", "cuInit": rc}
    driver.cuDeviceGetCount.argtypes = [ctypes.POINTER(ctypes.c_int)]
    driver.cuDeviceGetCount.restype = ctypes.c_int
    count = ctypes.c_int()
    rc = driver.cuDeviceGetCount(ctypes.byref(count))
    if rc != 0:
        return {"status": "fail", "reason": "CUDA device enumeration failed", "exit": rc}
    if count.value == 0:
        return {"status": "skip", "reason": "no CUDA device"}
    return {"status": "ready", "count": count.value}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--arch", default="sm_120")
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    original = repo / "tests/projects/cuda_kernel"
    output = (args.output_dir or gate / ".cache" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")).resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    records = []
    gpu = cuda_devices()
    configured_toolkit = environment.get("VYX_CUDA_TOOLKIT_ROOT", "")
    toolkit = Path(configured_toolkit) if configured_toolkit else Path("__missing_cuda_toolkit__")
    if not configured_toolkit and os.name == "nt":
        toolkit_root = Path("C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA")
        candidates = [item for item in toolkit_root.glob("v*") if (item / "lib/x64/cuda.lib").is_file()]
        if candidates:
            toolkit = max(candidates, key=lambda item: tuple(int(part) for part in item.name[1:].split(".")))
    native_project_available = os.name == "nt" and (toolkit / "lib/x64/cuda.lib").is_file()
    if native_project_available:
        environment["VYX_CUDA_TOOLKIT_ROOT"] = str(toolkit)

    def run(name: str, command: list[str], cwd: Path) -> bool:
        try:
            result = subprocess.run(command, cwd=cwd, env=environment, capture_output=True, timeout=120)
            record = {"name": name, "command": command, "cwd": str(cwd), "exit": result.returncode,
                      "stdout": result.stdout.decode(errors="replace"),
                      "stderr": result.stderr.decode(errors="replace"), "pass": result.returncode == 0}
        except subprocess.TimeoutExpired:
            record = {"name": name, "command": command, "cwd": str(cwd), "pass": False, "timeout": True}
        records.append(record)
        print(f"{name}: {'PASS' if record['pass'] else 'FAIL'}", flush=True)
        if not record["pass"]:
            print(json.dumps(record, ensure_ascii=False), flush=True)
        return record["pass"]

    def check(name: str, passed: bool, **facts) -> None:
        records.append({"name": name, "pass": passed, **facts})
        print(f"{name}: {'PASS' if passed else 'FAIL'}", flush=True)

    variants = {
        "plain": "",
        "module": "module gpu.compute;\n",
        "versioned-module": '@[version("2.0.0")]\n@[variant("device")]\nmodule gpu.compute;\n',
    }
    for name, prefix in variants.items():
        project = output / name
        (project / "src").mkdir(parents=True)
        (project / "device").mkdir()
        (project / "src/host.vyx").write_bytes((original / "src/host.vyx").read_bytes())
        source = project / "device/kernel.vyx"
        source.write_text(prefix + (original / "device/kernel.vyx").read_text(encoding="utf-8"), encoding="utf-8")
        manifest = (original / "Vyx.toml").read_text(encoding="utf-8")
        manifest = manifest.replace('cuda_arch = "sm_120"', f'cuda_arch = "{args.arch}"')
        (project / "Vyx.toml").write_text(manifest, encoding="utf-8")
        ptx = project / "direct.ptx"
        if run(name + "-nvptx", [str(compiler), "--src=file", str(source), "--emit=obj", "-o", str(ptx),
                                "--target=nvptx64-nvidia-cuda", "--target-cpu", args.arch,
                                "--export-root-names", "scale_kernel"], repo):
            text = ptx.read_text(encoding="utf-8")
            entries = re.findall(r"(?m)^\.visible \.entry\s+([A-Za-z0-9_]+)\(", text)
            check(name + "-direct-entry", len(entries) == 1, entries=entries)
        if not native_project_available:
            records.append({"name": name + "-project", "skip": True, "exit": 77,
                            "reason": "Windows CUDA toolkit with cuda.lib is required"})
            continue
        if not run(name + "-project-build", [str(compiler), "build", "-j2"], project):
            continue
        cached_ptx = project / ".cache/cuda_cuda_kernel.ptx"
        embed = project / ".cache/cuda_cuda_kernel_embed.vyx"
        entries = re.findall(r"(?m)^\.visible \.entry\s+([A-Za-z0-9_]+)\(", cached_ptx.read_text(encoding="utf-8"))
        embedded = re.search(r'vyx_cuda_kernel_scale_kernel_symbol\(\) -> string \{ return "([A-Za-z0-9_]+)";',
                             embed.read_text(encoding="utf-8"))
        check(name + "-embedded-identity", len(entries) == 1 and embedded is not None
              and entries[0] == embedded.group(1), entries=entries,
              embedded_symbol=embedded.group(1) if embedded else "")
        ptxas = toolkit / "bin/ptxas.exe"
        if ptxas.is_file():
            run(name + "-ptxas", [str(ptxas), f"-arch={args.arch}", "-o", str(project / "device.cubin"),
                                 str(cached_ptx)], project)
        else:
            records.append({"name": name + "-ptxas", "skip": True, "exit": 77, "reason": "ptxas not found"})
        if gpu["status"] == "ready":
            if run(name + "-gpu", [str(project / "target/cuda_kernel.exe")], project):
                record = records[-1]
                record["pass"] = "cuda_kernel: OK" in record["stdout"]
                if not record["pass"]:
                    print(name + "-gpu: FAIL (success marker missing)", flush=True)
        else:
            records.append({"name": name + "-gpu", "skip": gpu["status"] == "skip",
                            "exit": 77 if gpu["status"] == "skip" else 1,
                            "pass": gpu["status"] != "fail", **gpu})
    failed = any(not record.get("pass", record.get("skip", False)) for record in records)
    skipped = any(record.get("skip", False) for record in records)
    passed = not failed and not skipped
    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "gpu": gpu, "arch": args.arch, "records": records, "passed": passed, "skipped": skipped,
              "runtime_artifacts": [{"path": str(file), "sha256": hashlib.sha256(file.read_bytes()).hexdigest()}
                                    for name in ("vyx_compiler_backend.dll", "vyx_runtime.lib")
                                    if (file := compiler.parent / name).is_file()]}
    (output / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("report:", output / "verification.json", flush=True)
    return 1 if failed else 77 if skipped else 0


if __name__ == "__main__":
    raise SystemExit(main())

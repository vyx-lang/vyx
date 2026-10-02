#!/usr/bin/env python3
"""Generate meaningless multi-module corpora and cold-compile them."""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT_DIR = Path(__file__).resolve().parent / "_synth_giant"
REPO_CLANG = ROOT / "clang" / "bin"
BOOT_DIR = ROOT / "bootstrap_compiler" / "out"


def pad(i: int, n: int) -> str:
    return f"{i:0{n}d}"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def gen_all(files: int, funcs: int) -> None:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    w = len(str(files - 1))
    fw = len(str(funcs - 1))
    cpp_src = OUT_DIR / "cpp" / "src"
    c_src = OUT_DIR / "c" / "src"
    rs_src = OUT_DIR / "rust" / "src"
    vyx_src = OUT_DIR / "vyx" / "src"
    mods = []
    for fi in range(files):
        tag = pad(fi, w)
        mods.append(tag)
        cpp_lines = [f"int m{tag}_sink = 0;", ""]
        c_lines = [f"int m{tag}_sink = 0;", ""]
        rs_lines = ["#![allow(dead_code)]", ""]
        vyx_lines = [f"module synth.m{tag};", ""]
        for fn in range(funcs):
            name = f"w{pad(fn, fw)}"
            k = fi * 131 + fn
            cpp_lines.append(
                f"int m{tag}_{name}(int x) {{ m{tag}_sink += x; return x * 3 + {k}; }}"
            )
            c_lines.append(
                f"int m{tag}_{name}(int x) {{ m{tag}_sink += x; return x * 3 + {k}; }}"
            )
            rs_lines.append(
                f"pub fn {name}(x: i32) -> i32 {{ x.wrapping_mul(3).wrapping_add({k}) }}"
            )
            vyx_lines.append(
                f"public fn {name}(x: i32) -> i32 {{ return x * 3 + {k}; }}"
            )
        cpp_lines.append("")
        cpp_lines.append(f"int m{tag}_entry(int x) {{ return m{tag}_w{pad(0, fw)}(x); }}")
        c_lines.append("")
        c_lines.append(f"int m{tag}_entry(int x) {{ return m{tag}_w{pad(0, fw)}(x); }}")
        rs_lines.append("")
        rs_lines.append(
            f"pub fn entry(x: i32) -> i32 {{ w{pad(0, fw)}(x) }}"
        )
        vyx_lines.append("")
        vyx_lines.append(
            f"public fn entry(x: i32) -> i32 {{ return w{pad(0, fw)}(x); }}"
        )
        write(cpp_src / f"m{tag}.cpp", "\n".join(cpp_lines) + "\n")
        write(c_src / f"m{tag}.c", "\n".join(c_lines) + "\n")
        write(rs_src / f"m{tag}.rs", "\n".join(rs_lines) + "\n")
        write(vyx_src / f"m{tag}.vyx", "\n".join(vyx_lines) + "\n")

    rs_lib = ["#![allow(dead_code)]", ""]
    sources = []
    for tag in mods:
        rs_lib.append(f"pub mod m{tag};")
        sources.append(f'    "src/m{tag}.vyx",')
    rs_lib.append("")
    rs_lib.append("pub fn entry(x: i32) -> i32 {")
    rs_lib.append(f"    m{mods[0]}::entry(x)")
    rs_lib.append("}")
    write(rs_src / "lib.rs", "\n".join(rs_lib) + "\n")
    write(vyx_src / "main.vyx", "module synth;\n\npublic fn main() -> i32 {\n    return 0;\n}\n")
    toml = f"""[package]
name = "synth"
version = "0.1.0"
entry = "src/main.vyx"

[build]
cache_dir = ".cache"
output_dir = "out"
threads = 0
vyxflags = ["-O2", "--export-all"]

[target.synth]
type = "static"
entry = "src/main.vyx"
auto_sources = false
parallel_vyx = true
sources = [
    "src/main.vyx",
{chr(10).join(sources)}
]
"""
    write(OUT_DIR / "vyx" / "Vyx.toml", toml)
    meta = f"files={files}\nfuncs={funcs}\ntotal_fns={files * funcs}\n"
    write(OUT_DIR / "meta.txt", meta)
    print(f"generated {OUT_DIR} files={files} funcs/file={funcs} total_fns={files * funcs}")


def run(cmd: list[str], cwd: Path | None = None, env: dict | None = None) -> None:
    proc = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if proc.returncode != 0:
        tail = "\n".join((proc.stdout or "").splitlines()[-40:])
        raise RuntimeError(f"exit {proc.returncode}: {' '.join(cmd)}\n{tail}")


def timed(label: str, fn) -> float:
    t0 = time.perf_counter()
    fn()
    dt = time.perf_counter() - t0
    print(f"{label:42s} {dt:8.3f} s")
    sys.stdout.flush()
    return dt


def parallel_compile(cmds: list[list[str]], jobs: int) -> None:
    errors: list[str] = []

    def one(cmd: list[str]) -> None:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0:
            errors.append(f"{' '.join(cmd)}\n{(proc.stdout or '')[-2000:]}")

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futs = [pool.submit(one, cmd) for cmd in cmds]
        for fut in as_completed(futs):
            fut.result()
    if errors:
        raise RuntimeError("compile failed:\n" + errors[0])


def bench(files: int, funcs: int, jobs: int) -> None:
    if not (OUT_DIR / "meta.txt").exists():
        gen_all(files, funcs)
    clang = REPO_CLANG / ("clang.exe" if os.name == "nt" else "clang")
    clangxx = REPO_CLANG / ("clang++.exe" if os.name == "nt" else "clang++")
    rustc = shutil.which("rustc")
    zig = shutil.which("zig")
    boot = BOOT_DIR / ("boot.exe" if os.name == "nt" else "boot")
    seven = BOOT_DIR / ("7.exe" if os.name == "nt" else "7")
    env = os.environ.copy()
    env["LLVM_ROOT"] = str(ROOT / "clang")
    env["PATH"] = str(REPO_CLANG) + os.pathsep + env.get("PATH", "")

    cpp_files = sorted((OUT_DIR / "cpp" / "src").glob("*.cpp"))
    c_files = sorted((OUT_DIR / "c" / "src").glob("*.c"))
    cpp_odir = OUT_DIR / "cpp" / "obj"
    c_odir = OUT_DIR / "c" / "obj"
    rs_odir = OUT_DIR / "rust" / "out"
    print(f"corpus files={len(cpp_files)} funcs/file={funcs} jobs={jobs} opt=-O2")
    print("---")

    def clangxx_job() -> None:
        if cpp_odir.exists():
            shutil.rmtree(cpp_odir)
        cpp_odir.mkdir(parents=True)
        cmds = [
            [str(clangxx), "-c", "-O2", "-std=c++17", str(p), "-o", str(cpp_odir / (p.stem + ".o"))]
            for p in cpp_files
        ]
        parallel_compile(cmds, jobs)

    def zigcc_job() -> None:
        if c_odir.exists():
            shutil.rmtree(c_odir)
        c_odir.mkdir(parents=True)
        cmds = [
            [zig, "cc", "-c", "-O2", str(p), "-o", str(c_odir / (p.stem + ".o"))]
            for p in c_files
        ]
        parallel_compile(cmds, jobs)

    def rustc_job() -> None:
        if rs_odir.exists():
            shutil.rmtree(rs_odir)
        rs_odir.mkdir(parents=True)
        run(
            [
                rustc,
                str(OUT_DIR / "rust" / "src" / "lib.rs"),
                "--edition",
                "2021",
                "--crate-type",
                "rlib",
                "-C",
                "opt-level=2",
                "-C",
                f"codegen-units={jobs}",
                "--out-dir",
                str(rs_odir),
            ]
        )

    def vyx_job(exe: Path, label: str) -> None:
        vyx_root = OUT_DIR / "vyx"
        cache = vyx_root / ".cache"
        out = vyx_root / "out"
        if cache.exists():
            shutil.rmtree(cache)
        if out.exists():
            shutil.rmtree(out)
        run([str(exe), "build", "--target", "synth", f"-j{jobs}"], cwd=vyx_root, env=env)
        objs = list(cache.glob("*.obj")) if cache.exists() else []
        list_files = list(cache.glob("*.cgu.list")) if cache.exists() else []
        if list_files:
            listed = []
            for lp in list_files:
                for line in lp.read_text(encoding="utf-8").splitlines():
                    raw = line.strip()
                    if not raw:
                        continue
                    p = Path(raw)
                    if not p.is_absolute():
                        p = (vyx_root / p).resolve()
                    listed.append(p)
            objs = [p for p in listed if p.exists()]
        nbytes = sum(p.stat().st_size for p in objs)
        print(f"  {label} objs={len(objs)} bytes={nbytes}")
        if label == "boot":
            unit = cache / "unit_crate_synth.sources"
            nsrc = 0
            if unit.exists():
                nsrc = sum(1 for line in unit.read_text(encoding="utf-8").splitlines() if line.strip())
            if not (1 <= len(objs) <= 16):
                raise RuntimeError(f"boot crate model: expected 1..=16 objs, got {len(objs)}")
            if nsrc != files + 1:
                raise RuntimeError(
                    f"boot crate model: expected {files + 1} unit sources, got {nsrc}"
                )
            min_bytes = max(2000, files * funcs * 40)
            if nbytes < min_bytes:
                raise RuntimeError(
                    f"boot crate model: obj too small ({nbytes} bytes); sibling bodies not lowered"
                )

    timed("clang++ -c -O2", clangxx_job)
    if zig:
        timed("zig cc -c -O2", zigcc_job)
    else:
        print(f"{'zig cc':42s} MISSING")
    if rustc:
        timed(f"rustc rlib -O2 cgu={jobs}", rustc_job)
    else:
        print(f"{'rustc':42s} MISSING")
    if seven.exists():
        timed("vyx out/7 -O2 static", lambda: vyx_job(seven, "7"))
    else:
        print(f"{'vyx out/7':42s} MISSING")
    if boot.exists():
        timed("vyx out/boot -O2 static", lambda: vyx_job(boot, "boot"))
    else:
        print(f"{'vyx out/boot':42s} MISSING")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", type=int, default=64)
    ap.add_argument("--funcs", type=int, default=48)
    ap.add_argument("--jobs", type=int, default=10)
    ap.add_argument("--gen-only", action="store_true")
    ap.add_argument("--bench-only", action="store_true")
    args = ap.parse_args()
    if not args.bench_only:
        gen_all(args.files, args.funcs)
    if not args.gen_only:
        bench(args.files, args.funcs, args.jobs)
    return 0


if __name__ == "__main__":
    sys.exit(main())

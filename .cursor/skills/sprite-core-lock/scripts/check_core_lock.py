#!/usr/bin/env python3
"""Assert packed 512 frames share one core height. Exit 0 prints CORE_LOCK OK."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# Import the same core_height used by the packer — do not reimplement.
_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))
from pack_locked_sprites import core_height  # noqa: E402


CLIP_GLOBS = {
    "idle": "idle/idle_*.png",
    "attack": "attack/atk_*.png",
    "skill": "skill/skill_*.png",
    "death": "death/death_*.png",
}


def measure(path: Path) -> float:
    return core_height(Image.open(path).convert("RGBA"))


def check_dir(frames: Path, target: float, max_delta: float, skill_jump_slack: float) -> int:
    if not frames.is_dir():
        print(f"FAIL: missing {frames}", file=sys.stderr)
        return 2
    failed = 0
    for name, glob in CLIP_GLOBS.items():
        paths = sorted(frames.glob(glob))
        if not paths:
            print(f"FAIL: no frames for {name} ({glob})", file=sys.stderr)
            failed += 1
            continue
        cores = [measure(p) for p in paths]
        lo, hi = min(cores), max(cores)
        delta = hi - lo
        mean = float(np.mean(cores))
        line = f"{name:8s} n={len(paths)} core={lo:.0f}-{hi:.0f} Δ={delta:.0f} mean={mean:.1f}"
        ok = True
        if name == "skill":
            # one jump frame may drop; the rest must sit on target
            below = [c for c in cores if c < target - max_delta]
            if len(below) > 1 or (below and min(below) < target - skill_jump_slack):
                ok = False
            if abs(mean - target) > 8:
                ok = False
        else:
            if delta > max_delta + 0.51:
                ok = False
            if abs(mean - target) > 3.0:
                ok = False
        print(f"{'OK  ' if ok else 'FAIL'} {line}")
        if not ok:
            failed += 1
            for p, c in zip(paths, cores):
                print(f"       {p.name}={c:.1f}")

    walk_root = frames / "walk"
    if walk_root.is_dir():
        for d in sorted(p.name for p in walk_root.iterdir() if p.is_dir()):
            paths = sorted((walk_root / d).glob(f"walk_{d}_*.png"))
            if not paths:
                print(f"FAIL: walk/{d} empty", file=sys.stderr)
                failed += 1
                continue
            cores = [measure(p) for p in paths]
            lo, hi = min(cores), max(cores)
            delta = hi - lo
            mean = float(np.mean(cores))
            ok = delta <= max_delta + 0.51 and abs(mean - target) <= 3.0
            print(f"{'OK  ' if ok else 'FAIL'} walk_{d:2s} n={len(paths)} core={lo:.0f}-{hi:.0f} Δ={delta:.0f} mean={mean:.1f}")
            if not ok:
                failed += 1
    else:
        print("FAIL: missing walk/", file=sys.stderr)
        failed += 1

    pngs = list(frames.rglob("*.png"))
    print(f"png_count={len(pngs)}")
    if failed:
        print("CORE_LOCK FAIL")
        return 1
    print("CORE_LOCK OK")
    return 0


def self_test() -> int:
    from pack_locked_sprites import pack_sequence, _draw_body
    import tempfile
    import shutil

    tmp = Path(tempfile.mkdtemp(prefix="corecheck_"))
    try:
        src = tmp / "src"
        src.mkdir()
        _draw_body(src / "a.png", 400)
        _draw_body(src / "b.png", 400)
        frames = pack_sequence([src / "a.png", src / "b.png"], target_core=380.0)
        out = tmp / "02_frames_512" / "idle"
        out.mkdir(parents=True)
        frames[0].save(out / "idle_00.png")
        frames[1].save(out / "idle_01.png")
        # minimal tree so check_dir can run walk-less? we require walk — seed one dir
        w = tmp / "02_frames_512" / "walk" / "s"
        w.mkdir(parents=True)
        frames[0].save(w / "walk_s_00.png")
        for clip, n, prefix in [("attack", 1, "atk"), ("skill", 1, "skill"), ("death", 1, "death")]:
            d = tmp / "02_frames_512" / clip
            d.mkdir()
            frames[0].save(d / f"{prefix}_00.png")
        return check_dir(tmp / "02_frames_512", 380.0, 4.0, 20.0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=Path, help="02_frames_512 directory")
    ap.add_argument("--target-core", type=float, default=380.0)
    ap.add_argument("--max-delta", type=float, default=4.0)
    ap.add_argument("--skill-jump-slack", type=float, default=20.0, help="allowed drop for one skill jump frame")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.frames:
        ap.error("--frames is required unless --self-test")
    return check_dir(args.frames, args.target_core, args.max_delta, args.skill_jump_slack)


if __name__ == "__main__":
    raise SystemExit(main())

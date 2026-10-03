#!/usr/bin/env python3
"""Reject damaged redraws before packing: low sat, pink lineart, whiteout, silhouette, near-dupes."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))
from pack_locked_sprites import CLIPS, WALK_DRAW, key_magenta  # noqa: E402


def diagnose(im: Image.Image) -> list[str]:
    a = np.array(im.convert("RGBA"))
    alpha = a[:, :, 3] > 16
    reasons: list[str] = []
    if int(alpha.sum()) < 200:
        return ["empty_after_key"]
    rgb = a[:, :, :3]
    hsv = np.array(Image.fromarray(rgb, "RGB").convert("HSV"))
    sat = hsv[:, :, 1][alpha].astype(np.float32)
    mean_sat = float(sat.mean())
    if mean_sat < 28.0:
        reasons.append(f"low_saturation mean={mean_sat:.1f}")
    r = rgb[:, :, 0].astype(np.int16)
    g = rgb[:, :, 1].astype(np.int16)
    b = rgb[:, :, 2].astype(np.int16)
    fg_r, fg_g, fg_b = r[alpha], g[alpha], b[alpha]
    n = float(alpha.sum())
    pink = float(((fg_r > 180) & (fg_b > 140) & (fg_g < 140)).sum()) / n
    if pink > 0.18:
        reasons.append(f"pink_lineart {pink:.2f}")
    white = float(((fg_r > 240) & (fg_g > 240) & (fg_b > 240)).sum()) / n
    if white > 0.45:
        reasons.append(f"whiteout {white:.2f}")
    dark = float(((fg_r + fg_g + fg_b) < 80).sum()) / n
    if dark > 0.70:
        reasons.append(f"silhouette {dark:.2f}")
    return reasons


def near_dupes(images: list[Image.Image], names: list[str]) -> list[str]:
    hits: list[str] = []
    if len(images) < 2:
        return hits
    for i in range(len(images) - 1):
        a = images[i].convert("RGB").resize((64, 64), Image.Resampling.BILINEAR)
        b = images[i + 1].convert("RGB").resize((64, 64), Image.Resampling.BILINEAR)
        diff = ImageChops.difference(a, b)
        arr = np.array(diff).astype(np.float32)
        mean = float(arr.mean())
        if mean < 2.4:
            hits.append(f"near_duplicate {names[i]} ~ {names[i + 1]} mean_diff={mean:.2f}")
    return hits


def collect(src: Path, prefix: str) -> list[Path]:
    paths: list[Path] = []
    for spec in CLIPS.values():
        paths.extend(src / spec["pattern"].format(prefix=prefix, i=i) for i in range(spec["n"]))
    for d in WALK_DRAW:
        paths.extend(src / f"{prefix}_walk_{d}_{i:02d}.png" for i in range(4))
    return paths


def check_src(src: Path, prefix: str) -> int:
    paths = [p for p in collect(src, prefix) if p.is_file()]
    if not paths:
        print(f"FAIL: no {prefix}_*.png under {src}", file=sys.stderr)
        return 2
    failed = 0
    by_clip: dict[str, list[tuple[str, Image.Image]]] = {}
    for p in paths:
        keyed = key_magenta(p)
        reasons = diagnose(keyed)
        clip = p.stem.split("_")[1] if "_" in p.stem else "misc"
        # redraw_walk_s_00 -> walk
        parts = p.stem.split("_")
        clip = parts[1] if len(parts) > 1 else "misc"
        by_clip.setdefault(clip, []).append((p.name, keyed))
        if reasons:
            failed += 1
            print(f"FAIL {p.name}: {', '.join(reasons)}")
        else:
            print(f"OK   {p.name}")
    for clip, items in by_clip.items():
        if clip in {"idle", "walk", "atk", "skill", "death"} and len(items) >= 2:
            for msg in near_dupes([im for _, im in items], [n for n, _ in items]):
                print(f"FAIL {clip}: {msg}")
                failed += 1
    if failed:
        print("FRAME_QUALITY FAIL")
        return 1
    print("FRAME_QUALITY OK")
    return 0


def self_test() -> int:
    import tempfile
    import shutil
    from pack_locked_sprites import _draw_body

    tmp = Path(tempfile.mkdtemp(prefix="fq_"))
    try:
        good = tmp / "good.png"
        _draw_body(good, 400, vfx=False)
        reasons = diagnose(key_magenta(good))
        if reasons:
            print(f"FAIL: good body flagged {reasons}", file=sys.stderr)
            return 1
        bad = Image.new("RGB", (200, 200), (255, 0, 255))
        for y in range(40, 160):
            for x in range(60, 140):
                bad.putpixel((x, y), (20, 20, 20))
        bp = tmp / "bad.png"
        bad.save(bp)
        reasons = diagnose(key_magenta(bp))
        if not any("silhouette" in r or "low_saturation" in r for r in reasons):
            print(f"FAIL: silhouette not caught {reasons}", file=sys.stderr)
            return 1
        print("FRAME_QUALITY_SELFTEST OK")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path)
    ap.add_argument("--prefix", default="redraw")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.src:
        ap.error("--src is required unless --self-test")
    return check_src(args.src, args.prefix)


if __name__ == "__main__":
    raise SystemExit(main())

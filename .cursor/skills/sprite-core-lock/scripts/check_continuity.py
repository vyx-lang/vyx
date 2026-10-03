#!/usr/bin/env python3
"""Adjacent-frame silhouette IoU + core-height jump. Exit 0 prints CONTINUITY OK."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))
from pack_locked_sprites import core_height  # noqa: E402


def _key_arr(im: Image.Image) -> np.ndarray:
    rgb = np.array(im.convert("RGB"))
    r, g, b = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]
    mag = (r > 180) & (b > 180) & (g < 125)
    return ~mag


def as_rgba(im: Image.Image) -> Image.Image:
    if im.mode == "RGBA":
        return im
    rgb = np.array(im.convert("RGB"))
    a = (_key_arr(im).astype(np.uint8)) * 255
    return Image.fromarray(np.dstack([rgb, a]), "RGBA")


def iou(a: np.ndarray, b: np.ndarray) -> float:
    inter = np.logical_and(a, b).sum()
    union = np.logical_or(a, b).sum()
    return float(inter) / float(union) if union else 1.0


def check_clip(paths: list[Path], min_iou: float, max_core_delta: float) -> list[str]:
    fails: list[str] = []
    masks = []
    cores = []
    for p in paths:
        im = Image.open(p)
        rgba = as_rgba(im)
        masks.append(np.array(rgba)[:, :, 3] > 16)
        cores.append(core_height(rgba))
    for i in range(len(masks) - 1):
        v = iou(masks[i], masks[i + 1])
        if v < min_iou:
            fails.append(f"{paths[i].name}->{paths[i+1].name} iou={v:.2f}")
    if cores and (max(cores) - min(cores) > max_core_delta):
        fails.append(f"core Δ={max(cores)-min(cores):.1f} ({min(cores):.0f}-{max(cores):.0f})")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=Path, help="directory of clip subfolders or a single clip folder")
    ap.add_argument("--min-iou", type=float, default=0.42)
    ap.add_argument("--max-core-delta", type=float, default=8.0)
    args = ap.parse_args()
    if not args.frames or not args.frames.is_dir():
        ap.error("--frames dir required")
    failed = 0
    # clip folders or flat pngs
    sub = [p for p in sorted(args.frames.iterdir()) if p.is_dir()]
    groups = sub if sub else [args.frames]
    for g in groups:
        pngs = sorted(g.glob("*.png"))
        if len(pngs) < 2:
            continue
        # death may crumple; allow lower iou
        min_iou = 0.22 if g.name == "death" else args.min_iou
        hits = check_clip(pngs, min_iou, args.max_core_delta)
        if hits:
            failed += 1
            print(f"FAIL {g.name}: {'; '.join(hits)}")
        else:
            print(f"OK   {g.name} n={len(pngs)}")
    if failed:
        print("CONTINUITY FAIL")
        return 1
    print("CONTINUITY OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

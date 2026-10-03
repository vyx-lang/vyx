#!/usr/bin/env python3
"""Slice a 1024x1024 image-model sheet into 8x8 cells of 128x128."""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

from PIL import Image

CELL = 128
CANVAS = 1024


def cell(im: Image.Image, c: int, r: int) -> Image.Image:
    x, y = c * CELL, r * CELL
    return im.crop((x, y, x + CELL, y + CELL))


def gif(frames: list[Image.Image], path: Path, duration: int, loop: int) -> None:
    bg = (22, 26, 36)
    rgb = []
    for fr in frames:
        base = Image.new("RGB", (CELL, CELL), bg)
        base.paste(fr.convert("RGB"))
        rgb.append(base.convert("P", palette=Image.Palette.ADAPTIVE, colors=80))
    path.parent.mkdir(parents=True, exist_ok=True)
    rgb[0].save(path, save_all=True, append_images=rgb[1:], duration=duration, loop=loop, disposal=2)


def strip(frames: list[Image.Image], path: Path) -> None:
    s = Image.new("RGB", (CELL * len(frames), CELL), (255, 0, 255))
    for i, fr in enumerate(frames):
        s.paste(fr, (i * CELL, 0))
    s.save(path)


def slice_sheet(sheet: Path, out: Path) -> dict[str, int]:
    im = Image.open(sheet).convert("RGB")
    if im.size != (CANVAS, CANVAS):
        raise SystemExit(f"FAIL: expected {CANVAS}x{CANVAS}, got {im.size}")
    if out.exists():
        shutil.rmtree(out)
    frames_dir = out / "frames"
    gif_dir = out / "gif"
    frames_dir.mkdir(parents=True)
    gif_dir.mkdir()

    clips: dict[str, list[Image.Image]] = {
        "idle": [cell(im, c, 0) for c in range(4)],
        "hit": [cell(im, c, 0) for c in range(4, 8)],
        "attack": [cell(im, c, 1) for c in range(8)],
        "death": [cell(im, c, 2) for c in range(8)],
        "walk_s": [cell(im, c, 3) for c in range(8)],
        "walk_se": [cell(im, c, 4) for c in range(8)],
        "walk_e": [cell(im, c, 5) for c in range(8)],
        "walk_ne": [cell(im, c, 6) for c in range(8)],
        "walk_n": [cell(im, c, 7) for c in range(8)],
    }
    clips["walk_w"] = [fr.transpose(Image.FLIP_LEFT_RIGHT) for fr in clips["walk_e"]]
    clips["walk_sw"] = [fr.transpose(Image.FLIP_LEFT_RIGHT) for fr in clips["walk_se"]]
    clips["walk_nw"] = [fr.transpose(Image.FLIP_LEFT_RIGHT) for fr in clips["walk_ne"]]

    for name, frames in clips.items():
        d = frames_dir / name
        d.mkdir()
        prefix = {"attack": "atk", "hit": "hit"}.get(name, name)
        for i, fr in enumerate(frames):
            fr.save(d / f"{prefix}_{i:02d}.png")
        strip(frames, out / f"strip_{name}.png")

    gif(clips["idle"], gif_dir / "idle.gif", 160, 0)
    gif(clips["hit"], gif_dir / "hit.gif", 100, 0)
    gif(clips["attack"], gif_dir / "attack.gif", 90, 0)
    gif(clips["death"], gif_dir / "death.gif", 140, 1)
    for d in ("walk_s", "walk_se", "walk_e", "walk_ne", "walk_n", "walk_w", "walk_sw", "walk_nw"):
        gif(clips[d], gif_dir / f"{d}.gif", 110, 0)

    counts = {k: len(v) for k, v in clips.items()}
    print("SLICED", counts)
    return counts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=Path("/tmp/mannequin_ai"))
    args = ap.parse_args()
    slice_sheet(args.sheet, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

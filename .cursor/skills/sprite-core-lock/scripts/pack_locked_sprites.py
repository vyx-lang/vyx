#!/usr/bin/env python3
"""Lock redrawn poses into 512x512 frames.

No plate+VFX composite. No idle scale-breathe. No face-detect refine.
Scale is only TARGET_CORE / core_height. Effects clip; the body does not shrink.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

CANVAS = 512
PAD = 28
PIVOT_X = 256.0
GROUND_Y = 452.0
TARGET_CORE = 380.0
DIR_ORDER = ["s", "sw", "w", "nw", "n", "ne", "e", "se"]
WALK_DRAW = ["s", "w", "n", "se", "ne"]
WALK_MIRROR = {"e": "w", "sw": "se", "nw": "ne"}

CLIPS = {
    "idle": {"pattern": "{prefix}_idle_{i:02d}.png", "n": 4, "loop": True},
    "attack": {"pattern": "{prefix}_atk_{i:02d}.png", "n": 8, "loop": False},
    "skill": {"pattern": "{prefix}_skill_{i:02d}.png", "n": 10, "loop": False},
    "death": {"pattern": "{prefix}_death_{i:02d}.png", "n": 4, "loop": False},
}


def key_magenta(path: Path) -> Image.Image:
    rgb = np.array(Image.open(path).convert("RGB"), dtype=np.uint8)
    f = rgb.astype(np.float32)
    r, g, b = f[:, :, 0], f[:, :, 1], f[:, :, 2]
    dist = np.sqrt((r - 255.0) ** 2 + (g - 0.0) ** 2 + (b - 255.0) ** 2)
    mag = ((r > 180) & (b > 180) & (g < 125)) | (dist < 95)
    hard = (~mag).astype(np.uint8)
    alpha = np.clip((95.0 - dist) * (255.0 / 28.0), 0, 255).astype(np.uint8)
    alpha[hard == 0] = 0
    alpha[hard == 1] = np.maximum(alpha[hard == 1], 210)
    out = rgb.copy()
    spill = (hard == 1) & (r > g + 40) & (b > g + 40)
    out[:, :, 0] = np.where(spill, np.minimum(out[:, :, 0], (g + 36).astype(np.uint8)), out[:, :, 0])
    out[:, :, 2] = np.where(spill, np.minimum(out[:, :, 2], (g + 36).astype(np.uint8)), out[:, :, 2])
    return Image.fromarray(np.dstack([out, alpha]), "RGBA")


def bbox(im: Image.Image, a_min: int = 16) -> tuple[int, int, int, int]:
    a = np.array(im)[:, :, 3]
    ys, xs = np.where(a > a_min)
    if len(xs) == 0:
        return (0, 0, im.width, im.height)
    return int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1


def vfx_mask(a: np.ndarray) -> np.ndarray:
    r, g, b = a[:, :, 0].astype(np.int16), a[:, :, 1].astype(np.int16), a[:, :, 2].astype(np.int16)
    return (g > r + 20) & (b > r + 8) & (g > 130)


def body_mask(im: Image.Image) -> np.ndarray:
    a = np.array(im.convert("RGBA"))
    fg = a[:, :, 3] > 16
    body = fg & (~vfx_mask(a))
    return body if body.sum() > 30 else fg


def core_centroid(im: Image.Image) -> tuple[float, float]:
    a = np.array(im.convert("RGBA"))
    alpha = a[:, :, 3]
    h, w = alpha.shape
    yy, xx = np.ogrid[:h, :w]
    hard = alpha > 90
    core = hard & (~vfx_mask(a)) & (np.sqrt((yy - h * 0.50) ** 2 + (xx - w * 0.50) ** 2) < min(h, w) * 0.38)
    if core.sum() < 40:
        core = hard & (~vfx_mask(a))
    if core.sum() < 40:
        core = hard
    ys, xs = np.where(core)
    if len(xs) == 0:
        return w / 2.0, h / 2.0
    wts = alpha[ys, xs].astype(np.float32)
    return float(np.average(xs, weights=wts)), float(np.average(ys, weights=wts))


def feet_xy(im: Image.Image) -> tuple[float, float]:
    m = body_mask(im)
    ys, xs = np.where(m)
    y0, y1 = int(ys.min()), int(ys.max())
    low = ys > (y0 + 0.78 * (y1 - y0))
    if low.sum() < 8:
        low = ys > (y0 + 0.65 * (y1 - y0))
    return float(np.mean(xs[low])), float(y1)


def core_height(im: Image.Image) -> float:
    m = body_mask(im)
    ys, xs = np.where(m)
    if len(ys) < 8:
        return 8.0
    y0, y1 = int(ys.min()), int(ys.max())
    low = ys > y0 + 0.60 * (y1 - y0)
    cx = float(xs[low].mean()) if low.sum() > 10 else float(xs.mean())
    bw = max(10, int((xs.max() - xs.min()) * 0.13))
    x0 = max(0, int(cx - bw))
    x1 = min(m.shape[1], int(cx + bw))
    col = m[:, x0:x1].any(1)
    best = 0
    i = 0
    n = len(col)
    while i < n:
        if col[i]:
            j = i
            while j < n and col[j]:
                j += 1
            if j - i > best:
                best = j - i
            i = j
        else:
            i += 1
    if best < 180:
        return max(8.0, float(y1 - y0))
    return float(best)


def paste(src: Image.Image, dx: int, dy: int, canvas: int = CANVAS) -> Image.Image:
    out = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    out.paste(src, (dx, dy), src)
    return out


def place(rs: Image.Image, plant_feet: bool, pivot_x: float, ground_y: float, canvas: int) -> Image.Image:
    if plant_feet:
        fx, fy = feet_xy(rs)
        return paste(rs, int(round(pivot_x - fx)), int(round(ground_y - fy)), canvas)
    cx, cy = core_centroid(rs)
    return paste(rs, int(round(pivot_x - cx)), int(round(canvas / 2.0 - cy)), canvas)


def pack_sequence(
    paths: list[Path],
    target_core: float = TARGET_CORE,
    plant_feet: bool = True,
    pivot_x: float = PIVOT_X,
    ground_y: float = GROUND_Y,
    canvas: int = CANVAS,
) -> list[Image.Image]:
    keyed = [key_magenta(p) for p in paths]
    locked = []
    for k in keyed:
        ch = core_height(k)
        s = float(np.clip(target_core / max(ch, 1.0), 0.28, 1.20))
        nw = max(1, int(round(k.width * s)))
        nh = max(1, int(round(k.height * s)))
        rs = k.resize((nw, nh), Image.Resampling.LANCZOS)
        locked.append(place(rs, plant_feet, pivot_x, ground_y, canvas))
    fixed = []
    for im in locked:
        ch = core_height(im)
        if ch < target_core - 6:
            s = target_core / max(ch, 1.0)
            nw = max(1, int(round(im.width * s)))
            nh = max(1, int(round(im.height * s)))
            im = place(im.resize((nw, nh), Image.Resampling.LANCZOS), plant_feet, pivot_x, ground_y, canvas)
        fixed.append(im)
    return fixed


def save_seq(frames: list[Image.Image], folder: Path, prefix: str) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    for i, fr in enumerate(frames):
        if fr.size != (CANVAS, CANVAS):
            raise SystemExit(f"FAIL: {folder}/{prefix}_{i:02d} size={fr.size}")
        fr.save(folder / f"{prefix}_{i:02d}.png")


def make_gif(frames: list[Image.Image], path: Path, duration: int, loop: int) -> None:
    bg = (22, 26, 36)
    rgb = []
    for fr in frames:
        base = Image.new("RGB", (CANVAS, CANVAS), bg)
        base.paste(fr, mask=fr.split()[-1])
        rgb.append(base.convert("P", palette=Image.Palette.ADAPTIVE, colors=240))
    path.parent.mkdir(parents=True, exist_ok=True)
    rgb[0].save(
        path,
        save_all=True,
        append_images=rgb[1:],
        duration=duration,
        loop=loop,
        disposal=2,
        optimize=False,
    )


def drift(frames: list[Image.Image], name: str) -> dict:
    xs, ys, cores = [], [], []
    for im in frames:
        cx, cy = core_centroid(im)
        xs.append(cx)
        ys.append(cy)
        cores.append(core_height(im))
    info = {
        "n": len(frames),
        "cx_delta": float(max(xs) - min(xs)),
        "cy_delta": float(max(ys) - min(ys)),
        "core_min": float(min(cores)),
        "core_max": float(max(cores)),
        "core_delta": float(max(cores) - min(cores)),
    }
    print(
        f"{name:10s} dxy=({info['cx_delta']:.1f},{info['cy_delta']:.1f}) "
        f"core={info['core_min']:.0f}-{info['core_max']:.0f} Δ={info['core_delta']:.0f}"
    )
    return info


def require_paths(src: Path, prefix: str) -> dict[str, list[Path]]:
    missing: list[str] = []
    found: dict[str, list[Path]] = {}
    for name, spec in CLIPS.items():
        paths = [src / spec["pattern"].format(prefix=prefix, i=i) for i in range(spec["n"])]
        for p in paths:
            if not p.is_file():
                missing.append(str(p))
        found[name] = paths
    walk: dict[str, list[Path]] = {}
    for d in WALK_DRAW:
        paths = [src / f"{prefix}_walk_{d}_{i:02d}.png" for i in range(4)]
        for p in paths:
            if not p.is_file():
                missing.append(str(p))
        walk[d] = paths
    found["walk"] = walk  # type: ignore[assignment]
    if missing:
        print("FAIL: missing source frames:", file=sys.stderr)
        for m in missing:
            print(f"  {m}", file=sys.stderr)
        raise SystemExit(2)
    return found


def pack_pack(src: Path, out: Path, prefix: str, character: str, cg: list[Path]) -> None:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    frames_root = out / "02_frames_512"
    prev = out / "04_preview_gif"
    paths = require_paths(src, prefix)

    idle = pack_sequence(paths["idle"])
    attack = pack_sequence(paths["attack"])
    skill = pack_sequence(paths["skill"])
    death = pack_sequence(paths["death"])
    walk = {d: pack_sequence(paths["walk"][d]) for d in WALK_DRAW}  # type: ignore[index]
    for dest, src_dir in WALK_MIRROR.items():
        walk[dest] = [im.transpose(Image.FLIP_LEFT_RIGHT) for im in walk[src_dir]]

    save_seq(idle, frames_root / "idle", "idle")
    save_seq(attack, frames_root / "attack", "atk")
    save_seq(skill, frames_root / "skill", "skill")
    save_seq(death, frames_root / "death", "death")
    for d in DIR_ORDER:
        save_seq(walk[d], frames_root / "walk" / d, f"walk_{d}")

    make_gif(idle, prev / "idle.gif", 160, 0)
    make_gif(attack, prev / "attack.gif", 90, 0)
    make_gif(skill, prev / "skill.gif", 80, 0)
    make_gif(death, prev / "death.gif", 180, 1)
    for d in DIR_ORDER:
        make_gif(walk[d], prev / f"walk_{d}.gif", 110, 0)

    pos = {
        "nw": (0, 0), "n": (1, 0), "ne": (2, 0),
        "w": (0, 1), "e": (2, 1),
        "sw": (0, 2), "s": (1, 2), "se": (2, 2),
    }
    comp = []
    for fi in range(4):
        canvas = Image.new("RGB", (CANVAS * 3, CANVAS * 3), (22, 26, 36))
        for d, (cx, cy) in pos.items():
            canvas.paste(walk[d][fi], (cx * CANVAS, cy * CANVAS), walk[d][fi])
        comp.append(canvas)
    comp[0].save(
        prev / "walk_8dir_compass.gif",
        save_all=True,
        append_images=comp[1:],
        duration=110,
        loop=0,
        disposal=2,
    )

    report = {
        "idle": drift(idle, "idle"),
        "attack": drift(attack, "attack"),
        "skill": drift(skill, "skill"),
        "death": drift(death, "death"),
    }
    for d in DIR_ORDER:
        report[f"walk_{d}"] = drift(walk[d], f"walk_{d}")

    meta = {
        "character": character,
        "canvas": [CANVAS, CANVAS],
        "target_core": TARGET_CORE,
        "ground_y": GROUND_Y,
        "notes": (
            "Core height locked to 380px. Feet planted at y=452. "
            "VFX clips, never shrinks the character. No plate+VFX, no idle scale-breathe."
        ),
        "clips": {
            "idle": {"frames": 4, "loop": True},
            "walk": {"frames": 4, "loop": True, "dirs": DIR_ORDER},
            "attack": {"frames": 8, "loop": False},
            "skill": {"frames": 10, "loop": False},
            "death": {"frames": 4, "loop": False},
        },
        "report": report,
    }
    (frames_root / "atlas.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")

    cg_dir = out / "01_cg"
    cg_dir.mkdir()
    for p in cg:
        if p.is_file():
            shutil.copy2(p, cg_dir / p.name)

    sheets = out / "03_sheets_original"
    sheets.mkdir()
    samples = [
        src / f"{prefix}_idle_00.png",
        src / f"{prefix}_atk_03.png",
        src / f"{prefix}_skill_04.png",
    ]
    for p in samples:
        if p.is_file():
            shutil.copy2(p, sheets / p.name)

    (out / "README.txt").write_text(
        f"{character} · locked sprite pack\n"
        "====================\n\n"
        "01_cg/           splash + Q turnaround\n"
        "02_frames_512/   58 x 512x512 transparent PNG\n"
        "  idle/ 4 loop: weight/blink, not whole-image scale\n"
        "  walk/{s,sw,w,nw,n,ne,e,se}/ 4 loop each\n"
        "  attack/ 8: action+VFX on the same frame\n"
        "  skill/ 10: action+VFX on the same frame; overflow clipped\n"
        "  death/ 4 no-loop, hold last frame\n"
        "03_sheets_original/ design refs, not slice sources\n"
        "04_preview_gif/\n\n"
        f"Pivot x={int(PIVOT_X)}, feet y={int(GROUND_Y)}, target_core={int(TARGET_CORE)}.\n"
        "E/SW/NW mirrored from W/SE/NE.\n",
        encoding="utf-8",
    )
    print("PACKED", out)


def _draw_body(path: Path, body_h: int, vfx: bool = False) -> None:
    im = Image.new("RGB", (800, 800), (255, 0, 255))
    x0, y0 = 340, 200
    x1, y1 = 460, 200 + body_h
    for y in range(y0, y1):
        for x in range(x0, x1):
            im.putpixel((x, y), (210, 140, 80))
    if vfx:
        for y in range(40, 120):
            for x in range(200, 600):
                im.putpixel((x, y), (40, 210, 180))
    im.save(path)


def self_test() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="corelock_"))
    try:
        src = tmp / "src"
        src.mkdir()
        _draw_body(src / "body.png", 400, vfx=False)
        _draw_body(src / "body_vfx.png", 400, vfx=True)
        a = pack_sequence([src / "body.png"], target_core=380.0)[0]
        b = pack_sequence([src / "body_vfx.png"], target_core=380.0)[0]
        ha, hb = core_height(a), core_height(b)
        print(f"self-test packed core body={ha:.1f} body+vfx={hb:.1f}")
        if abs(ha - 380.0) > 3.0:
            print(f"FAIL: packed core {ha:.1f} != 380", file=sys.stderr)
            return 1
        if abs(hb - ha) > 4.0:
            print(f"FAIL: vfx changed core {ha:.1f} -> {hb:.1f}", file=sys.stderr)
            return 1
        if a.size != (512, 512) or b.size != (512, 512):
            print("FAIL: canvas", file=sys.stderr)
            return 1
        print("PACK_SELFTEST OK")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack redrawn magenta-key poses into locked 512 frames.")
    ap.add_argument("--src", type=Path, help="Directory of redraw_*.png")
    ap.add_argument("--out", type=Path, help="Output pack root")
    ap.add_argument("--prefix", default="redraw")
    ap.add_argument("--character", default="character")
    ap.add_argument("--cg", nargs="*", default=[], help="Optional splash/turnaround paths to copy into 01_cg/")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.src or not args.out:
        ap.error("--src and --out are required unless --self-test")
    pack_pack(args.src, args.out, args.prefix, args.character, [Path(p) for p in args.cg])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

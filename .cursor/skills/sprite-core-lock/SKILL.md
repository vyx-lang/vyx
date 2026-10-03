---
name: sprite-core-lock
description: >-
  把原画装进锁定尺寸的序列帧画布。Use when packing gacha/chibi frames into 512x512
  transparent PNGs, locking character core height, planting feet, removing
  magenta #FF00FF, or the user says 锁身高、脚钉、禁止根运动、人物大小不能变.
  Do not generate new art here (that is q-gacha-character-sprites). Do not
  commit the packed PNG into a compiler repo (that is asset-zip-delivery).
---

# 序列帧装箱：锁核心身高，禁止后期「看起来齐」

本 skill 只做 **品红原画 → 512×512 透明 PNG**。画帧走 [`q-gacha-character-sprites`](../q-gacha-character-sprites/SKILL.md)。送走走 [`asset-zip-delivery`](../asset-zip-delivery/SKILL.md)。

## 何时启用

- 手上已经有 `redraw_*.png`（或同名规范），要切成游戏能用的序列
- 用户说人物大小飘、特效把人缩小、idle 在呼吸缩放、脚在滑
- 复检已装箱目录的核心身高

## 死数字（实测可用，不要改到「看起来更满」）

| 常量 | 值 | 含义 |
|---|---|---|
| `CANVAS` | 512 | 输出边长 |
| `PAD` | 28 | 观察用边距，**不是** contain 缩放依据 |
| `PIVOT_X` | 256 | 水平枢轴 |
| `GROUND_Y` | 452 | 脚底钉在这条线上 |
| `TARGET_CORE` | 380 | 身体中轴最长实心 run 的目标像素身高 |
| 抠图底 | `#FF00FF` | 只要品红。海军蓝会吃金棕头发 |

缩放公式只有这一条：

```
scale = TARGET_CORE / core_height
```

`core_height` = 身体中轴带（约宽度 13%）上、排除青绿 VFX 之后，**最长连续实心 run**。不是整图 bbox，不是含特效的外接框，不是脸高。

禁止再乘第二刀：

- ❌ 因特效 / 宽度 `contain` 进 512 再缩小角色
- ❌ 脸检测 / 二次 refine（不稳定，会把单帧爆缩到铺满画布）
- ❌ idle 整图 `resize` 当呼吸
- ❌ 静态底板 + 特效层 `composite`

脚钉：`paste(x = PIVOT_X - foot_x, y = GROUND_Y - foot_y)`。特效越界直接裁。

Q 版身子偏瘦、左右空，是正确的。不要非均匀拉伸填满。

## 死命令

依赖：`python3` + `Pillow` + `numpy`。

```bash
python3 .cursor/skills/sprite-core-lock/scripts/pack_locked_sprites.py \
  --src /path/to/redraw_dir \
  --out /tmp/char_pack \
  --prefix redraw \
  --character "角色名"

python3 .cursor/skills/sprite-core-lock/scripts/check_core_lock.py \
  --frames /tmp/char_pack/02_frames_512 \
  --target-core 380 \
  --max-delta 4

python3 .cursor/skills/sprite-core-lock/scripts/check_continuity.py \
  --frames /tmp/char_pack/02_frames_512 \
  --min-iou 0.42
```

邻帧剪影 IoU 过低 = 前后动作不连贯，重画该格（带着小白人 + 前一帧）。不要整段缩放假装齐。

`--src` 里按 `q-gacha-character-sprites` 的命名放原画。缺文件脚本非 0 退出，不准用旧 plate 顶上。

`--self-test` 不读角色图，用合成色块验证公式还在：

```bash
python3 .cursor/skills/sprite-core-lock/scripts/pack_locked_sprites.py --self-test
python3 .cursor/skills/sprite-core-lock/scripts/check_core_lock.py --self-test
```

期望：两行都带 `OK`。

## 核心身高怎么量（不要换算法）

1. 品红键抠成 RGBA。
2. 前景里丢掉青绿 VFX：`(G > R+20) & (B > R+8) & (G > 130)`。
3. 用下半身像素估中轴 `cx`，取 `cx ± 13% 身宽` 的竖带。
4. 竖带里找最长连续实心 run = `core_height`。
5. 若 run `< 180`（图坏了或全是特效），回退到身体 bbox 高，并 **报警**，不要假装正常。

实测绊线（琉璃纸鸢 v4，中轴头到脚）：

- idle：380×4，Δ=0
- walk 各向：380–381，Δ≤1
- attack：377–381，Δ=4
- death：380–381
- skill：允许 **一帧** 跳跃落到 ~366，其余 379–381。超过一帧或再低 = 重装或重画，不准放宽 `TARGET_CORE`

`--max-delta 4` 是跨同一 clip 的 core 极差。跨 clip 均值都应在 380±2，attack 极差顶到 4。

## 质量门（装箱前）

```bash
python3 .cursor/skills/sprite-core-lock/scripts/check_frame_quality.py \
  --src /path/to/redraw_dir --prefix redraw
```

命中 `FAIL` 的帧整张重画。禁止「先装进去再看 GIF 圆不圆」。

## 禁止的脚本模式（看到就停）

| 模式 | 为什么炸 |
|---|---|
| `plate = idle[0]; plate.composite(vfx)` | 角色变底板，攻击像贴图 |
| `breathe(im, 1.008)` | idle 是缩放图片 |
| 4×4 sheet 等分切片 | 邻帧头顶从底边探进下一帧 |
| `contain(bbox_including_vfx)` | 大特效 → 角色缩到 300px 级 |
| 脸检测二次 refine | 单帧爆缩、身高乱跳 |
| 海军蓝 / 绿幕抠金棕发 | 头发被当背景吃掉 |
| 用 SDK `vyxc` 或 hello gate 验收美术 | 那是编译器门，跟帧无关 |

旧毒物：`/tmp/rebuild_fill.py`、`q_plate_*` + `q_vfx_*` 叠层。不要复活。

## 输出布局（给 delivery skill 打 zip）

```
$OUT/
  01_cg/                 # 仅 splash + 三视图
  02_frames_512/
    idle/idle_00.png …
    walk/{s,sw,w,nw,n,ne,e,se}/walk_<dir>_00.png …
    attack/atk_00.png …
    skill/skill_00.png …
    death/death_00.png …
    atlas.json
  03_sheets_original/    # 少量设计参考，不是切表源
  04_preview_gif/
  README.txt
```

`atlas.json` 只记录帧数 / 循环 / 本批 drift。不把「脸高 118」写进去当第二套锁——脸高锁已废弃。

## 回滚

装箱结果不对：删 `$OUT`，修原画或参数后重跑脚本。不要在 512 成品上再 Photoshop 一刀「对齐」。不要把 `TARGET_CORE` 改成每段一个数来掩盖漂。

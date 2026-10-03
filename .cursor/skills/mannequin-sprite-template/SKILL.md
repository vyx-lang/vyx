---
name: mannequin-sprite-template
description: >-
  小白人序列帧模板：一张 1024×1024、8×8、每格 128×128 的图像模型 sheet，切开后当动作圣经。
  Use when starting any gacha/chibi sprite set, or when the user says 小白人、占位、
  tile sheet、128、1024、动作模板. Generate the sheet with the image model — never
  draw the dummy in Python/PIL. Then slice and paint the real character onto cells.
---

# 小白人 tile 模板（图像模型，不是脚本小人）

角色帧如果一张一张独立生成，色、身高、前后动作都会漂。先用图像模型出 **一张** 小白人 sheet，切开成序列帧，后面的 AI 只往格子上画皮。

**禁止**用脚本画关节小人冒充模板。用户已否决。

## 死规格

| 项 | 值 |
|---|---|
| 画布 | **1024×1024**，`aspect_ratio=1:1` |
| 格子 | **128×128**，**8×8 = 64** 格 |
| 间隙 | **0**。格子贴死，不准留缝、不准编号、不准写字 |
| 底 | 每格品红 `#FF00FF` |
| 人 | 同一只小白人：白身体、深线、两点眼（背面无脸） |
| 大小 | 64 格同一身高，脚钉在格子底部同一条地平线 |
| 生成 | **只许 `GenerateImage`** |

切的时候按整数格切，不要自动找缝（缝是 0）：

```
cell(c, r) = crop(c*128, r*128, (c+1)*128, (r+1)*128)
```

```bash
python3 .cursor/skills/mannequin-sprite-template/scripts/slice_tile_sheet.py \
  --sheet /path/to/mannequin_sheet_1024.png \
  --out /tmp/mannequin_ai
```

期望：64 张 128 PNG + 各 clip 的 GIF。

## 64 格怎么排（给模型的行说明，也给切片用）

| 行 | 列 0–7 |
|---|---|
| 0 | idle 00–03 \| hit 00–03 |
| 1 | attack 00–07 |
| 2 | death 00–07（最后两格躺平，不要站起来） |
| 3 | walk_s 00–07（整行同一朝向：朝镜头） |
| 4 | walk_se 00–07 |
| 5 | walk_e 00–07 |
| 6 | walk_ne 00–07 |
| 7 | walk_n 00–07 |

`w` / `sw` / `nw` **只许**水平镜像 `e` / `se` / `ne`。不要再画一套。

走路必须是短步四相的加长版（8 帧同一朝向），禁止一行里换方向。

## 生成命令（图像模型）

`GenerateImage`：`aspect_ratio=1:1`，`filename=mannequin_sheet_1024.png`。

提示词必须写死：1024 画布、8×8、每格 128、无字无编号、品红底、同一只白人、同一身高、脚钉地线、每一行一个动作一个朝向。

可以丢一张 **空的** 8×8 品红格线当 `reference_image_paths`（只画格子，不画人）。不准用脚本画好的小人当参考再「生成」。

出图后立刻：

1. 断言文件是 1024×1024。
2. 按 128 切开。
3. 做成 GIF 给人过目。格子里钻进邻帧、一行里换脸、身高跳 = **整张 sheet 重画**，不准单格修补。

## 后面的 AI 怎么用

每张角色帧的 `reference_image_paths`：

1. splash
2. Q 版三视图
3. **这一格**切开的小白人
4. **上一格**已经画好的角色帧（第一格没有就省略）

提示词写：按这个小白人的姿势、脚位、朝向画皮，不要改身高、不要改镜头、不要另起一个姿势。

## 色 / 大小 / 连贯（切完、画皮后）

走 `sprite-core-lock` 的检查：

```bash
python3 .cursor/skills/sprite-core-lock/scripts/check_continuity.py \
  --frames /tmp/char_pack/02_frames_512 \
  --min-iou 0.42
python3 .cursor/skills/sprite-core-lock/scripts/check_core_lock.py \
  --frames /tmp/char_pack/02_frames_512 --target-core 380 --max-delta 4
```

邻帧剪影 IoU 过低 = 动作跳帧，重画那一格（带着小白人 + 前一帧），不要整段缩放对齐。

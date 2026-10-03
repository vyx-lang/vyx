---
name: q-gacha-character-sprites
description: >-
  二次元卡牌 gacha splash 立绘与 Q 版序列帧。Use when the user wants an anime gacha
  character splash, chibi/Q-version turnaround, or idle/walk/attack/skill/death
  sprite frames in 原神/崩铁 style. Also trigger on 立绘、序列帧、八向走路、卡牌角色.
  Do not use for photoreal/dark-fantasy art, compiler work, or packing already
  drawn frames (that is sprite-core-lock). Never commit PNG/zip into a compiler repo
  (that is asset-zip-delivery).
---

# 二次元卡牌角色：立绘 + Q 版序列帧

读完本文件再画。装箱走 [`sprite-core-lock`](../sprite-core-lock/SKILL.md)。交付走 [`asset-zip-delivery`](../asset-zip-delivery/SKILL.md)。三份缺一不可。

## 何时启用

用户要下面任意一项就启用：

- 二次元卡牌立绘 / gacha splash / 原神 / 崩铁风格角色
- Q 版三视图 / chibi turnaround
- idle / walk 八向 / attack / skill / death 序列帧
- 「画这个角色的动画帧」「序列帧」「精灵图」

不启用：写实暗黑、脸谱化 Q 版（大头豆豆眼无五官）、纯 UI 图标、编译器 / Vyx 工作。

## 阶段清单（列完直接做，不问要不要开始）

1. 锁设计：一张 gacha splash + 一张 Q 版三视图。
2. **先出小白人模板**（图像模型，不是脚本）：一张 **1024×1024**、**8×8**、每格 **128×128**，见 [`mannequin-sprite-template`](../mannequin-sprite-template/SKILL.md)。切开，用 GIF 过目。
3. 角色帧 = 往**这一格小白人**上画皮。`reference_image_paths` = splash + 三视图 + **本格小白人** + **上一格已画角色**。禁止再对角色做 32 次无模板的独立生成。
4. 品红抠图 + 色差 / 大小 / 邻帧连贯门；坏帧带着小白人重画，不准硬装。
5. `sprite-core-lock` 装箱。`asset-zip-delivery` 打 zip。角色 PNG 不进编译器仓库。

## 风格（死约定）

必须是 **二次元 gacha splash**（原神 / 崩铁那类）：清晰五官、妆造、服饰褶皱、金色饰品可读。

禁止：

- 写实、暗黑写实、厚涂泥脸
- 脸谱化：无鼻无口、豆豆眼、贴纸五官
- 欧美卡通 / 像素 / 黏土

Q 版是 **缩小的同一套设计**，不是换一个角色。身子偏瘦可以，左右留空正常；**禁止非均匀拉伸**去填画布。

## 片段规格（帧数是死的）

| 片段 | 帧数 | 循环 | 内容 |
|---|---|---|---|
| idle | 4 | 循环 | 同机位微动作：重心、眨眼、发丝。不是整图缩放 |
| walk 八向 | 每向 4 | 循环 | 短步四相：contact / down / passing / up |
| attack | 8 | 不循环 | 真拉弓（或对应武器）+ 特效画在动作上 |
| skill | 10 | 不循环 | 聚气→旋转/蓄力→释放→落地；特效跟动作走 |
| death | 4 | **不循环，停最后一帧** | 受击→跪/倒→落地→静止 |

方向名（小写，目录名用这个）：

`s` 下 / `sw` 左下 / `w` 左 / `nw` 左上 / `n` 上 / `ne` 右上 / `e` 右 / `se` 右下。

W / SW / NW **只许**由 E / SE / NE（或反过来）水平镜像，不要各画一套还对不齐。

技术硬约束（和 `sprite-core-lock` 同一套数）：

- 每帧最终交付 **512×512 PNG，透明底**
- **禁止根运动**：脚钉地面，人物锁中心
- **整身可见**；特效越界裁掉，**不能因为特效把角色缩小**
- 特效必须**画在动作帧上**（角色动作联动特效）
- 人物**核心大小跨片段不能变**

## 生成规则（死命令级）

### 先锁参考，再衍生动画

1. 先出 **1 张** splash 立绘、**1 张** Q 版三视图（正/侧/背，同一比例）。
2. 后续每一帧的 `GenerateImage` **必须**把这两张放进 `reference_image_paths`。
3. 没有参考就开画 = 必漂。漂了就停，重锁参考，不要用后期缩放「对齐」。

### 角色画皮：一格一张（模板 sheet 除外）

- 小白人模板 **必须** 是一张 1024 的 64 格 sheet（图像模型出）。那是唯一允许的角色前 sheet。
- 画皮时比例 `1:1`，底 **品红 `#FF00FF`**。一次只画 **1 格对应的 1 个姿势**。
- 禁止再为角色单独出 4 格拼表（邻帧会探进来）。禁止 “same pose four times”。
- 禁止用 Python 画关节小人当模板。
- 源文件命名（装箱脚本按这个找）：

```
redraw_idle_00.png … _03.png
redraw_atk_00.png … _07.png
redraw_skill_00.png … _09.png
redraw_death_00.png … _03.png
redraw_walk_{s,w,n,se,ne}_00.png … _03.png
```

`e` / `sw` / `nw` 装箱时镜像，不必画。

### idle：同机位微动作

四帧必须是**同一机位、同一身高、同一脚位**的微小变化：

- 00 基准站立
- 01 重心微移 / 肩微落
- 02 眨眼或头微低
- 03 发丝 / 衣带回弹，回到接近 00

禁止：

- 整图缩放当呼吸（`breathe(plate, 1.008)` 这种）
- 换角度、换镜头、角色走近走远
- 四张同一姿势复制

### walk：短步四相

每向 4 帧，步幅要短（Q 版大步会劈叉、出画）：

| 帧 | 相 | 脚 |
|---|---|---|
| 00 | contact | 前脚刚点地，后脚将离 |
| 01 | down | 重心最低，双脚较近 |
| 02 | passing | 摆动腿经过支撑腿 |
| 03 | up | 后脚蹬，前脚迈出但未劈叉 |

禁止：乱角度立绘当走路、弓步劈叉、脚滑出画布、同一姿势复制四次、从 4×4 大表等分切片（邻帧头发会探进来）。

先画 `s` `w` `n` `se` `ne` 五向；其余镜像。五向的脚接触点必须在画布同一条地平线上。

### attack / skill：动作和特效同一张

用户原话级约束：**既要角色动作，又要特效，特效跟动作联动。禁止角色当底板。**

attack 8 帧建议：

0 持弓警戒 → 1 搭箭 → 2 开拉 → 3 满弦 → 4 射出（箭离弦）→ 5 后坐 / 衣带前冲 → 6 光矢飞出仍连着姿态 → 7 收回站姿

skill 10 帧建议：

0 聚气 → 1–2 旋转 / 踏步 → 3 举弓 → 4–5 大箭成形 → 6 释放 → 7–8 冲击波跟身体走 → 9 落地收势

禁止：

- 一张 idle 底板 `composite()` 叠 VFX 层
- 特效单独一张、角色单独一张再叠
- 释放帧白爆把角色吃掉（反白）
- 为了塞进特效把角色 `contain` 缩小

特效画不下就裁，角色身高不准让。

### death

4 帧不循环。最后一帧是尸体 / 跪地静止，引擎停这帧。预览 GIF 也不要 loop 回去站起来。

## 提示词骨架（生成时套这个，不要自由发挥风格）

公共前缀（每帧都带）：

```
Anime gacha chibi sprite, same character as the reference splash and turnaround,
full body visible, feet planted on an invisible ground line, centered,
NOT a sheet, ONE pose only, solid magenta #FF00FF background, no crop,
no photoreal, no dark fantasy, no faceless chibi.
```

然后只追加这一帧的动作一句（拉弓到哪、哪只脚在前、特效长在哪）。不要在提示词里写 “breathe by scaling” / “four frames on one canvas”。

## 质量门（抠完、装箱前）

品红抠图后，对每一张跑（脚本在 `sprite-core-lock`）：

```bash
python3 .cursor/skills/sprite-core-lock/scripts/check_frame_quality.py \
  --src /path/to/redraw_dir --prefix redraw
```

任一帧命中就 **整张重画**，不准硬装进 512：

| 判据 | 含义（本对话里真实炸过） |
|---|---|
| 前景 HSV 饱和度均值 `< 28` | 灰 / 褪色 / 损坏 |
| 粉线稿占比高 | 品红线稿当成品（attack 急诊反白的前身） |
| 近白像素占比高 | 释放帧白爆 |
| 近黑剪影占比高 | 抠坏变黑影 |
| 四帧像素几乎一样 | walk / idle 复制姿势 |
| 底边露出另一颗头 / 另一双脚 | 切表邻帧残留 |

## 禁止再犯（本对话里用户点名骂过的）

1. **静态底板 + 特效**：同一张 idle plate `composite()` 叠 VFX。看起来像纸片人套特效。
2. **idle 整图缩放当呼吸**：`breathe(plate, 1.008)`。
3. **walk 切表不干净**：4×4 sheet 等分切片，底边露出邻帧头顶。
4. **walk / idle 同一姿势复制四次**：没有迈步。
5. **attack 反白 / 损坏**：品红线稿硬抠变黑影；释放帧白爆。
6. **idle / walk 不像人**：乱角度、劈叉、出画。「不是人类可以做出来的」。
7. **人物核心大小随时变**：idle 415、walk 440、attack 316（特效撑宽后 contain 缩小角色）。
8. **把 PNG / zip 推进编译器仓库**：Cloud 挂在 Vyx 上不是许可证。打包外链，见 `asset-zip-delivery`。

旧错误文件名（看到就当毒物，不要当动画源）：`q_anim_*`、`q_vfx_*`、`q_plate_*`、`rebuild_fill.py`。

## 角色设计锁：琉璃纸鸢（Glazed Kite）

这是收成本 skill 的那次对话的角色。换角色时用同一流程，只换设计锁。

- 蜜茶 / 金棕高马尾
- 丹凤 / 琥珀眼
- 白 + 青瓷 + 金的汉服风 Q 版
- 橙流苏
- 头上小白鸟
- 风鸢弓：木框 + 青绿纸鸢帆

参考（若本机还在）：

- splash：`/opt/cursor/artifacts/assets/anime_gacha_character_splash.png`
- 三视图：`/opt/cursor/artifacts/assets/q_char_turnaround.png`

没有参考就先重画这两张，再衍生动画。

## 验收（父级只看命令输出）

```bash
# 1. 帧数
find "$OUT/02_frames_512" -name '*.png' | wc -l
# 期望：58  （idle4 + walk8×4 + atk8 + skill10 + death4）

# 2. 核心身高锁死
python3 .cursor/skills/sprite-core-lock/scripts/check_core_lock.py \
  --frames "$OUT/02_frames_512" --target-core 380 --max-delta 4
# 期望：末行 CORE_LOCK OK

# 3. 交付
# zip + 外链，仓库 git status 里不准出现 PNG
```

预览 GIF 只给人看，不当验收物。验收看 `check_core_lock.py` 和帧数。

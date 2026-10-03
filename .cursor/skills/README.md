# Agent Skills — 二次元卡牌角色资产

从「琉璃纸鸢」立绘 / 序列帧对话收成。三份 skill **一起读**，缺一就会把上一轮骂过的坑再踩一遍。

| Skill | 何时启用 |
|---|---|
| [`mannequin-sprite-template`](mannequin-sprite-template/SKILL.md) | 先出 1024×1024 / 128 格 / 64 tile 小白人（图像模型，禁止脚本小人） |
| [`q-gacha-character-sprites`](q-gacha-character-sprites/SKILL.md) | 往小白人格子上画皮：立绘、idle / walk / attack / hit / death |
| [`sprite-core-lock`](sprite-core-lock/SKILL.md) | 装箱 + 色差 / 核心身高 / 邻帧 IoU |
| [`asset-zip-delivery`](asset-zip-delivery/SKILL.md) | 在编译器或不相关仓库的 Cloud 空间出图 |

根 `.gitignore` 有 `**/.*/` 和 `.cursor/`，新文件要用 `git add -f .cursor/skills` 才能进版本库。**PNG / zip / 立绘不准提交进这个仓库。**

---
name: asset-zip-delivery
description: >-
  在编译器或不相关仓库的 Cloud 空间交付美术资源。Use when packing character
  art, sprite frames, or any binary asset as a downloadable zip+URL while the
  workspace is a compiler/unrelated repo (Vyx, bootstrap_compiler). Trigger on
  打包zip、外链、不要提交图片、Cloud 空间是编译器. Never git add PNG/zip
  into those repos. Never treat SDK vyxc as art acceptance.
---

# 美术交付：只打 zip + 外链

Cloud Agent 经常挂在 **编译器仓库**（例如 `github.com/vyx-lang/vyx`）上画角色。仓库的 `AGENTS.md` 管的是 SDK 编译器 / MIR / DCI，**不是**许可证把立绘推进 `release`。

用户原话级约束：空间本身是编译器空间，把资源推进这个仓库是偷懒。正确交付是 **zip + 直链**。

## 何时启用

- 刚画完 / 刚装箱完角色资产
- 用户说打包、要下载、不要进 git、Cloud 产物列表没按钮
- 工作区是 Vyx / 其它无关 repo，产出却是图 / 音频 / 模型

## 死流程

1. 资源只写在 `/tmp/<pack>/` 和 `/opt/cursor/artifacts/`。
2. 打 zip，**不要** `git add` 任何 PNG / JPG / GIF / zip / webp。
3. 上传临时直链（下面这条已跑通）。
4. 回复里给人：**直链 + 本地绝对路径 + 目录说明**。不要只说「在 artifacts 里」。
5. 工作分支若曾误提资源：从 git 拿掉、关错开的 PR、删远程资源分支。编译器分支回到 `release`。

## 打包布局

和 `sprite-core-lock` 输出一致时，直接 zip 那个根目录：

```
01_cg/
02_frames_512/     # 58×512 透明 PNG + atlas.json
03_sheets_original/
04_preview_gif/
README.txt
```

```bash
PACK=/tmp/char_pack
ZIP=/opt/cursor/artifacts/char_asset_pack.zip
rm -f "$ZIP"
# zip 内带一层目录名，解压不洒一地
ln -sfn "$PACK" /tmp/char_asset_pack
(cd /tmp && zip -r -q "$ZIP" char_asset_pack)
ls -lh "$ZIP"
unzip -l "$ZIP" | head
```

同时留一份 `/tmp/char_asset_pack.zip`（同内容），防止 artifacts 目录在 UI 上没下载按钮。

## 外链（已验证）

litterbox，72h，这条 API 在本对话里传过 ~几十 MB 的包：

```bash
curl -fsS -F "reqtype=fileupload" -F "time=72h" \
  -F "fileToUpload=@/opt/cursor/artifacts/char_asset_pack.zip" \
  https://litterbox.catbox.moe/resources/internals/api.php
```

期望：一行 `https://litter.catbox.moe/<id>.zip`。

封装：

```bash
bash .cursor/skills/asset-zip-delivery/scripts/upload_litterbox.sh \
  /opt/cursor/artifacts/char_asset_pack.zip
```

失败就重试最多 4 次（指数退避 4/8/16/32s）。不要改走需要账号的网盘。不要把 zip commit 上去「当网盘」。

直链有效期约 72h，回复里写清楚。过期就重新打、重新传，不要叫人去翻已关 PR。

## 绝对不要

- `git add` 立绘 / 序列帧 / zip 进 Vyx / `bootstrap_compiler` / `probes/` / `docs/`
- 开 `cursor/liuli-*-assets-*` 这类资源 PR（本对话里的 #26 已关，远程已删，不要复活）
- 用 SDK `/usr/local/bin/vyxc`、hello gate、`--verify-mir2`、fixpoint 当美术验收
- 混用不同构建的 SDK 编译器 `.so` / runtime（那是编译器纪律，跟图无关，但别顺手破坏工作区）
- 只丢 `/opt/cursor/artifacts/...` 路径就收工——Cloud 产物列表经常 **没有下载按钮**
- 把 skill 本身和 58 帧 PNG 打进同一个 git commit

## 回复模板（按这个填，不要写成编译器 PR 叙述）

```
zip：<litterbox url>（约 72h）
本地：/opt/cursor/artifacts/<name>.zip
解压根：/tmp/<pack>/
帧数：58 × 512
核心身高：idle/walk/death 380，attack Δ≤4（check_core_lock 输出贴上）
预览：04_preview_gif/ 或 artifacts 下的 strip
本包未进 git。
```

## 和本仓库的边界

- 编译器改动仍只许 `bootstrap_compiler/` + `vyx_codegen/` + `tools/dci/`。
- 本 skill 与两份美术 skill 可以进 `.cursor/skills/`（文本 + 脚本）。
- 角色 PNG 永远不进这个边界。

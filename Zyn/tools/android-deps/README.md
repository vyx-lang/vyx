# Zyn Android 原生依赖

为 Zyn 当前的 SDL 窗口与文本栈构建 Android 共享库。默认构建 `arm64-v8a`、`x86_64`，使用 Release、NDK r30-beta2、API 29 和 `c++_shared`。版本与仓库现有头文件一致；Windows 库和公共头文件不会被覆盖。

| 依赖 | 版本 | 官方源 | Android 产物 |
| --- | --- | --- | --- |
| SDL3 | 3.4.8 | [SDL release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.8) | `libSDL3.so`，同版本 Java 源码 |
| FreeType | 2.14.1 | [FreeType tag](https://github.com/freetype/freetype/tree/VER-2-14-1) | `libfreetype.so` |
| HarfBuzz | 14.2.0 | [HarfBuzz tag](https://github.com/harfbuzz/harfbuzz/tree/14.2.0) | `libharfbuzz.so` |
| ICU | 78.3 | [ICU release](https://github.com/unicode-org/icu/releases/tag/release-78.3) | `libzynicuuc.so`、`libzynicui18n.so`、`libzynicudata.so` |

下载地址和固定 SHA256 见 [sources.json](sources.json) 与 ICU 子目录中的来源记录。构建前校验归档哈希；下载失败、哈希不符或编译失败都会终止。源文件、构建目录和逐命令日志保存在 `.work/`，重复执行复用这些目录，不清理其他项目缓存。

## 构建

需要 Windows、PowerShell 7、Android NDK、CMake、Ninja、`curl.exe` 和 `tar.exe`。当前环境默认路径：

- NDK：`E:/Android/sdk/ndk/30.0.15729638`
- CMake：`D:/Jetbrains/CLion/bin/cmake/win/x64/bin/cmake.exe`
- Ninja：`D:/LLVM/bin/ninja.exe`

从仓库根目录执行：

```powershell
./Zyn/tools/android-deps/Build.ps1 -Jobs 2
./Zyn/tools/android-deps/icu/Build-Icu.ps1 -Jobs 2
```

两条命令顺序运行，默认分别覆盖两个 ABI。`Build.ps1` 可用 `-Libraries SDL3` 或 `-Abi arm64-v8a` 只构建一个依赖或架构；HarfBuzz 需要先有对应 ABI 的 FreeType。工具链路径可通过同名参数调整。

应用还需要与目标 ABI 一致的 Vyx 静态运行库。从仓库根目录执行，先确保已从当前源码构建 SDK 编译器 `bootstrap_compiler/out/vyxc.exe`，并配套使用同次构建的后端与运行时：

```powershell
$env:LLVM_ROOT = (Resolve-Path ./clang).Path
$env:ANDROID_NDK_HOME = 'E:/Android/sdk/ndk/30.0.15729638'
Push-Location bootstrap_compiler
./out/vyxc.exe build --target vyx_runtime --triplet aarch64-linux-android29 -j4
./out/vyxc.exe build --target vyx_runtime --triplet x86_64-linux-android29 -j4
Pop-Location
```

产物分别位于 `bootstrap_compiler/out/runtimes/android/<arm64|x64>/ndk/libvyx_runtime.a`。交叉链接只接受对应目标目录的运行库；不能复用宿主 `out/` 中的静态库。

共享库放在：

```text
Zyn/vendor/<SDL3|FreeType|HarfBuzz|ICU>/lib/android/<arm64-v8a|x86_64>/
```

每个目录包含实际 `.so`、来源及产物哈希 `manifest.json`、许可证。SDL、FreeType、HarfBuzz 目录同时保留同一 NDK 的 `libc++_shared.so`；打包时每个 ABI 只需一份，校验脚本会拒绝彼此不同的副本。库文件的 SONAME 使用无版本文件名，ICU 的公开符号仍保留 `_78` 后缀。

SDL 的 11 个官方 Java 文件复制到 `Zyn/vendor/SDL3/android/java/org/libsdl/app/`，供 Android 工程编译。原生依赖构建关闭可选的 `SDL_ANDROID_JAR` 目标，Java 源码随应用构建，不需要在本步骤生成 JAR。

## 构建范围

- SDL 使用上游 Android 后端，包含窗口、输入、音频等实现，不替换为桌面库。
- FreeType 使用 NDK 的 zlib；关闭额外的 BZip2、PNG、Brotli 和 FreeType 内部 HarfBuzz 支持。核心字形栅格化保留；这些额外压缩格式、PNG 嵌入字形和相关自动 hinting 路径不在本配置内。
- HarfBuzz 启用 FreeType 互操作及核心 shaping，关闭与 Zyn 当前调用无关的 subset、raster、vector、GPU 工具库。Zyn 独立调用 ICU；本配置不构建 HarfBuzz 的 ICU adapter。使用上游仓库自带 CMake 配置，其维护状态由上游明确标为社区维护。
- ICU 编译官方 `sources.txt` 列出的 common 与 i18n 实现，并链接官方发行包中的完整 `icudt78l.dat`。数据含 4305 项、33,107,232 字节；两个 ABI 都是小端，可使用该数据。构建不使用 `stubdata`，也不链接 Android 私有 ICU。具体数据逐字节验证见 [ICU 构建说明](icu/README.md)。

## 验证

```powershell
./Zyn/tools/android-deps/Verify.ps1 -Abi arm64-v8a
./Zyn/tools/android-deps/Verify.ps1 -Abi x86_64
```

验证会用 NDK 编译仓库实际的 `src/nut/text_bridge.c`，要求 `--no-undefined` 链接成功，同时生成运行探针。产物、日志及 `result.json` 位于 `.work/verify/<ABI>/`。这是目标架构编译与链接验证；未提供设备时，结果明确记录 `runtime: not run`。

设备在线后可运行：

```powershell
./Zyn/tools/android-deps/Verify.ps1 -Abi arm64-v8a -Device '<adb serial>'
```

脚本先检查设备声明的 ABI，再上传到 `/data/local/tmp/zyn-deps-<ABI>/`，验证四个库的实际版本、FreeType 初始化、HarfBuzz buffer、ICU 组合字符/中文/emoji 的字素边界及混合方向文本。该探针不创建 SDL 窗口；窗口、输入和应用界面仍需通过 Zyn APK 实测。

2026-09-27 的本机验证结果：

| 检查 | arm64-v8a | x86_64 |
| --- | --- | --- |
| 四个依赖实际交叉构建 | 通过 | 通过 |
| ELF 架构、共享库 SONAME | 已核对 | 已核对 |
| ICU 完整数据与官方 DAT 的逐字节哈希 | 相同 | 相同 |
| 当前 `text_bridge.c` 的 `--no-undefined` 链接 | 通过 | 通过 |
| 版本、字素边界、双向文本运行探针编译 | 通过 | 通过 |
| Android 设备执行 | Android 16 arm64 真机通过 | Android 10 x86_64 AVD 通过 |

ICU 两次完整 `-j2` 编译分别为 109.5 秒、108.5 秒。其余依赖首次构建日志在 `.work/logs/20260927-173011-218/`，许可证补充后的增量复核在 `.work/logs/20260927-173340-918/`。Release 库当前保留符号，便于集成诊断。x86_64 的完整 APK 已在 Android 10 AVD 上显示界面并通过暂停/恢复；arm64 的依赖运行探针已通过，完整 APK 的真机验收另见[平台接入记录](../../docs/CACAO_PLATFORM_ZH.md)。

## 许可证

SDL 使用 zlib 许可证；FreeType 提供 FTL 或 GPLv2 授权，保留其完整许可及组件声明；HarfBuzz 保留 `COPYING`；ICU 保留发行包完整 LICENSE，包括第三方条款。复制的 NDK 运行库附带 sysroot NOTICE。生成 Android 分发包时应同时携带相应许可文件。

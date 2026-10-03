# Cacao 平台接入

Zyn 的 Platform 层管理 SDL3 窗口和事件。Nut 将窗口提供的原生句柄交给
`std.cacao.Instance.createNativeSurface`，由 Cacao 创建对应后端的 Surface、
Device 和 Swapchain。Platform 不持有 Vulkan、D3D 或 OpenGL 设备。

## 窗口句柄与生命周期

| 平台 | SDL3 来源 | 交给 Cacao 的句柄 |
| --- | --- | --- |
| Windows | `SDL.window.win32.hwnd`、`SDL.window.win32.instance` | HWND、HINSTANCE |
| Android | `SDL.window.android.window` | ANativeWindow |
| Linux / Wayland | `SDL.window.wayland.display`、`SDL.window.wayland.surface` | wl_display、wl_surface |
| Linux / X11 | `SDL.window.x11.display`、`SDL.window.x11.window` | Display、Window ID |
| macOS / iOS / tvOS | `SDL_Metal_CreateView`、`SDL_Metal_GetLayer` | CAMetalLayer |

`PlatformWindowHandle` 保存借用的原生句柄。空窗口、已销毁窗口以及 SDL 没有提供
所需句柄时，`isValid()` 返回 false，Renderer 创建返回错误。X11 的 Window ID
通过 SDL 的整数属性读取，不把它当指针。Android 的 EGLSurface 不是 ANativeWindow；
Apple 的 NSWindow、UIWindow 也不是 CAMetalLayer。

销毁顺序为 Renderer（含 Cacao Surface）→ Window → Platform。Apple 的 SDL Metal
view 归 Window 所有，在 SDL 窗口之前释放。Android 的 SDLActivity 管理原生窗口
生命周期。应用宿主收到进入后台的事件后暂停提交帧；回到前台后重新读取窗口句柄，
不能向已销毁的 ANativeWindow 提交。Surface 被系统替换后，旧的借用句柄不能继续使用。
`Renderer.syncToWindow` 检测 Android 原生窗口变化后重建 Surface 和 Swapchain；
可恢复的 Android 帧错误也强制重建，避免同尺寸或句柄地址复用触发 resize 空操作。
应用也可显式调用 `recreateSurface(window)`。重建失败返回错误，格式变化要求完整
重建 Renderer；OpenGL ES 的 Surface 持有 EGL context，局部 Surface 重建会使已有
GPU 对象失效，因此该方法明确要求重新创建整个 Renderer。

Linux 和 Apple 句柄接线不等于这些目标的 SDK 和设备测试已经完成。Linux 的 Cacao
构建必须选择与 SDL 驱动一致的 Xlib 或 Wayland surface 支持；当前任务没有构建
Linux、macOS 或 iOS SDK，也没有在这些设备上验证渲染。Cacao 不提供的后端应返回
创建错误，不能用空句柄或空渲染器代替成功。

## 设备能力

Nut 创建 `DeviceDesc.forSurface`，请求可呈现的图形队列，默认不请求可选高级 GPU
特性。读取适配器能力用于诊断，不代表启用全部能力。Cacao 面向所有 RHI 使用者；
Zyn 的功能需求不会成为整个 Cacao 的最低硬件要求。

## 同步 Release SDK

先在 Cacao 仓库配置并构建 Release 的 `Cacao` target，再运行：

```powershell
Zyn/tools/sync_cacao_sdk.ps1 `
  -CacaoRoot E:/Dev/C++/Cacao `
  -BuildDirectory E:/Dev/C++/Cacao/cmake-build-release `
  -Platform windows-x64
```

Windows 库位于 `vendor/Cacao/lib/x64`，运行时位于 `vendor/Cacao/bin/x64`。
启用 WebGPU 的 Cacao DLL 还依赖 `dawn.dll`，同步脚本会同时复制它与
`slang-compiler.dll`，SDK 的正常 stage 流程将这些 DLL 放到应用输出目录。

Android 分别构建 `arm64-v8a` 和 `x86_64`，统一使用 Android API 29，启用 Vulkan
和 OpenGL ES，关闭当前仅供 Windows 使用的 WebGPU 构建选项。对每种 ABI 执行：

```powershell
Zyn/tools/sync_cacao_sdk.ps1 `
  -CacaoRoot E:/Dev/C++/Cacao `
  -BuildDirectory E:/Dev/C++/Cacao/cmake-build-android-arm64 `
  -Platform android-arm64-v8a
```

另一种平台参数为 `android-x86_64`。Android 产物分别位于
`vendor/Cacao/lib/android/<ABI>/`，包括 `libCacao.so` 与 `libslang-compiler.so`。
SDL3、FreeType、HarfBuzz、ICU 和 Zyn 本体必须另有同 ABI 的 Android 构建；Windows
导入库不能用于 Android 链接。

脚本核验 Release、来源目录和 ABI，在所有输入存在后才复制，每个文件用 SHA256
确认复制结果。`vendor/Cacao/sdk-<平台>.json` 记录来源提交、工作区状态、后端选项、
Android API 和文件摘要。它描述本次复制的构建，不代替设备验收。

## 验证入口

从 `Zyn/` 执行 `bin/zyn.ps1 build`，再从下面三个工程分别通过 SDK 入口构建并运行：

- `samples/projects/zyn_native_surface_smoke`：句柄校验、真实 SDL 窗口和 Cacao Surface、同尺寸 Surface 重建后的绘制、销毁幂等。
- `samples/projects/zyn_platform_smoke`：现有 Platform 操作。
- `samples/projects/zyn_render_smoke`：现有 Nut 绘制和渲染资源操作。

`zyn_native_surface_smoke` 的人工句柄值只测试类型校验，不会交给 Cacao；它随后创建
真实窗口来验证 Surface。Android 需要应用模板的 Activity/native 桥，在 Android
进程中运行真实 SDL/Cacao，不能把 Windows smoke 结果当作 Android 验收。

Zyn 的布局尺寸使用显示独立单位。每帧从 SDL 读取窗口像素尺寸和显示缩放，布局宽高
等于像素宽高除以显示缩放，绘制时再统一映射到像素。SDL 的触摸坐标是归一化值，需映射
到同一布局坐标后才参与命中测试；鼠标坐标及输入法候选框也沿用该坐标系。
`samples/projects/zyn_recomposition_smoke --self-test` 检查两组不同像素密度下的
布局尺寸和点击目标相同。Android 应用通过 `AndroidPermission` 异步申请清单中声明
的运行时权限；调用方按请求 ID 读取结果，完成后释放请求。

2026-09-27 至 28 日的实际验证：Windows 的 D3D12、Vulkan Surface smoke 均通过；
Android 10 x86_64 AVD 上，`tests/android/Run.ps1` 创建的 Java 与 Kotlin 工程均
完成原生编译、APK 打包、安装、启动、显示页面和暂停/恢复。Cacao 日志报告
`device API 1.1, render path legacy RenderPass, timeline off`。测试保存启动和恢复后
的截图及 logcat；检查进程存在的同时也检查 Android 窗口层级，避免把 SDL 加载
错误对话框误报为成功。

2026-09-28 的 x86_64 API 29 模拟器交互测试使用 `Run.ps1 -Interaction` 的 Java
工程：触屏按钮执行一次只产生一次命令；相机权限的系统弹窗中分别选择允许和拒绝，
异步结果为 `1` 和 `0`。将模拟器从 1080×2400、420 dpi 改为 540×1200、210 dpi，
页面保持相同的显示独立尺寸，缩放后的按钮仍可点击；测试后恢复模拟器原始参数。
APK 只包含 `Zyn.runtime.toml`，不包含构建用的 `Vyx.toml`。
`Run.ps1 -Interaction -StatefulInteraction` 用相同的按钮、权限和触屏步骤验证
`StatefulComponent`，另外检查 `stateResource()` 命名空间及帧、触屏事件进入
`update()`；点击后画面中的计数从 0 更新为 1。对应的 Stateless 用例也在同一
模拟器和运行时上重跑。

`Run.ps1 -CustomRender -DeviceSerial emulator-5554` 验证应用自带的 Slang 文件随
APK 发布，并由 Cacao 在两个 `CustomRenderView` 槽位中绘制；门会检查运行时标记、
实际截图颜色及暂停/恢复后的画面。完整的接口与边界见
[CustomRenderView](CUSTOM_RENDER_VIEW_ZH.md)。

2026-10-01 的 Windows 回归使用从当时源码构建的 SDK 编译器完整构建 Zyn AOT
（`-j4 -O0`，177 个任务，包含 root chunks），并运行
`zyn_recomposition_smoke --self-test`，返回 `0`。它补充验证容器测量、
稳定列表 key 和显示密度坐标契约；该无头测试不创建 Surface，也没有重跑上述
Android 设备或 Windows GPU 画面门。新的 surface 外观及布局用法见
[View 组合与布局](COMPOSITION_ZH.md)。

Android API 29 下，Zyn 使用私有 SONAME 的 ICU 78 库，避免系统 `libicu*.so`
遮蔽随包版本；`src/android/compat.c` 提供该系统版本缺少的 `bcmp` 符号。
arm64 真机 `192.168.1.3:35153` 上，Java 模板工程完成原生编译、APK 打包、安装、
实际 Vulkan 渲染及后台/前台恢复。恢复前后进程 ID 相同，截图均显示 Zyn 页面；
Cacao 日志报告 `device API 1.3, render path dynamic core, timeline on`。
真机验收记录位于忽略目录 `tests/android/.runs/20260927-152738-151-arm64-v8a-java/`。

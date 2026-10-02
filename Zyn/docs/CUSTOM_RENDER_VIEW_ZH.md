# CustomRenderView：在 Zyn 页面中绘制 Cacao 内容

`CustomRenderView` 是普通 View 布局中的 GPU 内容区域。应用通过 Cacao 在离屏颜色目标上录制命令，Nut 按 View 顺序把目标与其他 UI 合成。窗口、命令提交和呈现仍由 Zyn 管理；应用可以自行决定区域内部的管线、着色器、网格和渲染步骤。

可运行示例见 [`zyn_custom_render_view_smoke`](../../samples/projects/zyn_custom_render_view_smoke/src/main.vyx)。它在同一页面中放置两个独立槽位、一个普通按钮，并用 Cacao 着色器绘制彩色三角形。

在仓库根目录运行：

```powershell
Set-Location Zyn
& ..\bootstrap_compiler\out\vyxc.exe build --target Zyn
Set-Location ..\samples\projects\zyn_custom_render_view_smoke
& ..\..\..\bootstrap_compiler\out\vyxc.exe build
& .\target\zyn_custom_render_view_smoke.exe --visible
# 用 Vulkan 验证同一示例时，先设置 $env:CACAO_BACKEND = 'vulkan'
```

## 接入方式

1. 实现 [`CustomRenderHandler`](../src/nut/CustomRender.vyx) 的 `render` 和 `release`。`render` 收到借用的 `Device`、`ShaderCompiler`、`CommandEncoder`、输出 `Texture` 及目标的像素宽高。它返回 `Result<i32, Zyn.Nut.Types.Error>`；错误会中止当前帧并传回宿主。
2. 在 `AppSpec.setRendererSetup(fn(&mut Renderer)->i32)` 回调中，调用 `renderer.registerCustomRender(slot_id, Box::<dyn CustomRenderHandler>.new(handler))`。槽位 ID 必须非负且不能重复。回调在 `Renderer.create` 成功后、第一帧之前执行。
3. 把 `CustomRenderView.create(view_id, slot_id).size(width, height)` 放进组件的 View 树。View 只保存槽位 ID；拥有 GPU 资源的处理器由 Renderer 持有，不随 View 重建而复制。

应用的着色器和其他文件放在项目根目录的 `assets/` 下，代码中也使用 `assets/...` 路径，例如 `ShaderDesc.source_path: "assets/shaders/triangle.slang"`。`zyn publish` 会将该目录复制到桌面发行目录；Android APK 将其放在 `assets/zyn/assets/`，生成的 `ZynActivity` 启动时再安装到应用私有目录的 `assets/`。因此同一源路径可用于桌面和 Android。已生成的旧 Android 工程需要更新 `ZynActivity.java` 中的项目素材安装代码；新的 `zyn new`/`init-android` 工程会直接使用新版模板。

关键接口如下。完整的着色器、管线创建和错误处理以可运行示例为准。

```vyx
use CustomRenderHandler = Zyn.Nut.CustomRender.CustomRenderHandler;
use CustomRenderView = Zyn.App.CustomRenderView;

// AppSpec.create(...).setRendererSetup(setupRenderer)
// setupRenderer 中注册整数槽位 1。
// 在 View 树中：
CustomRenderView.create("scene.preview", 1).size(420.0, 240.0)
```

处理器可以持有自己的管线、缓冲、着色器和深度目标。宿主传入的对象仅供本次调用借用：不得保存、销毁或对宿主编码器执行 `reset`、`end`、`submit`、`present`。`render` 应完成针对输出目标的绘制录制；Zyn 负责在调用前后转换目标状态，并在同一图形队列上提交。GPU 资源须保持到提交完成，不能在 `render` 返回前销毁。`release` 在注销槽位、Renderer 销毁和设备资源重建时调用，处理器应释放自身资源，并允许之后重新创建。

## 布局、合成与生命周期

`CustomRenderView` 使用普通 View 的尺寸、可见性、命中和动作绑定。输出以矩形四边形合成，并对自身矩形及滚动容器祖先设置裁剪；越出滚动容器的区域也不能命中。一个槽位在一帧里只录制一次。如果多个可见 View 使用同一槽位，目标尺寸取这些 View 需求的最大值，各 View 将该目标缩放到自己的矩形；需要不同画面或独立分辨率时使用不同槽位。

视图默认高 160 个逻辑单位，`.size(...)` 或正的 `.height(...)` 优先于
父容器内容测量。需要剩余高度时，先 `.autoHeight()` 清除默认高度再
`.fillHeight()`。用 `.childIf(...)` 选择是否加入 View，并不会注册或注销
Renderer 槽位；GPU 资源仍按处理器的生命周期管理。将它嵌在有 elevation 的
Panel 或 MD3 card 内可以组合页面 surface，而不用改 GPU 回调。
详细布局约束见 [View 组合与布局](COMPOSITION_ZH.md)。

Zyn 按像素尺寸创建并复用每个槽位的 RGBA8 颜色目标与采样描述符；尺寸改变后重建该目标。`RenderGraph` 保留外部 View 命令，`RenderPlan` 记录外部颜色目标资源，Renderer 先录制处理器，再按原有 View 顺序合成。应用无须把画面拷入 Sprite 图集，也不发生 CPU 回读。隐藏的 View 不触发该槽位录制。

当前实现使用单图形队列，逐帧等待 `queue.waitIdle()`，性能上限受此同步点约束。应用需要自己管理队列、Swapchain 或整个窗口的帧循环时，应直接以 Cacao 作为宿主，而不是把这些操作放进 `CustomRenderHandler`。

## 当前验证与后续工作

下列 GPU 和 Android 记录保留其原有验证范围。2026-10-01 的完整 Zyn AOT
构建及 recomposition 无头自测另行验证了组合布局与状态契约，没有重跑本页
所述的设备画面检查；不能用无头自测替代 Cacao 内容的实际显示验收。

- Windows D3D12 与 Vulkan 示例已验证：两个槽位的真实 GPU 三角形可见、普通按钮正常绘制，第二个槽位在滚动容器内被正确裁剪，回调跨 DLL 执行，应用退出时释放资源。示例的默认短帧运行同时检查槽位回调、按钮命中及滚动容器之外的命中拒绝；`--visible` 可人工检查画面。设置 `CACAO_BACKEND=vulkan` 可选用 Vulkan。
- Android 10 x86_64 模拟器上的 Vulkan 示例已验证：APK 包含应用着色器，两个槽位实际绘制彩色三角形，滚动容器正确裁剪；切到后台再返回前台后进程和画面保持正常。可用 `Zyn/tests/android/Run.ps1 -CustomRender -DeviceSerial emulator-5554` 重跑，该门检查 APK 素材、两个渲染回调标记以及恢复前后截图中的三角形颜色。手动将模拟器尺寸改为 1200×1600、密度改为 300 dpi，进程未重启，两个画面继续显示；测试后已恢复 1080×2400、420 dpi。
- 仍需覆盖设备丢失后的重建设备路径和旋转变换。当前接口没有应用级 Cacao 必需特性声明；需要光追或其他可选硬件能力时，还需增加明确的特性协商与失败诊断。
- 尚未提供异步队列、多输出、跨 View 显式依赖、MSAA resolve、HDR 和独立透明度契约。逐帧 `waitIdle` 与目标回收应随帧栅栏改造。不要将这些能力视为现有接口的隐含保证。

`.zyns` 只记录应用动作与状态，不序列化 GPU 句柄。无头重放不执行渲染回调；视觉重放会重新绘制，所以渲染回调不应执行不可重放的业务副作用。

# 自定义 View

`Component.view(world)` 返回的是声明式 `View`。普通控件可以写一个返回 `View` 的函数来组合现有元素；需要自行绘制曲线、时间轴或图表时，使用 `CanvasView`。它仍参与 Zyn 的布局、命中测试、动作分发、无障碍节点和重建流程。容器内容测量、条件子项和 surface 外观的完整契约见 [View 组合与布局](COMPOSITION_ZH.md)。

## 绘制对象

实现 `Zyn.UI.CustomView`，再通过 `Zyn.App.CanvasView` 放进组件树。`paint` 收到布局后的逻辑坐标边界和 Nut 的 `SceneBuilder`，返回绘制后的 builder。这个接口目前负责二维绘制。`dup` 必须创建独立副本；View、布局树和运行时快照会分别持有该对象，不能共享可变的裸指针。

```vyx
use CustomView = Zyn.UI.CustomView;
use CanvasView = Zyn.App.CanvasView;
use Color = Zyn.Nut.Types.Color;
use Rectangle = Zyn.Nut.Types.Rectangle;
use Paint = Zyn.Nut.Scene.Paint;
use SceneBuilder = Zyn.Nut.Scene.SceneBuilder;

class BarView : CustomView {
    public fraction: f32;

    public fn paint(&self, scene: SceneBuilder, bounds: Rectangle) -> SceneBuilder {
        return scene.drawRect(
            Rectangle.xywh(bounds.x, bounds.y, bounds.width * self.fraction, bounds.height),
            Paint.fill(Color.rgba(0.2, 0.7, 0.9, 1.0))
        );
    }

    public fn dup(&self) -> Box<dyn CustomView> {
        return Box::<dyn CustomView>.new(BarView { fraction: self.fraction });
    }
}

// 在 Component.view(world) 中：
return CanvasView.create("meter", Box::<dyn CustomView>.new(BarView { fraction: 0.6 }))
    .size(240.0, 80.0)
    .accessibility("image", "Level meter", "");
```

`CanvasView` 默认高 160 个逻辑像素；实际应用应设置 `.size(...)`、`.height(...)` 或按容器约束布局。`Column` 默认横向拉伸子项，需要固定宽度时在容器上使用 `.alignStart()`。Zyn 在绘制时压入布局边界裁剪；`CustomView.paint` 必须保持传入 builder 的状态栈平衡，不调用 `clear`、`restoreAll` 或弹出外层裁剪。应用状态由 `Component.view(world)` 读取，再创建新的 `CustomView`；绘制函数只产生绘制命令，使同一状态在录制和重放时产生同一画面。

容器会递归测量这个默认高度及其他子项，不会从 `paint` 输出反推内容尺寸。
需要占满父容器高度时，先 `.autoHeight()` 清除默认固定高度，再
`.fillHeight()`；需要卡片样式时，将 CanvasView 放在
`Md3Surface.card(...).child(...)` 或带 `.elevation(...)` 的 Panel 内。
动态显示可用 `.childIf(...)`，仍由应用状态决定重建。

## 交互与语义

`CanvasView` 复用普通 View 的 `.onClick(...)`、`.onPointerDown(...)`、`.onPointerMove(...)`、`.onPointerUp(...)`、`.onPointerDrag(...)`、`.onPointerScroll(...)` 和 `.onKey(...)`。动作由 `AppWorld.registerBehavior` 或命令处理器接收。指针动作的 `AppAction.payload` 含 `CanvasPointer.fromAction(action)` 可解析的局部坐标 `x/y` 与移动量 `dx/dy`；先检查 `valid`。这些数值存入普通动作载荷，因而可以随 `.zyns` 记录与重放。

命中区域是布局后的矩形。拖动事件目前只在指针仍落在该区域内时送达；跨区域持续拖拽需要后续加入指针捕获。交互式自定义 View 应同时提供 `.accessibility(role, label, hint)`，并给 View 一个稳定 ID。`CustomView` 只负责画面；事件处理和资源状态仍在应用组件与 `AppWorld` 中。

完整可执行回归在 [`zyn_canvas_view_smoke`](../../samples/projects/zyn_canvas_view_smoke/src/main.vyx)，验证自定义 View 跨 DLL 调用、布局、裁剪、命中、动作载荷和销毁顺序。

## Cacao 自定义渲染

`CustomView.paint(SceneBuilder, bounds)` 是二维入口。需要 3D 或其他 GPU 绘制时，使用 [`CustomRenderView`](CUSTOM_RENDER_VIEW_ZH.md)：应用在 View 区域的离屏目标上录制 Cacao 命令，Nut 将结果与普通 UI 合成。它复用 View 的布局和动作绑定，但 GPU 处理器由 Renderer 持有，不放进可复制的 `View` 或 `AppWorld`。完整接口和 Windows D3D12 示例见链接文档。

# View 组合与布局

Zyn 的普通组件是返回 `View` 的 Vyx 函数或 `Component.view(world)`。
布局、外观、动作和语义信息放在同一棵声明式树中，由 App 绑定到
`AppWorld`，再生成 Nut 使用的 UI 树。可以先组合小控件，再把结果放进
`Column`、`Row`、`Panel`、`Grid` 或 `Wrap`。

## 条件子项

`childIf(condition, child)` 与 `child(child)` 使用相同的树和动作绑定路径。
条件为 false 时不加入子项；它不是独立的挂载或生命周期系统。

```vyx
use Column = Zyn.App.Column;
use Text = Zyn.App.Text;

// 在 Component.view(world) 中：
return Column.create().id("details").gap(8.0)
    .child(Text.title("Overview"))
    .childIf(world.boolResource("details.open", false),
        Text.body("More information").id("details.body"));
```

动作提交改变受观察的应用资源后，框架重新调用 `view(world)`。
子项应使用稳定 ID，使重建后的动作、焦点和语义信息仍对应同一业务对象。
`childIf` 的参数是普通 Vyx 值，调用前已经求值；昂贵的视图计算应放在
应用自己的 `if` 分支中。

## 高度、间距和嵌套容器

未设置正的 `.height(...)` 时，容器递归测量可见子项。

| 容器 | 默认内容高度 |
|---|---|
| `Column`、`Panel`、`Scroll` | 子项高度之和，加子项间 gap 和上下 padding |
| `Row` | 按分配后的子项宽度测量，取最大子项高度，加上下 padding |
| `Stack`、`Container` | 最大子项高度，加上下 padding |
| `Grid`、`Wrap` | 按列数或换行位置计算行高与 gap |

复合控件如 `FormField` 会为标签、输入框和错误行留下空间，再放置下一个
兄弟控件；无需给每个复合控件手工复制固定高度。`Panel.padding(...)`
仅在容器布局中应用一次。

`.height(64.0)` 指定固定高度并优先于内容测量；`.fillHeight()` 显式使用
父容器可分配的剩余高度。填充与内容测量是不同的选择：不要靠未设置高度
让一个卡片自动占满窗口。现有 `.flex(...)`、锚点和尺寸限制仍参与实际分配。

显式 `.gap(0.0)` 和 `.padding(0.0)` 保留为零。未调用这些修饰器时，
容器才使用自己的构造值或主题默认值。

布局单位是显示独立单位；App 将 SDL 像素尺寸与缩放映射到相同坐标系。
当前文本叶节点的高度仍来自主题字号和间距，未把实际字形换行结果反馈到
容器测量。字体回退、段落换行、字素编辑和动态文字缩放的完整闭环仍需补齐。

## Surface 与 elevation

`View.elevation(value)` 把逻辑单位的深度值传到 `UiNode`，再通过
`SceneBuilder.drawShadowRect` 或圆角路径的 `drawShadowPath` 生成软阴影。
正值控制阴影模糊、偏移与透明度；
零值不产生阴影。阴影遵守渲染路径的裁剪规则。

```vyx
use Md3Surface = Zyn.App.MD3.Md3Surface;
use Text = Zyn.App.Text;

return Md3Surface.card("summary")
    .child(Text.title("Summary"))
    .child(Text.body("Ready for review"));
```

当前 MD3 surface 默认值：

| 工厂 | 背景与描边 | 圆角 | elevation |
|---|---|---:|---:|
| `card` | surface，outlineVariant 描边 | 12 | 2 |
| `filledCard` | surfaceVariant，无描边 | 12 | 1 |
| `outlinedCard` | surface，outline 描边 | 12 | 1 |
| `primaryContainer` | primaryContainer，无描边 | 16 | 2 |

以上四个工厂均使用 24 padding、16 gap；应用可以继续调用修饰器覆盖它们。
核心 View 不要求 MD3，普通 panel 也可使用 `.elevation(...)`、
`.backgroundRole(...)`、`.noStroke()` 和 `.cornerRadius(...)` 组合外观。
这些值已连接绘制路径，但不代表完整 Material 规范或与其他 UI 框架的视觉一致性。

## 资源列表的稳定 key

`View.list(itemsResource)` 当前显示固定高度 checkbox 行。
`.keyResource(keysResource)` 读取与 label 列表位置对应的业务 key：

```vyx
use View = Zyn.App.View;

return View.list("tasks.labels").id("tasks")
    .keyResource("tasks.keys")
    .bindCheckedList("tasks.checked")
    .itemActionPrefix("tasks.toggle.");
```

若第 2 行的 key 为 `task-42`，绘制节点 ID 和 action source 都是
`tasks.task-42`，动作为 `tasks.toggle.task-42`。动作表、焦点 ID、
accessibility 节点沿用相同身份。插入或重排行时，应用同时重排 label、key
和 checked 列表；动作处理器按 key 查找业务记录。

每行应提供唯一、非空且稳定的 key。key 列表缺项或 key 为空时会回退到行索引；
这个兼容路径不能保证插入后的业务身份。框架目前不替应用校验重复 key，也不
自动按 key 迁移 checked 列表的数据。

列表只把可见范围附近的固定高度行加入绘制树。任意组件作为 row builder、
可变行高、复杂树表与可编辑 DataGrid 尚未由这个接口提供；当前 `DataGrid`
仍是文本和布局控件的组合。

资源列表会在绘制时读取 `.scrollOffset(resource, extent)` 绑定的资源。
普通 `Scroll` 当前仍读取 `.scrollY(...)` 的值；使用它时需在 `view(world)`
中同步世界 offset，不能假定仅设置 `scrollOffset` 就会移动内容。

## 验证与边界

2026-10-01：使用从当时源码构建的 SDK 编译器完整完成 Windows AOT Zyn 构建
（`-j4 -O0`，177 个任务，包含 root chunks）；同一编译器构建的
`zyn_recomposition_smoke --self-test` 返回 `0`。

该无头回归覆盖：命令后的结构变化、两次时间线重放、elevation 写入 UI 节点、
Panel padding 一次应用、复合表单内容高度、显式固定/填充高度、列表 key
插入重排后的节点和动作一致性，以及两组显示密度下的布局和点击坐标。
它不打开窗口，也不检查实际阴影截图、字体覆盖或平台辅助技术桥。

重跑命令及录制示例见
[`zyn_recomposition_smoke`](../../samples/projects/zyn_recomposition_smoke/README.md)。
运行时、控件和平台的使用边界见 [Zyn 说明](../README.md#current-limits)。

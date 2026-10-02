# 网站视觉与素材

- `public/vyx.png`：仓库原始语言图标，与 VYX 字标共同用于导航。
- `src/TechnicalGlyph.vue`：代码生成的 V 形三角网格，作为两页共同的线框图形；不使用外部模型或图片。
- 字体：Oswald 用于英文展示标题，Manrope 用于正文，JetBrains Mono 用于代码与技术标记。字体随网站构建输出；中文使用系统字体。
- `src/style.css`、`src/mosp.css`：纸白、近黑与亮蓝；直线网格、分栏和统一的按钮、代码区、标签样式。
- `src/motion.css`、`src/usePageMotion.js`：滚动入场、线框、编译阶段、DCI 流程、标签过渡与阅读进度。
- `src/FeatureShowcase.vue`：源代码展示和 DCE 流程示意。示意数字不代表实际性能。
- `src/SourceCode.vue`：MOSP 代码区的语法高亮与行号；展示标记不写入复制的源码。

代码阅读区没有循环位移动画。导航栏可以暂停动态效果，偏好在两页间保留。
系统开启减少动态效果时停止动画与过渡，编译流程直接显示完成状态。
网站不依赖 WebGL；静态内容和链接可正常使用。

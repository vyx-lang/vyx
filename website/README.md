# Vyx 展示站

[文档目录](../docs/README.md) · [MOSP](../docs/MOSP_ZH.md) · [DCI 规范](../docs/DCI_SPEC_ZH.md)

Vyx 项目网站，使用 Vue 3 + Vite。分为三个页面：

- `index.html`：语言介绍、hello 程序、四项特性的简短示例和 SDK 下载。
- `mosp.html`：通过四个 tab 分别介绍 Migrate、Reflection、DCI、DCE，默认打开 DCI。
  DCI 重点讲解契约、原始 `std::vector<T>`、真实 C++ / Rust 库、生命周期与异常边界；
  Migrate 提供两版模块与历史调用，Reflection 提供名称查询、字段读写与方法绑定，
  DCE 简述入口、依赖追踪和代码裁剪。
- `tutorial/index.html`：入门、进阶、高级特性与迁移对照。内容在构建时从双语文档生成，
  按章加载，使用 `#章节/小节` 深链接。桌面使用侧栏，窄屏使用章节与小节选择器。

三页均提供中文、英文和明暗主题，切换时保留偏好。
视觉使用黑白排版、亮蓝分区和统一的技术图示。MOSP 的 DCI 内容提供随阅读位置更新的
页内目录；示例带语法高亮与行号，复制时保留原始源码。滚动入场、编译阶段、DCI 调用流程
与标签切换共享动效规则；导航栏可暂停，系统减少动态效果时直接显示静态内容。
首页的立体 ASCII 图形使用 WebGL，支持指针响应；离屏或后台时停止渲染。
暂停保留当前姿态，减少动态效果使用固定姿态，WebGL 不可用时显示静态字符图形。
教程切章后回到页顶，小节跳转同时移动阅读位置与键盘焦点。

## 本地开发

需要 Node.js 22.12 或更高版本。

```sh
cd website
npm ci
npm run dev
```

首页地址为 http://127.0.0.1:5173/ ，MOSP 页面为
http://127.0.0.1:5173/mosp.html ，教程为 http://127.0.0.1:5173/tutorial/ 。

## 构建与预览

```sh
npm run build
npm run preview
```

Vite 以三个 HTML 文件作为构建入口，输出到被 Git 忽略的 `website/dist/`。
部署脚本会把这个目录整体上传，其中包括 `index.html`、`mosp.html`、`tutorial/` 与 `assets/`。
资源使用相对路径，可部署在子目录，无需配置 SPA 路由回退。

## 部署

`deploy.py` 把站点发布到生产服务器：**构建 → 上传 → 校验**。
它不涉及 nginx 与证书配置——那部分已经在服务器上配好，站点根固定为
`/data/www/www.vyxlang.com`。

```sh
python deploy.py                 # 构建 + 上传 + 校验，日常就用这一条
python deploy.py --skip-build    # 只上传现有 dist/
python deploy.py --prune         # 顺带删掉远端多余的旧 hash 资源
python deploy.py --dry-run       # 只打印将要做什么
```

也可以用 `npm run deploy`。脚本用 paramiko 走 SFTP，不需要 rsync / sshpass；
PATH 上的 `python` 若没有 paramiko，会自动改用 `D:/miniconda3/python.exe` 重跑。

凭据按 命令行参数 > 环境变量 > `website/.deploy.env` 的顺序读取，键为
`VYX_SSH_HOST`、`VYX_SSH_PORT`、`VYX_SSH_USER`、`VYX_SSH_PASS`、
`VYX_DEPLOY_ROOT`、`VYX_DEPLOY_DOMAIN`。`.deploy.env` 被 `.gitignore` 的
`.*` 规则忽略，不要把密码提交进仓库。

校验会比对远端文件与本地 `dist/` 的体积，再从服务器上对源站
（`--resolve` 到 `127.0.0.1`）与公网域名各请求一次 `/`、`/mosp.html`、
`/vyx.png` 和最大的那份 JS，全部返回 200 才算通过。

## 内容维护

| 文件 | 内容 |
|---|---|
| `index.html`、`src/main.js`、`src/App.vue` | 首页入口、布局与安装弹窗 |
| `mosp.html`、`src/mosp-main.js`、`src/MospPage.vue` | MOSP 独立页入口与布局 |
| `src/MospDciPanel.vue`、`src/MospFeaturePanel.vue` | DCI 详细介绍与其他特性的内容面板 |
| `src/TechnicalGlyph.vue`、`src/asciiGlyph.js`、`src/usePageMotion.js` | WebGL 字符图形、页面时钟、滚动入场和动效偏好 |
| `src/TutorialPage.vue`、`src/TutorialBlocks.vue`、`src/tutorial.css` | 教程导航、正文与响应式排版 |
| `scripts/build-tutorial.mjs`、`src/tutorial-chapters.js` | 双语文档转换与章节映射；生成内容被忽略 |
| `src/SourceCode.vue`、`src/sourceHighlight.js` | MOSP 与教程示例的语法高亮、行号与滚动阅读 |
| `src/pageTransition.js` | 独立页面之间的切换，遵循暂停与减少动态效果偏好 |
| `src/content.js` | 中英文通用文案、仓库地址与 Release 下载链接 |
| `src/features.js` | 首页四项特性的说明与示例 |
| `src/dci-examples.js` | DCI 继承、覆写与 Rust / C++ 开放泛型示例，分别对照 `dci_multilang` 与 `dci_opengeneric` 测试 |
| `src/mosp-content.js` | MOSP 独立页通用说明、DCI 代码与参考链接 |
| `src/mosp-features.js` | Migrate、Reflection、DCE 三个 tab 的中英文说明、示例与参考链接 |
| `src/FeatureShowcase.vue` | 特性与文件切换、DCE 流程展示 |
| `src/style.css`、`src/motion.css`、`src/mosp.css` | 公共样式、动效与 MOSP 页面样式 |
| `vite.config.js` | 三页面构建入口、教程生成与相对资源路径 |
| `deploy.py`、`.deploy.env` | 发布脚本与本地凭据（后者被 Git 忽略） |

### 教程内容

教程正文维护在 `docs/`，网站使用以下中英文配对：

| 章节 | 中文 | English |
|---|---|---|
| 入门 | [入门指南](../docs/入门指南_ZH.md) | [Getting started](../docs/TUTORIAL.md) |
| 进阶 | [进阶教程](../docs/进阶教程_ZH.md) | [Intermediate tutorial](../docs/INTERMEDIATE_TUTORIAL.md) |
| 高级特性 | [高级特性](../docs/高级特性_ZH.md) | [Advanced features](../docs/ADVANCED_FEATURES.md) |
| 迁移对照 | [迁移指南](../docs/快速迁移_Rust_CPP.md) | [Migration guide](../docs/MIGRATING_FROM_RUST_CPP.md) |

修改正文时同步双语文档；章节配对与合并关系在 `src/tutorial-chapters.js` 中维护。
`@[vis]` 的范围与组合规则在入门第 8 课，多文件访问示例在进阶第 19 课。
开发服务和构建会自动生成教程内容，`src/tutorial/content/` 下的生成物不提交。
在 `website/` 中可单独检查章节配对：

```sh
node scripts/build-tutorial.mjs --check
```

### SDK 与示例

SDK 链接使用本仓库
[Releases latest](https://github.com/vyx-lang/vyx/releases/latest)
下的 `vyx-sdk-windows-x86_64-llvm22.zip` 和
`vyx-sdk-linux-x86_64-llvm22.tar.gz`，不依赖源码树内的 `dist/`。
Windows 与 Linux SDK 均已内置 LLVM 后端。

语言内容对应 [项目 README](../README.zh-CN.md)、
[入门教程](../docs/入门指南_ZH.md) 和 [MOSP 文档](../docs/MOSP_ZH.md)。
DCI 示例需要契约、原语言工具链与链接配置，页面中的代码应指向完整项目。
首页的 DCE 面板是流程示意，数字不代表实测结果。

MOSP 页中的具体验证范围见：

- [原始 vector](../probes/gates/dci-vector/README.md)：调用方选择 `i32` / `f64`，
  构建请求原始模板实例；包含增长、借用访问、析构释放与越界异常检查。
- [C++ 生态](../probes/gates/dci-cpp-ecosystem/README.md)：链接 ICU 78.3 的原始
  `UnicodeString`，与独立 C++ 程序比较结果。
- [Rust 生态](../probes/gates/dci-rust-ecosystem/README.md)：通过 locked Cargo
  依赖、实测 slice view 和生成的 native bridge 调用 `crc32fast` / `adler2`。
  支持范围由签名、目标及生产端工具链限定。
- [DCI 规范](../docs/DCI_SPEC_ZH.md)：对象存储释放、共享异常 ABI 和拒绝条件。

当前以 AOT 为开发和验证基准，JIT 对齐是后续工作。
版本、平台和库的描述以具名夹具为依据。品牌与素材说明见 [ASSETS.md](ASSETS.md)。

## 页面检查

```sh
npm run build
npx playwright install chromium
npm run check:ui
npm run check:motion
```

检查包括三页的桌面与移动布局、语言与主题、代码复制、首页特性切换、
MOSP 四个 tab、DCE 流程、页面导航、安装弹窗、文档入口和流程动画暂停。
动效检查会采样动画的实际变化，并检查阶段顺序、暂停恢复、快速切换与减少动态效果。
教程检查覆盖小节跳转、重复点击、切章回顶、浏览器历史、键盘焦点、手机选择器、
`@[vis]` 课程和源码链接。字符图形检查比较实际像素，验证旋转、暂停与静态回退。
截图写入被 Git 忽略的 `artifacts/`。

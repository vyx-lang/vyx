# AGENTS.md — Vyx 项目接手指南

本文件是整个仓库的 agent 指令入口，适用于编译器、DCI、Zyn、IDE 插件和网站。
各目录 README 负责操作细节，`docs/` 负责规范；不要再创建分散的 `Agents.md` 副本。
用户本轮的任务、范围和约束优先于历史方案。

## 接手顺序

1. 阅读本文件，然后查看 `git status --short`、当前分支和最近提交。保留已有未提交改动。
2. 从下表找到任务的源码与验证入口，先读对应 README 和涉及的代码。
3. 确认实际工具链、已有产物的来源和相关探针基线，再修改源码。
4. 用本次构建的产物验证，交接时写明执行命令、结果、跳过项和剩余问题。

从用户当前工作树继续，以当前源码与门 A/B/C 的结果为基线。
禁止以旧 `release` 或本地 seed 工程代替当前主树开展重构。

## 目录与事实归属

| 范围 | 源码入口 | 先读 / 验证入口 |
|---|---|---|
| 自举编译器 | `bootstrap_compiler/src/{core,hir,mir,codegen}/` | [编译器 README](bootstrap_compiler/README.md)、[验证指南](docs/TESTING_GUIDE_ZH.md) |
| LLVM 桥接与运行时 | `vyx_codegen/`、`runtime/` | `bootstrap_compiler/Vyx.toml`、验证指南 |
| 标准库 | `bootstrap_compiler/std_packages/`、`std_packages/`；`bootstrap_compiler/std/` 为兼容入口 | [标准库参考](docs/STD_LIBRARY.zh-CN.md) |
| DCI 生产端与契约工具 | `tools/dci/`；消费端在自举编译器 | [DCI 规范](docs/DCI_SPEC_ZH.md)、[MOSP](docs/MOSP_ZH.md) |
| 回归与压力测试 | `tests/`、`probes/gates/` | 各夹具 README / `run.ps1` |
| Zyn UI 框架 | `Zyn/`、`samples/projects/zyn_*` | [使用说明与边界](Zyn/README.md)、[组合与布局](Zyn/docs/COMPOSITION_ZH.md) |
| IDE 插件 | `bootstrap_compiler/IDE-Plugins/` | [插件说明](bootstrap_compiler/IDE-Plugins/README.md)、[工具指南](docs/TOOLING_ZH.md) |
| 官网 | `website/` | [网站说明](website/README.md) |
| 文档 | 根目录 README、`docs/`、组件 README | [文档索引](docs/README.md) |

## 通用规则

- 当前编译器和 DCI 验收以 **原生 AOT** 为基准；JIT 对齐与性能是后续工作。
- 修复必须覆盖真实使用路径。不能把单个能力探针通过写成工业级目标已经完成。
- 编译器语义修改落在 `bootstrap_compiler/`、`vyx_codegen/`、`tools/dci/`；
  探针落在 `probes/gates/<任务>/`，项目夹具落在 `tests/projects/`。
- repo-root `src/` 是冻结的 C++ host，repo-root `std/` 是冻结的旧标准库。
  不复活旧 host，不为其旧 IR 快照添加编译器特例。
- 不新增 fail-open：不能用名字 replace/join、末段模糊匹配、null/rawptr 万能相容、
  `{i8}` 假布局或 `_silent` 吞诊断掩盖缺口。类型、布局或生命周期未知时要报告错误。
- MIR2CPP / `--emit=cpp` 不作为自举或发布验收门；`--legacy-codegen` 已拒绝。
  不用编译器源码子集之外的冻结特性（async/comptime/derive 等）充当自举验收。
- 保持 MIR 按函数验证再丢弃的 streaming 路径，不为 whole-unit 二次扫描恢复大内存模型。
- 类型收口与自举冲突时，记录具体 concession 和探针证据，不破坏固定点。
- 新增标准库、公开 API 或 DCI 映射应进入真实项目；错误必须有可定位的诊断。
- 已授权并行工作时，worker 的允许写路径必须两两不相交。
  同一大文件上的不同任务先机械拆分，再并行；父 agent 检查实际 diff 和门结果。
- 不覆盖其他人的改动，不提交无关文件，不强推，不 amend 已推送提交。
  回滚只针对自己的修改；提交前检查 `git diff --cached`。

## 编译器：构建和验收

### 工具链身份

Release SDK 的 `vyxc` 作为 **Stage 0** 构建本树 SDK 编译器；被测编译器必须来自这次构建。
各阶段的 SDK 编译器必须配合同次构建的 `vyx_compiler_backend` / `vyx_runtime`。
现成的 `out/vyxc` 或本地 seed 文件不能证明当前源码通过了验证。
记录 Stage 0 的路径、版本、哈希，以及新构建的 SDK 编译器身份。

Windows / Linux SDK 已内置 LLVM 后端，普通 SDK 使用者无需另装 LLVM。
从源码构建 LLVM 桥接仍需 LLVM 22 开发工具和库；Windows 默认使用仓库本地 `clang/`，
其他位置显式设置 `LLVM_ROOT`。环境细节以[验证指南](docs/TESTING_GUIDE_ZH.md)为准。

### Windows

从仓库根目录执行。先让 PATH 中的 `vyxc` 指向兼容的 Release SDK：

```powershell
$env:VYX_BOOTSTRAP_VYXC = (Get-Command vyxc -ErrorAction Stop).Source
& $env:VYX_BOOTSTRAP_VYXC --version
Get-FileHash $env:VYX_BOOTSTRAP_VYXC -Algorithm SHA256
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
$compilerTarget = [regex]::Match((Get-Content .\bootstrap_compiler\Vyx.toml -Raw), '(?m)^name\s*=\s*"([^"]+)"').Groups[1].Value
Push-Location .\bootstrap_compiler
try {
    & $env:VYX_BOOTSTRAP_VYXC build --target $compilerTarget -j4
    if ($LASTEXITCODE -ne 0) { throw 'SDK compiler build failed' }
    Copy-Item -LiteralPath ".\out\$compilerTarget.exe" -Destination .\out\vyxc.exe -Force
} finally { Pop-Location }
$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path
```

### Linux

从仓库根目录执行：

```bash
export LLVM_ROOT=/usr/lib/llvm-22
export PATH="$LLVM_ROOT/bin:$PATH"
cd bootstrap_compiler
compiler_target=$(awk -F '"' '/^name[ \t]*=/{print $2; exit}' Vyx.toml)
vyxc build --target "$compiler_target" -j"$(nproc)"
cp "out/$compiler_target" out/vyxc
LD_LIBRARY_PATH=out ./out/vyxc help
```

### 门 A / B / C

编译器修改前后都跑门 A 和 B。DCI / class ABI / lowerer 修改另跑门 C。
纯文档、网站或忽略规则修改检查相关内容与产物范围，无需重跑编译器自举。

- **门 A — hello**：使用含 `print("hello")` 和 `return 0` 的程序运行 `--dump-mir2`。
  历史计数是 `functions=1 blocks=1 locals=1 places=1 values=6 instrs=2 cases=0 types=5`。
  先记本树基线，修改后解释所有漂移，尤其 `values=6 instrs=2`。
- **门 B — self-host fixpoint**：Stage 0 构建 S1，S1 构建 S2，S2 构建 S3，要求 S2 == S3。
  若元数据抖动，明确记录原因，再用固定语料的 IR 哈希验证；不能静默换门。
- **门 C — DCI E2E**：运行 `dci_cpp_trait`、`dci_rust_trait`、`dci_multilang`、`dci_zig_abi`
  项目及相关 `probes/gates/dci-*`。不得以 Vyx FFI E2E 或 MIR 验证替代。
  `dci_spdlog` 夹具已退役，不复活旧 `run.sh` 门。

Windows 固定点脚本和 DCI 项目扫描（从仓库根目录）：

```powershell
.\bootstrap_compiler\scripts\test_project_selfhost_fixpoint.ps1 `
    -SeedCompiler $env:VYX_BOOTSTRAP_VYXC -LlvmRoot $env:LLVM_ROOT -Target $compilerTarget -HelloGate
.\tests\run_all_modules.ps1 -BootstrapCompiler $compiler -BootstrapOnly -PathFilter 'projects\dci_'
```

压力与生态验证入口：

- [编译器压力门](probes/gates/compiler-industrial/README.md)：冷构建、热缓存、增量、串行/并行一致性。
- [DCI 压力门](probes/gates/dci-industrial/README.md)：真实 C++ / Cargo / 多语言夹具，重复运行与资源测量。
- [异常模型](probes/gates/dci-exceptions/README.md)：传播与清理；shared_abi 的 Linux 路径单独验证。
- [跨模块](probes/gates/cross-module/README.md)、[match 表达式](probes/gates/match-expression/README.md)、
  [泛型接口](probes/gates/generic_interfaces/README.md)：按修改范围加入回归。

每个门必须记录实际编译器和结果。`exit=77` 是跳过；仅编译成功不等于运行成功。
旧测量、空测量和未执行的平台不能写成当前验证通过。

## Zyn

Zyn 按底层到应用推进。只有真实依赖项目构建并运行后，才能报告一层完成。

- Platform 负责 SDL3 窗口、事件、输入、IME、剪贴板、时钟、显示、应用路径、OS 消息、
  文件系统、手柄、触摸、帧时钟和生命周期。
- Cacao 是 Nut 使用的 RHI；调用有类型的封装，不直接访问原始 backend handle。
- Nut 负责渲染图、Canvas/path、CPU atlas 与像素存储、图片元数据、批处理、裁剪和 GPU 帧。
  实现放在 `Zyn/src/nut/` 子模块，`Zyn/src/Zyn.vyx` 只负责框架组装。
- 应用通过 `Zyn/bin/zyn.ps1`、`zyn.cmd` 或 `bin/zyn` 构建，入口设置 `ZYN_SDK_ROOT`
  并部署运行资产。编译器回归时显式设置 `ZYN_VYXC` 指向本次构建的 SDK 编译器。
- 新公开类型至少进入一个真实 smoke 项目。绘制 API 必须连接到实际 GPU 工作。
- 裁剪或 transform 命令必须同时作用于 solid 和 textured 路径，否则明确拒绝。
- 图片用二进制读取；禁止文本读取路径截断 NUL 字节。
- 图片解码使用私有 `Zyn/vendor/stb/` 和 `stb_impl.c`，不依赖或修改根 `std/stb_image.vyx`。
  字体链接与加载使用 `Zyn/vendor/FreeType/`、`Zyn/vendor/HarfBuzz/` 的私有副本。
- 可失败操作返回显式 `Result`，不吞 backend 错误。
- 每个获得的 SDL3 / Cacao 资源恰好销毁一次，公开 handle 重复销毁保持无害。
- 不新增 Vyx `Thread` API；调度通过帧时钟、coroutine 和 async 集成表达。

Platform / Nut 修改后，构建框架与 `zyn_platform_smoke`、`zyn_render_smoke`，并运行可执行文件。
组件、布局和状态组合修改另跑 `zyn_recomposition_smoke --self-test`。
例如在仓库根目录，已获得 `$compiler` 后：

```powershell
$env:ZYN_VYXC = $compiler
Push-Location .\Zyn
try {
    & .\bin\zyn.ps1 build --target Zyn -j4
    if ($LASTEXITCODE -ne 0) { throw 'Zyn build failed' }
} finally { Pop-Location }
foreach ($sample in @('zyn_platform_smoke', 'zyn_render_smoke')) {
    Push-Location ".\samples\projects\$sample"
    try {
        & ..\..\..\Zyn\bin\zyn.ps1 build -j4
        if ($LASTEXITCODE -ne 0) { throw "$sample build failed" }
        & ".\target\$sample.exe"
        if ($LASTEXITCODE -ne 0) { throw "$sample run failed" }
    } finally { Pop-Location }
}
```

具体运行参数看各样例 README。未运行、缺平台依赖或失败的门要写明，不能报告完成。

## IDE 插件

`bootstrap_compiler/IDE-Plugins/vscode/vyx/` 和 `IDEA/vyx/` 的源码随主仓库维护，
作为普通目录提交，不引入嵌套 Git 仓库或缺少 `.gitmodules` 的 gitlink。
保留 Gradle wrapper、真正扩展项目的 `package-lock.json`、`.gitattributes`、`.vscodeignore`。
依赖、IDE 状态、`build/`、`out/`、VSIX / ZIP / JAR 产物忽略；wrapper JAR 是明确例外。

- VS Code / Cursor：在 `vscode/vyx/` 执行 `npm ci`、`npm run compile`。
- IntelliJ / CLion：在 `IDEA/vyx/` 用 `gradlew.bat buildPlugin -PvyxIdePath=<本机IDE目录>`；
  平台与 JDK 要求查该目录的 README 和 `build.gradle.kts`，不提交本机 IDE 路径或 SDK 缓存。
- LSP / DAP 行为在 `bootstrap_compiler/src/core/lsp_main.vyx`、`dap_main.vyx`。
  验证插件时配套使用同一次构建的语言服务器和调试适配器。

## 网站与文档

- 网站是 `website/` 中的 Vue / Vite 项目，主页和 MOSP 页共用样式与交互。
  修改后执行 `npm run build`，界面与动效改动执行 `npm run check:ui`、`npm run check:motion`。
  页面效果须在浏览器检查，包括窄屏、主题、键盘操作与 reduced motion。
- 文案陈述已实现能力与边界，不用未执行的性能数据或路线图承诺替代事实。
- 文档统一称 SDK 编译器，命令入口为 `vyxc` / `vyxc.exe`；源码验收使用当前构建的入口与配套运行时。
- 语言规则与 DCI 契约以相应规范为准。双语说明保持一致，更新链接和组件索引。
- 新接手信息放在本文件或它链接的主题文档；不复制整套 agent 指令到其他目录。
- 任务结束后删除临时计划、分工清单和重复阶段总结。长期使用的构建步骤、API 约束和
  未实现边界维护在对应 README 或规范中；历史过程从 Git 查，不另存一套 AI 任务档案。

## 本地文件与提交范围

- `bootstrap_compiler/seed*`、`deprecated_seeds/` 是本地 seed 工程与二进制；不提交。
- 自举目录顶层 `test*.vyx` 是临时试验；正式测试放 `bootstrap_compiler/tests/`、`tests/`
  或 `probes/gates/`，这些源码应保持可跟踪。
- `out/`、`target/`、缓存、日志和插件包是生成物。不要用全目录忽略来隐藏正式源码。
- 忽略已经跟踪的本地产物时，用 `git rm --cached` 停止跟踪，保留磁盘文件。
  提交前用 `git check-ignore` 与 `git diff --cached --name-only` 核对范围。

## 交接记录

交接说明至少包含：

1. 本次目标、完成的修改与对应文件。
2. 当前分支、提交和工作树里仍未提交的改动归属。
3. 真实执行的验证命令、使用的编译器身份、通过 / 失败 / 跳过结果。
4. 剩余缺口、具体复现入口、下一步和平台限制。

固定操作写入主题 README；一次运行的日志和临时路径留在本地结果目录。
接手者应能据记录复现结论，不依赖前一位 agent 的聊天历史。

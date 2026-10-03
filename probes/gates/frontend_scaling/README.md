# 前端导入扩展性探针

本目录测量单个编译进程处理多个接口时的成本，不把 LLVM 并发数或构建调度器吞吐混入结果。每个接口只有一个 `computeNNN(i64) -> i64` 声明，主文件导入并调用全部接口；不能改成只调用第一个模块。

## 重现

先从当前源码构建 SDK 编译器，并使用同次构建的 backend/runtime；从仓库根目录运行：

```powershell
$compiler = (Resolve-Path ./bootstrap_compiler/out/vyxc.exe).Path
pwsh -NoProfile -File probes/gates/frontend_scaling/Measure-Fanout.ps1 -Compiler $compiler -Modules 8
pwsh -NoProfile -File probes/gates/frontend_scaling/Measure-Fanout.ps1 -Compiler $compiler -Modules 16
pwsh -NoProfile -File probes/gates/frontend_scaling/Measure-Fanout.ps1 -Compiler $compiler -Modules 32
```

每次运行在 `.runs/` 保存完整输入、接口、编译器 SHA-256、参数、stdout、stderr、100 ms 进程采样和结果 JSON。默认单进程、单 LLVM CGU、30 秒超时、2 GiB private-memory 停止阈值。阈值是探针的保护机制，不是编译器的资源保证；采样可能超过阈值后才停止。`-NoImplicitStd` 用于排除隐式标准库注册表扫描成本。

## 修改前的观测

被测 SDK 编译器是由仓库源码构建的原生自举产物 `70731A1F95454406343EE7D0F1858C01DA7C3FFA9EC31BEA956502282999C3C0`。以下为单次历史诊断样本，不是正式吞吐基准。

| 模块数 | 主源码字节 | Wall | 函数体语义分析 | Clone 调用 | 结果 |
|---:|---:|---:|---:|---:|---|
| 8 | 579 | 0.612 s | 374 ms | 1,017,529 | 成功 |
| 16 | 1,044 | 4.524 s | 3,779 ms | 7,138,237 | 成功；RSS 650.2 MiB，private 721.3 MiB |
| 32 | — | 13.836 s 后停止 | 尚未完成 | 无完整退出计数 | 达到探针 2 GiB private 阈值，非成功样本 |

8 模块关闭隐式 std 后仍有 372 ms 的函数体语义分析，总耗时 0.524 s。默认 8 模块第一版脚本在进程退出后读取峰值，未获得有效 OS 峰值，因此不使用该 JSON 的空峰值字段；编译器自己的 MIR 前采样报告 working 168 MiB、private 242 MiB。16/32 模块已改为保留运行期间观察到的 OS lifetime peak。

16 模块进入 HIR 时只有 50 items、17 functions、16 locals、2 types，却已经占用 626 MiB working set。进入 MIR 时字符串长度表达到 7,103,903 entries，AST duplication、AST node 和 raw duplication 的 MiB 计数均显示 0，字符串 arena reserved 32 MiB。LLVM lowering 是毫秒级。因而这个输入的主要内存放大在语义查询期间，不是 HIR/MIR 节点规模，也不是 LLVM 优化。

`Clone` 计数同时包含 native `vyx_string_clone` 和 `vyx_string_clone_len_abi` 两条 ABI，不能把它全部解释为 arena 分配。长度表条目数也是观察结果，不等于精确分配字节数。

原始诊断运行目录：

- `.runs/20260927-164329-741-n8`
- `.runs/20260927-164408-842-n8`（关闭隐式 std）
- `.runs/20260927-164409-456-n16`
- `.runs/20260927-164414-025-n32`（主动停止）

## 明确的重复查询路径

生产文件：`bootstrap_compiler/src/core/sema.vyx`。

1. `sema_lazy_load_pending_until_symbol` 遍历 `pending_imports` 文本。
2. 原实现在**每个字符**后调用 `sema_symbol_known(name/base/short)`，而只有行末 `sema_load_module_symbol` 才可能加载声明、改变符号表。
3. 普通名称的 `name == base == short`，原代码仍重复查询三次。
4. 未命中的函数查询继续进入 `sema_find_class_symbol`，然后进入 `sema_expand_short_name`。
5. 短名展开再次遍历全部 `use` 前缀，经 `sema_named_in_module`、`sema_module_exports_name` 等查询产生临时字符串克隆。现有模块/符号索引只能降低单次查询成本，不能消除外层逐字符重复。

这里存在“待加载模块文本长度 × 可见 use 数 × 待查符号数”的乘积。8 到 16 模块时 Clone 增长约 7 倍，与这个放大结构一致；真正的因果验收仍要求修改后使用相同输入比较。

首批补丁仅做两件事：将已知符号检查移到一个完整非空模块名的加载组之后；通过 `sema_symbol_variants_known` 去掉相同拼写的重复查询。导入顺序、加载尝试顺序、alias/prelude/registry fallback、负结果缓存和错误诊断路径保留。

## 语义回归

```powershell
pwsh -NoProfile -File probes/gates/frontend_scaling/Check-ImportResolution.ps1 -Compiler bootstrap_compiler/out/pipeline_final_s3.exe -Label baseline
pwsh -NoProfile -File probes/gates/frontend_scaling/Check-ImportResolution.ps1 -Compiler $compiler -Label patched -ReferenceResult probes/gates/frontend_scaling/.runs/import-resolution/baseline.json
```

八个用例覆盖：正序 use、逆序 use、函数别名、完全限定调用、循环 use、泛型实例、缺失名称、参数类型错误。成功用例比较完整 LLVM IR SHA-256；失败用例比较退出码与全部 error 行。修改前六个成功、两个按预期失败，错误分别包含 E2000 和 E1000。

所有 VYI 定义使用独立的 module 声明行和函数声明行。当前注册表扫描器对同一行 `module X; public fn ...` 的导出发现存在缺口；这不是本次查询频率修复的范围，也不能用这样的输入作为正确性基线。

## 后续结构改造边界

- 当前 Sema 已通过 loaded-path 集合避免同一分析中重复解析同一路径；不能将上述 7 百万 Clone 误报为反复解析同一个接口。
- 多个 codegen 子进程仍各自执行 registry → parse/import → Sema → HIR；跨进程共享只读编译快照尚未实现。这是另一层成本，需要独立计数每个 source/interface 的解析次数。
- 将 registry 文本变为模块 ID、符号 ID、源文件 ID 和 span 的只读索引，可使热路径查询不再依赖临时拥有型字符串。缓存必须绑定语义状态变化：符号表新增、导入集合变化、当前模块/alias scope 变化；不能只按名称永久记住 miss。
- 解析 AST、语义事实和懒加载状态尚可变。任何共享线程方案都应先冻结接口和类型事实，再将每个函数实例的工作区隔离；不能直接在当前 Sema 全局缓存上增加线程。

## 首轮新 SDK 编译器对照

新 SDK 编译器（原生自举产物）SHA-256：`5D55754D8E1C3784E3E6B2BF75A9DFCDF94A9578A80B93A80282B4E5D4FF44A1`。相同生成器、参数和 profile 开关；输入内容相同，运行目录独立。

| 模块数 | Wall | 函数体语义分析 | Clone 调用 | 编译器报告 working/private 峰值 |
|---:|---:|---:|---:|---:|
| 8 | 0.188 s | 14 ms | 32,389 | 93 / 167 MiB |
| 16 | 0.240 s | 62 ms | 163,477 | 113 / 188 MiB |
| 32 | 0.721 s | 455 ms | 1,013,557 | 196 / 269 MiB |

新 8/16 模块运行很短，100 ms 外部采样会遗漏退出前的峰值，因此这里统一使用编译器阶段日志中的 OS lifetime peak，整数 MiB。不能把新 16 模块脚本采到的 74.9 MiB 宣称为完整运行峰值。脚本现在另行记录这两项内置峰值，不篡改原始采样结果。

16 模块相对旧版本：总耗时 4.524 → 0.240 秒，函数体语义分析 3,779 → 62 ms，Clone 7,138,237 → 163,477，内置 working 峰值约 650 → 113 MiB。旧 32 模块主动停止，新版本成功完成；不能计算一个不存在的旧版完成时间加速比。

八个语义回归用例的成功 IR SHA-256、失败退出码和 error 行全部一致。证据在 `.runs/import-resolution/patched.json`；性能原始记录在 `.runs/20260927-170059-659-n8`、`.runs/20260927-170100-255-n16`、`.runs/20260927-170100-921-n32`。

残余 Clone 随模块数仍从 32k → 163k → 1.01M 增长，说明“每个名称扫描 use 集合”的结构成本仍然存在。共享不可变前端、索引化名称解析和完整自举固定点需分别验收；单一热点改善不等于工业级并行编译已经完成。

## 第二步：局部 import epoch

`sema_load_module_symbol` 的布尔结果不能代表所有语义修改：返回 true 可能只是已有符号；返回 false 也可能发生在 import passes 之后，只因 `sema_append_unit` 没有追加声明。它不是事务 mutation flag。

新增 `Sema.lazy_import_epoch` 在已有 `sema_lazy_symbol_miss_clear` 处递增。这两个真实调用点都位于加载/强制布局合并的 queue/pass/append 之前。仅在一个 pending 模块的同步加载组前后比较 epoch；入口、prelude、最终查询仍然无条件执行。它不作为全局 resolver memo 的失效协议。

```powershell
pwsh -NoProfile -File probes/gates/frontend_scaling/Check-ImportEpoch.ps1 -Compiler $compiler
pwsh -NoProfile -File probes/gates/frontend_scaling/Check-ImportResolution.ps1 -Compiler '<S2>' -Label epoch -ReferenceResult probes/gates/frontend_scaling/.runs/import-resolution/baseline-expanded.json
```

`Check-ImportEpoch.ps1` 摘录实际生产 lazy-load 控制流、distinct-name 查询与名称处理函数。测试适配器提供可控 loader/known 谓词，覆盖返回 false 但 facts 改变、完全无变化、返回 true 但无变化、嵌套 mutation、epoch 改变但名称仍未知。它明确是 **loader-contract unit test**，不是完整 Sema 集成测试，也不宣称正常用户工程触发过 null-unit 追加路径。原生 S1 编译并执行该控制流门 exit 0，记录在 `.runs/epoch-20260927-171702-309`。

真实接口回归已增加嵌套类型导入和嵌套 use alias，共十项；S1 基线为 `.runs/import-resolution/baseline-expanded.json`，八项成功、两项按预期诊断失败。第二步生产源码已交统一 S2 构建，其性能和完整集成对照由主线程继续验收。


## S2 epoch 集成验证

`37FBD96EF670275B445FEAE288AE2983CF4A56D62D02AF5E187CD77240972031` 的十项真实接口用例与 `baseline-expanded.json` 全部一致，结果为 `.runs/import-resolution/epoch.json`。相同生成器 8/16/32 模块均成功，wall 分别 0.181 / 0.210 / 0.343 秒，原始目录 `.runs/20260927-172338-802-n8`、`20260927-172339-084-n16`、`20260927-172339-342-n32`。它们是单次诊断，短进程的外部 100 ms 采样峰值仍可能低估；32 模块内置 OS working/private lifetime peak 为 139 / 213 MiB。

后续主要成本仍包括名称/registry 索引、跨进程前端重复与事实所有权。这个结果不替代自举固定点和真实项目吞吐验收。

# IR Regression Guard — Snapshots

本目录保存 **host 时代** 的 IR 回归基线。设计计划文档曾存在于
`docs/PLAN_IR_REGRESSION_GUARD.md`，当前仓库已不保留该临时计划；本 README、
`scripts/ir_diff.ps1` 和 `scripts/run_gates.ps1` 仅描述这套历史基线。

> 当前 checkout 以兼容 Release SDK（Stage 0）→ 从当前源码构建的 SDK 编译器为准。这里的
> golden 文件尚未迁移为该编译器的基线，因此不能把 `-Strict` 的结果作为当前语言
> 或 bootstrap 的验收结论。迁移时可显式传 `-Compiler <刚构建的 SDK 编译器路径>`
> 生成并评审新基线，并使用同次构建的后端与运行时；不得混写两代编译器的结果。

> **当前阶段：D6 已落地（industrial 全集纳入 0a/0b/0c/0d，64 用例 × O0/O2 = 128 snapshot）。**
> D1 落地 baseline；D2-D3 warn-only 集成；D4 切 `-Strict` 红线；D5 micro-asserts 全量化 + 阈值分档；**D6 industrial 全集 + 总览总表**。
>
> 当前基线规模：64 用例 × O0/O2 = 128 snapshot。最新运行结果以命令输出
> 和生成的 `last_report.md` 为准；该文件每次 check 会被覆盖，属于忽略的
> 本地产物，不再提交。历史报告不能作为当前 SDK 编译器的验收结论；读取报告时
> 必须核对原运行的编译器身份。

---

## 1. 目录布局

```
tests/snapshots/ir/
├── manifest.txt                 选样清单（路径或 stable-id | 路径）
├── meta.json                    上次生成 / bless 时的元信息
├── REPORT.tpl.md                D2: last_report.md 的列含义说明（首次自动生成）
├── last_report.md               D2: 最近一次 check 的可读报告（生成/覆盖，不提交）
├── micropoints/                 D2: 微点测 DSL（每个 case 一个 .txt，可选）
│   └── <case-id>.txt
├── O0/                          -O0 档基准（无优化）
│   ├── <case-id>.ll.norm        规范化后的完整 IR 文本
│   ├── <case-id>.sha256         norm 文件的 SHA256
│   └── <case-id>.counts.json    D2: 关键 opcode 计数
└── O2/                          -O2 档基准（主流优化）
    ├── <case-id>.ll.norm
    ├── <case-id>.sha256
    └── <case-id>.counts.json
```

`<case-id>` 默认为相对仓库根的路径派生：分隔符 `\` `/` → `__`，去掉 `.vyx` 后缀。

`manifest.txt` 也支持 `stable-id | source-path.vyx`；suite 重构时保留既有
`stable-id`，可避免重写已评审的 golden 文件名。例：

- `tests/cases/_rc_smoke.vyx` → `tests__cases___rc_smoke`
- `tests___strict__strict_01_pack_boundary | tests/cases/conformance/strict/strict_01_pack_boundary.vyx`
  保留 `tests___strict__strict_01_pack_boundary` 作为稳定 ID。

---

## 2. 当前选样（D6 全集：64 用例 × 2 档 = 128 snapshot）

| 来源 | 数量 | 覆盖路径 |
|---|---|---|
| `cases/` (基础语法 + RC + closure + clone) | 5 | RC / 闭包 / clone / P4 auto-drop（早 return / reassign） |
| `cases/conformance/strict/` | 1 | pack/typeof 边界（auth） |
| `cases/conformance/adversarial/round1/` | 1 | reflect/pack 大量元素（auth） |
| `cases/conformance/generics/industrial/` (industrial **全集 codegenable 子集**) | 57 | smart-ptr / trait-object / 嵌套容器 / 多参数泛型 / variadic pack / generic closure / hkt-like / HashMap / ... |
| **合计** | **64** | × O0/O2 = **128 snapshot** |

> 排除 industrial 内 2 个 EF（expected-fail）诊断用例 (`chain_diag_3level`, `hkt_functor`)，由主门 cases/auth-adv 的诊断断言覆盖；详见 `manifest.txt` 注释。

---

## 3. 工作流（PowerShell）

> 所有命令在仓库根执行。
> 这套旧基线的默认路径优先级仍是：`-Compiler` 参数 → `$env:VYX_HOST_VYXC` →
> `cmake-build-debug\vyxc.exe` → `build\vyxc.exe`。当前分支的迁移工作必须显式传
> `-Compiler <以 Release SDK 为 Stage 0 刚构建的 bootstrap_compiler/out/vyxc.exe>`；不要让
> 脚本回退到 host 路径。

### 3.1 校验（最常用）

```powershell
powershell -ExecutionPolicy Bypass -File scripts/ir_diff.ps1
```

退出码：
| 码 | 含义 |
|---|---|
| 0 | 全部 match，或 mismatch 但当前是 warn 模式（D2-D3 默认） |
| 1 | mismatch 且 `-Strict`（D4+） |
| 3 | 至少一个 case 缺基线（需先 `-Mode generate`） |
| 4 | 至少一个 case 编译失败 |

### 3.2 首次生成基线（仅当文件缺失时落盘）

```powershell
powershell -ExecutionPolicy Bypass -File scripts/ir_diff.ps1 -Mode generate
```

已存在的 baseline **不会被覆盖**，会输出 `SKIP`。

### 3.3 刷新基线（评审通过后用，会覆盖）

交互式（推荐本地）：
```powershell
powershell -ExecutionPolicy Bypass -File scripts/ir_diff.ps1 -Mode bless
# 提示输入 BLESS（区分大小写）才会覆盖
```

非交互（评审 commit / CI）：
```powershell
powershell -ExecutionPolicy Bypass -File scripts/ir_diff.ps1 -Mode bless -ConfirmBless
```

> **CI 默认拒绝 `-ConfirmBless`**（D4 落地）：basline 刷新必须由人工评审 commit 提交，以保证"双锁"约束（行为变更 + IR 基线变更要分两步评审）。

### 3.4 指定优化等级 / 编译器

```powershell
# 只跑 O0
powershell -ExecutionPolicy Bypass -File scripts/ir_diff.ps1 -Levels O0

# 为迁移实验显式指定从当前源码构建的 SDK 编译器及同次构建的后端与运行时
powershell -ExecutionPolicy Bypass -File scripts/ir_diff.ps1 `
  -Compiler .\bootstrap_compiler\out\vyxc.exe
```

---

## 4. Normalize 规则（D1 保守版）

由 `scripts/ir_diff.ps1::Normalize-IR` 实现。当前刨除以下抖动源：

1. 头部 4 行：
   - `; ModuleID = '...'`
   - `source_filename = "..."`
   - `target datalayout = "..."`
   - `target triple = "..."`
2. 行内仓库根绝对路径 → `<REPO>` 占位符（应对路径改名/CI 路径不同）。
3. 统一行尾 `\r\n` → `\n`，去重收尾空行。

**未处理（已知尚有抖动可能）**：

- LLVM metadata 编号 `!N`：D1 暂不动，等 D2-D3 warn 期统计噪声后再决定。
- 临时变量编号 `%数字`：同上。
- `define` 内 attribute `#N` 编号：同上。

> 实测 D1 选样下，连续两次重跑全 18 项 SHA256 一致，证明保守 normalize 已足够稳定。后续如增样发现新抖动源，需在 PR 中同步扩展 `Normalize-IR` 与刷新基线。

---

## 5. 添加 / 移除用例

1. 编辑 `manifest.txt`：增删用例行；空行与 `#` 开头注释忽略。
2. 跑 `-Mode generate`：仅为新增项落盘，**不会动旧基线**。
3. 移除项的旧 `.ll.norm` / `.sha256` 文件需手动删除（`-Mode generate` 不会清理）。

> 后续 D6 阶段会引入"manifest 同步检查"自动报告 orphan baseline 文件。

---

## 6. Guard Components

| 组件 | 状态 |
|---|---|
| snapshot 位置 | `tests/snapshots/ir/{O0,O2}` |
| diff 脚本 | `scripts/ir_diff.ps1`（normalize + sha256 + counts + micropoints + bless） |
| strict wrapper | `scripts/run_gates.ps1` |
| 跨优化级 verifier | `src/CodeGen/CodeGenBackend.cpp` 在 O0/O1/O2/Os/Oz 路径 verify |
| micropoints DSL | 17 例：9 base + 8 industrial；DSL 表见 §8.2 |
| instr count 红线 | 粗 ±10% / 细 ±3% 分档；per-opcode `_tol` 覆盖 |

---

## 7. D4 接入说明（strict）

`scripts/run_gates.ps1` 是 D4 阶段入口，串联：

| stage | 名称 | 当前实现 | fail 策略 |
|---|---|---|---|
| 0a | `ir_guard_strict` | `ir_diff.ps1 -Mode check -Strict` | 任一异常即 exit=1 |
| 0b | `ir_snapshot_diff` | sha256 hash diff | 汇总到 0a |
| 0c | `ir_assert_micropoints` | DSL 断言（仅 O0） | 汇总到 0a |
| 0d | `instr_count_redline` | opcode ±5% | 汇总到 0a |

跑法：
```powershell
# 默认：仅 IR guard（strict）
powershell -ExecutionPolicy Bypass -File scripts/run_gates.ps1

# 同时跑主门测（主门目前仅信息展示）
powershell -ExecutionPolicy Bypass -File scripts/run_gates.ps1 -RunMainGates

# 只跑主门测
powershell -ExecutionPolicy Bypass -File scripts/run_gates.ps1 -OnlyMainGates
```

> **主门 deferred 原因**：仓库目前没有 EF (expected-failure) -aware 的统一 gate driver。
> `tests/run_all_modules.ps1` 会把 EF 测试误报为 fail。当仓库提供标准 gate driver 时，
> 把 `Run-MainGate` 内的 `$runner` 替换即可。

### D4 阈值

- `instr count` 容忍：默认 ±5%（`-CountTolerance` 调整）
- `micropoints` 当前 2 个示例（D5 扩到全部 codegen 关键路径）
- 任何 mismatch / count_warn / micro_warn 都会写到 `last_report.md`，并在 strict 下 exit=1

### O0 verifier 现状（D4）

- `CodeGen::optimize` 在进入优化前统一 verify；`O0` 路径不再静默 return，改为二次 verify 后返回。
- `CodeGen::emitIR` 写盘前增加 verify；失败会同时写 `llvm::errs()` + 诊断系统，禁止静默通过。
- 主门 driver 仍待 EF-aware 版本接入（当前 `run_all_modules.ps1` 会把 EF 计作 fail）。

---

## 8. D5 落地说明（micropoints 全集 + 阈值分档）

### 8.1 用例 → 关键路径覆盖矩阵

| # | case-id | 覆盖关键 codegen 路径 | 容忍度档 |
|---|---|---|---|
| 1 | `tests__cases__closure_int_test`               | (a) closure capture by-value，fat-ptr + heap env malloc           | 细 ±3% |
| 2 | `tests__cases___rc_smoke`                       | (b) `Ref<T>::drop` / `Weak<T>::upgrade` 双计数路径                | 细 ±3% |
| 3 | `tests__cases___p4_early_return_drop`           | (c) early-return 自动 drop（每个分支都必须 emit drop）            | 细 ±3% |
| 4 | `tests__cases___p4_reassign_drop`               | (d) reassignment 自动 drop（旧值 free + 末尾 free）               | 细 ±3% |
| 5 | `tests__cases___clone_semantics`                | clone 链路 + **(unwind future-proof 哨兵)**                        | 粗 ±10%（call/br ±5%）|
| 6 | `tests___strict__strict_01_pack_boundary`       | (g) string 常量 + pack/typeof 单态化（mangled name 形态）         | 细 ±3% |
| 7 | `tests___adversarial__adv_05_pack_of_10`        | (e) generic 单态化 10 元 fixed-arity                              | 细 ±3% |
| 8 | `tests___generics_industrial__box_dyn_trait`    | (h) trait dyn dispatch（vtable 双槽 + 间接 call）                  | 粗 ±10%（call/br ±5%）|
| 9 | `tests___generics_industrial__smart_ptr_clone`  | (e) `Vec<T>::push` 三档单态化 + (f) panic 调用点（unwind 哨兵）   | 粗 ±10%（call/br ±5%）|

> 9 个用例 × O0/O2 = **18 snapshot**；其中 micropoints 仅在 O0 跑（O2 inlining/DCE 后断言不稳定，详见 §3）。
> ▶ 用例 5 与 9 内置「unwind future-proof 哨兵」，详见 §8.4。

### 8.2 micropoints DSL 语法表

每行一条断言，`#` 起头为注释，空行忽略。**仅在 O0 baseline 上执行**。

| 语法                                | 语义                                                                |
|---|---|
| `must_have      <regex>`            | IR 文本必须匹配该正则至少一次；不命中 → `0c FAIL`                    |
| `must_not_have  <regex>`            | IR 文本必须**不**匹配该正则；命中即 → `0c FAIL`                      |
| `count          <regex> >= <N>`     | 命中行数 ≥ N，否则 → `0c FAIL`                                       |
| `count          <regex> <= <N>`     | 命中行数 ≤ N，否则 → `0c FAIL`（用于挡"指令暴增"）                   |
| `count          <regex> == <N>`     | 命中行数严格相等                                                     |

> regex 引擎：.NET `System.Text.RegularExpressions`（与 PowerShell `[regex]::Matches` 同源）。
> 多行模式默认开启，`^` / `$` 匹配每行起止；`.` 默认不跨行。

**实践要点**：
- 以 `must_have define void @"<symbol>"\(` 锁住"函数被 emit"——比锁完整签名鲁棒。
- `must_not_have @_Znwm` / `@__cxa_` / `@_ZdlPv` 反向锁"未链入 C++ 异常运行时"。
- `must_not_have ^\s*invoke\s+` / `landingpad` / `cleanuppad` 是 **unwind future-proof 哨兵**：当 codegen 重构引入 SEH/Itanium EH 形态后会立刻 fail，强制评审。
- `count` 上限挡 codegen 漂移（例如 `call void @free <= 30` 防 drop 调用点重复 emit）。

### 8.3 counts.json 阈值字段（D5 新增）

`counts.json` 在原有 6 类 opcode 计数（`call`/`load`/`store`/`alloca`/`br`/`ret`）外，新增两个**可选**字段：

```jsonc
{
  "_tol_default": 0.10,                         // 该用例的默认 ±tol，覆盖 -CountTolerance 全局值
  "_tol":        { "call": 0.05, "br": 0.05 },  // 单 opcode 阈值，覆盖 _tol_default
  "call": 956, "alloca": 1545, "ret": 561,
  "load": 4313, "store": 2345, "br": 937
}
```

**解析优先级**：`_tol[op]` > `_tol_default` > 命令行 `-CountTolerance`（默认 0.05）。

**生效写入路径**：`bless` / `generate` 在写出 `counts.json` 时会**保留**已有的 `_tol_default` / `_tol` 字段，避免刷新基线时把人工调整的阈值清掉。

### 8.4 阈值分档选择（D5 设计说明）

依据 PLAN §3.5 的 ±5% 粗骨架，D5 把阈值按用例**敏感度**分两档：

| 档位 | `_tol_default` | per-op 收紧             | 适用范围                                                                        |
|---|---|---|---|
| **细粒度** | **0.03** (±3%) | —                       | `cases/`、`_p4_*`、`_strict/`、`_adversarial/`：用例小、每条 instr 都是 codegen 直接产物；漏 1 条 drop 立刻被察觉 |
| **粗粒度** | **0.10** (±10%) | `call`: 0.05<br/>`br`: 0.05 | `_clone_semantics`、`_generics_industrial/*`：拖了大量 std lib（Vec/Dict/PriorityQueue/StringBuilder），LLVM 内部抖动空间大；但 **`call`** 是 drop / 静态分派核心、**`br`** 是控制流核心，劣化 5% 即立刻红线 |

**为什么不是统一 ±5%？**
- 工业级用例的 `load` / `alloca` 在 codegen 调整闭包槽布局或结构体字段顺序时容易抖动 ±7%-±9%（实测于近期 closure-env 重排），统一 ±5% 会高频假阳；
- 但工业级用例的 `call` 计数极敏感（`call drop` 缺一次就是真 RC 漏）—— 收紧到 ±5% 给"寂静退化"留更小逃逸窗口；
- 小用例反向：单条 `call` 占比大、抖动很快越过 ±5%，所以收紧到 ±3% 让 codegen 形态变化"必须显式 bless"。

> D6 industrial 全集（59 用例）落地时，会把每个 codegen 重构密集子集划入"粗"，把 RC/closure/auto-drop 关键路径划入"细"——本节是分档基线模板。

### 8.5 0c / 0d 故障排查

| 现象 | 来源 stage | 处置 |
|---|---|---|
| `WARN <lvl> <case> micro must_have FAIL: /<rx>/` | 0c | 看 `tests/snapshots/ir/<O0>/<case>.ll.norm` 该 pattern 是否真的丢；若 codegen 已迁移到等价形态，更新 micropoint 并提交，**不要直接 bless** |
| `WARN <lvl> <case> count <op> N→M (P% > T%)` | 0d | 比 baseline 多/少了 (M-N) 条 `<op>`；定位最近 codegen 改动；若是预期变化 → `-Mode bless` 刷新（保留 `_tol_*`） |
| 仅 sha 不一致但 counts/micro 全 ok | 0b | 通常是 normalize 漏处理某种新抖动；扩 `Normalize-IR` 后再 bless |

---

## 9. D6 落地说明（industrial 全集 + guard 总表）

### 9.1 D6 状态表（与 PLAN §4 集成对照）

| Stage | 名称                       | D5 前 | **D6 后**                                           |
|-------|----------------------------|-------|-----------------------------------------------------|
| 0a    | ir_guard_strict           | 9 例 × 2 = 18 snapshot | **64 例 × 2 = 128 snapshot**（含全部 57 industrial codegenable） |
| 0b    | ir_snapshot_diff          | 同上 | 同上 |
| 0c    | ir_assert_micropoints     | 9 用例 / O0 only | **17 用例 / O0 only**（9 base + 8 industrial 代表）  |
| 0d    | instr_count_redline       | 18 counts.json | **128 counts.json**（per-opcode `_tol` 字段全覆盖） |

### 9.2 guard 护住范围总表（D6 全景）

| 维度 | 数量 | 备注 |
|---|---:|---|
| **snapshots — `.ll.norm` 文件** | **128** | 64 用例 × O0/O2；总体积 **42.1 MB**（base ≈ 1.7 MB + industrial 40.5 MB），全部 < 50 MB 上限 |
| **NO_BODY 用例数** | **0** | 单文件最大 1.1 MB（second_order O0），未触发 >2 MB 阈值；NO_BODY 机制保留为未来扩展点（详见 manifest 注释） |
| **counts thresholds — counts.json 文件** | **128** | 全部携带 `_tol_default` 字段；其中 industrial 全集 (57×2=114) + `_clone_semantics` (×2) 共 **116** 文件追加 per-opcode `_tol`（call/br=0.05，粗 0.10 默认）；其余 12 文件（cases/_p4_*/_strict/_adv）使用细 `_tol_default=0.03`，无 per-opcode override |
| **micropoints — `.txt` 文件** | **17** | base 9 + industrial 8（multi_param_generic / variadic_print / nested_dict_vec / trait_obj_storage / nested_box_generic / lambda_capture_generic / second_order / hashmap_generic） |
|   ↳ 含 unwind future-proof 哨兵的 micropoints | **10** | base 2（_clone_semantics, smart_ptr_clone）+ industrial 8（D6 选样全部内置哨兵） |
| **EF 排除用例** | **2** | `chain_diag_3level.vyx`, `hkt_functor.vyx` —— 见 §2 注释 |
| **industrial codegenable 全集覆盖率** | **57/57 = 100%** | `meta.json::industrial_full = true` |
| **industrial micropoint 抽样覆盖率** | **8/57 ≈ 14%** | 选样依据：覆盖多参数泛型 / pack-variadic / Vec/Dict 组合 / trait dyn / smart_ptr / generic closure / 二阶嵌套 / HashMap |

### 9.3 D6 实测耗时

| 操作 | 耗时 |
|---|---|
| `scripts/ir_diff.ps1 -Mode generate`（一次性产出 110 个新 industrial snapshot + counts） | ~76 s |
| `scripts/run_gates.ps1`（一轮 64 用例 × 2 档 strict check） | ~77-104 s（各档 vyxc 重编译为主要开销，单 vyxc -O0 ~250-700 ms） |

> 实测 vyxc 在 O0/O2 下编译 industrial 中等规模用例 ~0.5 s/次；并发改进可压到 30 s 以内（D7+ 可优化点）。

### 9.4 验证通过证据（D6 落地时）

- 一轮 `run_gates.ps1` 全干净：`match=128 mismatch=0 count_warn=0 micro_warn=0`，0a/0b/0c/0d 均 PASS。
- 抽 3 个 industrial 用例混合伪造：
  - `trait_obj_dispatch.sha256` 篡改为 `deadbeef...` → 0b WARN(1) sha-mismatch；
  - `nested_vec_vec.counts.json` 把 `call:991` 改成 `500` → 0d WARN(1) `call 500→991 (98.2% > 5%)`，**per-op `_tol.call=0.05` 解析生效**；
  - `variadic_print.txt` 把 must_have 改成 `INTENTIONALLY_BROKEN_FOR_VERIFY` → 0c WARN(1) micro must_have FAIL；
  - 三类同时触发，0a 总闸 FAIL(rc=1)；回滚后 PASS（`match=128 ...`）。

---

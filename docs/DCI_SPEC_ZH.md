# DCI（声明式外部代码接口）核心规范

[English: DCI interoperability reference](DCI_SPEC.md) · [文档目录](README.md)

- **规范名称**：DCI（声明式外部代码接口）
- **规范版本**：DCI Core 1.0
- **状态**：初始规范性基线
- **首个实现与标准源**：Vyx
- **规范基线日期**：2026-09-02
- **实现核对日期**：2026-10-01

DCI 在 事实语义所有权系统框架下，把跨语言操作所需的程序事实组织成可验证契约。
元信息包含事实、实体身份、有效范围和来源；有效范围可以由目标、ABI、工具链、配置和版本限定。

对于实体 `e`、操作 `o`、目标 `T`，消费端必须具备所需的全部已验证事实：

```text
R(e, o, T) ⊆ K_C(e)
```

某项操作能否接入，取决于事实是否充分、实现产物是否匹配，以及 Consumer 是否支持对应契约。
布局相同不能替代调用与生命周期要求。
用法见 [事实语义所有权系统 核心特性](MOSP_ZH.md)，命令与配置见 [DCI 工具](../tools/dci/README.zh-CN.md)。

本文定义语言无关的 DCI 核心协议。Vyx 是第一个实现，也是初始规范的发布者；Vyx 当前使用的 C++ 语法、Clang 扫描器、MSVC ABI 元数据和生成式 C++ stub 属于 **Vyx C++ Profile**，不是 DCI Core 对其他语言实现的强制要求。

DCI Core 和 Consumer 的规范化输入中不得存在源语言特化行为。语言、编译器和对象模型差异只能存在于 Adapter、带版本的 Profile，以及可插拔的 ABI-native Stub backend 中。`source.language` 是来源身份和诊断信息，**MUST NOT** 成为 Consumer 选择布局、调用、位域、继承或生命周期算法的条件。

文中的 **MUST（必须）**、**MUST NOT（禁止）**、**SHOULD（应当）**、**SHOULD NOT（不应）** 和 **MAY（可以）** 是规范性关键词。

---

## 1. 定义

DCI 是编译型语言之间的对等互操作协议。它使用一份经过验证、目标相关且足够具体的描述文件，声明外部程序实体的 ABI 契约，使宿主编译器可以直接生成调用代码，或者生成并编译必要的桥接代码。

本文使用以下端点术语：

- **生产端（Producer Endpoint）**：产出将被调用的原生代码与制品的一方；
- **宿主端（Host Endpoint）**：导入契约并生成调用代码的一方；
- **DCI 描述（DCI Descriptor）**：机器可读、目标相关的 ABI 契约；
- **ABI 原生 Stub（ABI-native Stub）**：由生产端语言或兼容生产端 ABI 的工具链编译的桥接代码。

DCI 不是以 C ABI 为唯一公共分母的 FFI。C ABI 是 DCI 的最低兼容层，但 DCI 还可以描述：

- 非 C 调用约定与修饰符号；
- 按值聚合类型及其精确布局；
- 构造、复制、移动、销毁和分配等生命周期能力；
- 类、方法、继承和基类偏移；
- 虚表、动态派发、`this` 调整和反向 override；
- 已由外部泛型系统完成实例化的具体实体。

在一条互操作边上，“宿主”和“外部语言”只是角色，而不是永久的主从关系。同一对语言可以在另一条边上交换角色。

### Vyx 绑定入口

- `extern "C"` 用于 C ABI 声明；`extern "dci"` 配合 `@[dci_import]` 导入更完整的原生契约。
- C 回调使用 `cfn(...) -> R`；普通 Vyx `fn` 值不能直接作为 C 函数指针传入。
- 当前 cJSON 模型中的整数精确范围为 `±2^53`；编码器拒绝超出范围的整数事实。
- 开放泛型通过 Active Adapter 请求生产端物化；canonical 契约仍只承载闭合事实。

---

## 2. 核心规则

### 2.1 外部语法对宿主不透明

宿主 Consumer **MUST NOT** 依赖解析外部源语言语法来完成调用。它只理解 canonical `.dcib` 契约中声明的 ABI 事实、类型能力和生命周期操作。

宿主侧可以具有自己的绑定声明语法；这类声明是 Converter 的输出或人工核对结果，不等于宿主理解了外部源语言。

### 2.2 生命周期是显式能力

外部类型支持哪些生命周期操作，必须由契约明确声明。Consumer **MUST NOT** 根据类型名称或源语言习惯猜测以下能力：

- 默认构造、带参构造；
- 复制构造、复制赋值；
- 移动构造、移动赋值；
- 析构；
- 分配、释放及其必须配对的 allocator；
- retain/release 或其他共享所有权操作；
- 合法的借用、别名和可变访问模式。

未声明的能力视为不可用。Consumer **MUST NOT** 合成一个外部契约没有承诺的生命周期操作。

复制、移动和所有权是三个相互关联但不能互相替代的维度：

- **复制能力**说明是否允许产生两个独立可销毁值，以及采用 trivial copy、显式 copy operation、retain 还是共享所有权；
- **移动能力**说明所有权是否转移、采用 trivial move、显式 move operation 还是 relocate，以及源值移动后是仍可使用、仅可析构、已清零还是状态未指定；
- **所有权方向**说明每个参数和返回值是借用、可变借用、复制、移动、共享、out 参数还是新拥有值。

Consumer **MUST** 在调用点同时验证类型级 lifecycle 和参数/返回值级 ownership。参数标记为 `move` 不代表可以按字节复制；只有类型契约声明相应 move/relocate 能力时才合法。参数标记为 `copy` 也不代表 `memcpy` 合法；non-trivial copy 必须调用契约指定的 operation。

每个创建、复制、移动、retain 或分配操作都必须能追溯到兼容的销毁、release 或释放域。契约必须标明 allocator domain；跨 allocator、跨运行时或跨模块释放只有在契约显式允许时才合法。借用值不得被 Consumer 隐式接管或销毁，owned 值不得在所有权转移后再次销毁。

### 2.3 非本地控制流默认不得跨界

外部异常、panic、unwind、`longjmp` 或其他非本地语言控制流 **MUST NOT** 穿越 DCI 边界，除非双方显式声明并实现同一种传播 ABI。

默认契约是 `no_unwind`。Adapter 必须准确记录传播能力；如果双方没有共同传播 ABI，生产端实现或 Stub 必须在边界内完成捕获、终止或显式值转换，Consumer 不得默许未知 unwind 穿越。

### 2.4 泛型闭合与生产端语义

canonical `.dcib` **MUST** 只承载具体实体的闭合事实。
Core Consumer **MUST NOT** 通过解释外部源语言完成泛型约束、重载选择、特化选择或单态化。

Adapter **MAY** 在宿主构建期间请求生产端执行这些工作。
生产端完成语义闭合后，Adapter 才能交付操作所需的具体布局、符号、调用契约和实现产物。
离线提取、生产端原生输出和 Active / Online 请求是同一 Adapter 角色的不同事实生产模式。

宿主可以完成自己的类型推导，并将具体实参提交给 Adapter；
外部概念、trait、重载和实例化是否成立，仍由生产端裁决。
请求中出现开放实体，不意味着 canonical ABI 契约可以包含未解决的外部语义义务。

### 2.5 忠实执行不等于安全证明

DCI 不保证外部代码安全，也不证明外部实现满足其源语言规则。在描述文件与实际链接制品一致且契约通过校验的前提下，DCI Consumer 保证的是：

1. 在使用前验证描述文件、链接制品身份与当前目标兼容；
2. 拒绝缺失、冲突或无法表示的 ABI 契约；
3. 忠实执行通过验证的外部 ABI 契约。

外部代码中的越界、悬垂指针、数据竞争、错误析构或契约欺骗仍可能造成未定义行为。

### 2.6 精确描述差异，而非抹平差异

DCI 不要求参与语言共享对象模型、异常模型、所有权模型或泛型模型。不同之处必须被显式描述、转换或拒绝，不得通过隐式猜测隐藏。

### 2.7 面向编译型语言

DCI Core 适用于满足以下条件的端点：能够产出目标 ABI 明确的原生代码，并在声明的有效域内提供确定的符号、布局、调用和生命周期事实，并且能够生成或由 Adapter 提取足够精炼、有效、可验证的 DCI 描述。通常这意味着双方都是编译型语言。

解释型语言、VM 动态对象、运行时 shape、动态消息派发、GC root 协作和解释器异常传播不属于 DCI Core。此类互操作应由独立的 **DCI-Interp** 协议定义。

JIT 端点只有在调用前已经产生稳定、可验证的原生 ABI 实体时，才可以把该实体作为 DCI Core 的生产端或宿主端；其他 JIT/VM 互操作仍属于 DCI-Interp。DCI-Interp 是独立协议，不是 DCI L4。

### 2.8 被消费语言无需主动支持 DCI

被消费语言或其运行时不需要为 DCI 修改。只要外部工具链暴露了足够的 ABI 事实，外置 Adapter 就可以生成 canonical `.dcib` 契约制品。

原生支持 DCI 的语言仍然具有 Adapter 这一逻辑角色，但可以由编译器直接输出 `.dcib`，从而省去外置 Adapter。

如果外部工具链没有提供足够信息，Adapter **MUST NOT** 猜测；它只能降低支持层级或拒绝导出。

---

## 3. 参与者与产物

| 名称 | 职责 | 禁止承担的职责 |
|---|---|---|
| **Adapter** | 从生产端获取事实，或在构建期请求生产端生成事实与实现产物，再归一化为契约 | 不替 Consumer 猜测 ABI，不自行重实现生产端语义 |
| **Converter** | 将 `.dcib` 契约映射为宿主语言的绑定声明或内部绑定模型；可以是工具、编译器内建阶段，也可以人工编写后核对 | 不改变 ABI 事实，不负责生产端代码生成 |
| **Consumer** | 验证 `.dcib`，选择 Direct 或 Stub，并生成最终调用代码 | 不解析外部源语言，不猜测缺失契约 |
| **`.dcib` 契约制品** | canonical、目标相关、可验证的二进制 ABI 契约 | 不携带待宿主求解的外部语言语义问题 |
| **`.dci` / `.abi.json`** | 人类可读的 JSON 诊断、审查和 validator 输入；可在迁移 CLI 中读取并转换为 `.dcib` | 不作为 Vyx 项目构建或 `dci_import` 的 Consumer 输入 |
| **Profile** | 定义某种语言、对象模型或目标 ABI 对 DCI Core 的扩展 | 不得破坏 Core 的边界规则 |
| **Stub backend** | 把规范化 Stub 请求转换为某一生产端工具链可编译的桥接制品 | 不得把源语言语义泄漏回 Core Consumer |

Adapter、Converter 和 Consumer 是三个必需的逻辑角色，可以由三个工具分别实现，也可以合并在同一个编译器中。若契约已经能被宿主内部模型直接表示，Converter 可以是恒等转换，但其绑定映射与校验职责仍然存在。Stub backend 仅在选用 Stub 模式时需要；一个 Consumer 可以注册多个 backend，但 Core 不得内建“遇到某种 source language 就生成某种源码”的分支。

---

## 4. 标准流水线

```text
外部源代码 / 编译器 / 目标文件
              |
              v
          Adapter
              |
              v
       canonical 目标相关 .dcib
              |
       +------+------+
       |             |
       v             v
   Converter      Consumer 验证
       |             |
       v             +----------------+
宿主绑定声明          |                |
       |             v                v
       +--------> Direct          生成 Stub
                     |                |
                     |          外部工具链编译
                     |                |
                     +--------> Direct 暴露
                                      |
                                      v
                                  最终链接
```

规范性要求：

1. Adapter **MUST** 先把外部语言事实归一化为 `.dcib`，Consumer 不得绕过该契约读取外部语法。
2. Converter 是必需的逻辑阶段，但可以内建、恒等或以人工绑定加自动核验的形式完成。宿主声明或内部绑定模型 **MUST** 与 `.dcib` 一致。
3. Consumer **MUST** 在代码生成前验证目标、布局、符号、调用约定和所需能力。
4. Direct 与 Stub 可以按类型、函数或单次操作混合使用，不要求整个模块只能选择一种模式。

---

Active Adapter 在此流水线上增加“宿主请求 → 生产端语义工作 → 闭合结果”的反馈路径。
该路径可以请求生成先前未导出的实例；返回的事实和产物仍须满足相同的验证要求。
“Online”表示构建期间可调用，不要求联网。

## 5. `.dcib` 契约模型

本文规定 DCI 契约必须表达的逻辑语义。Vyx 当前 canonical 产物和 Consumer 输入是 `.dcib` 二进制容器；`dci_import`、项目 manifest 的 `dci_file`/`dci_files` 及项目构建只接受 `.dcib`。二进制编码不得改变同一 Core 契约的语义。

`tools/dci/schema/dci-1.0.schema.json` 定义 DCI 的 JSON 逻辑模型，供 Adapter、审查工具和 validator 使用。`.dci` 与 `.abi.json` 是该人类可读 JSON 表示的文件名别名；它们不是 Vyx 项目构建的消费容器。CLI 可为迁移和诊断兼容读取 JSON，再显式编码为 `.dcib`；这种兼容读取不得让 Consumer 在项目构建中回退到文本解析。

一份可消费的 `.dcib` **MUST** 足够具体，使 Consumer 无需再次运行外部语言的语义分析。至少应覆盖以下类别。

### 5.1 文件与目标身份

- DCI Core 版本和 Schema 版本；
- Profile 名称及版本；
- target triple、体系结构、指针宽度、端序；
- ABI family、编译器 ABI flavor 及必要版本；
- Adapter 身份和生成版本；
- 影响 ABI 的语言模式、编译选项、宏、packing 与标准库 ABI 标识；
- 来源文件、模块或制品身份；
- 用于缓存失效、链接制品身份验证和契约核验的 build ID、摘要或等价标识。

`.dcib` 是目标相关制品。为 MSVC x64 生成的布局 **MUST NOT** 被当作 Itanium ABI、x86 或其他目标的布局复用。

### 5.2 类型与布局

- 唯一类型身份；
- 类型种类；
- size、alignment、packing；
- 字段类型、offset、可见性和必要的位域信息；
- 按值、指针、引用、可空性和限定信息；
- 基类关系、基类 offset、虚继承信息；
- POD、trivial、non-trivial 等与 ABI 有关的性质。

`representation = stable` 或 `transparent` 只说明表示形式，不证明复制、移动或析构是 trivial。此类 value layout 必须提供一个显式 `lifecycle` 契约，或者同时明确声明 `is_pod = true` 与 `is_trivially_destructible = true`；字段缺失不能解释为 `true`。Validator 必须拒绝两类证据都缺失的 Descriptor。Consumer 在按值使用点还必须继续核对 lifecycle：只有 copy/move 为 trivial 且 destruction 为 trivial/none，或上述两个显式 trivial 标记同时成立且没有反证时，才可直接物化 caller temporary；否则必须执行已支持的 lifecycle materialization 或 fail-closed。

#### 5.2.1 位域

位域不是普通可寻址字段。位域契约必须显式给出：承载存储单元的 byte offset 与大小、bit offset、bit width、`signed`、`bit_order`，以及 `read`/`write` 分别采用 `direct`、`operation` 还是 `unavailable`。任一字段缺失都不是可推导默认值；选择 `operation` 时还必须给出完整的 `read_operation` 或 `write_operation` reference。

Direct 读必须按契约加载存储单元，执行移位与掩码，并在有符号位域上按声明宽度做符号扩展。Direct 写必须执行保留相邻位的 read-modify-write。Consumer **MUST NOT** 对位域生成地址、引用或跨边界裸指针，也不得把 Adapter 的源语言字段类型当作位域布局依据。

跨存储单元、volatile、atomic、并发可见性或目标不支持的 bit order 必须由 Profile 给出完整操作语义；Consumer 无法忠实执行时必须改用声明的 operation/Stub 或拒绝。`bit_order = target` 只有在当前 Profile 给出确定解释时才可执行，不能被静默当成 `lsb0` 或 `msb0`。`read`/`write` 为 `unavailable` 或当前模式不支持的 operation 时，Consumer 必须在使用点拒绝。

#### 5.2.2 普通基类与虚继承

普通非虚基类可以使用 `adjustment.kind = constant` 和固定 byte offset。虚基类或其他运行时相关基类必须使用规范化 adjustment，不能伪装成固定 offset。

`is_virtual = true` 的 base 缺少 `adjustment` 必须拒绝；虚基使用 `constant` 同样必须拒绝。`constant` 必须显式给出整数 `offset`，`operation` 必须给出可执行且可解析的 operation reference，`table` 则必须满足下述 Direct 完整字段，或明确作为仅可由 Stub 消费的 fallback 事实。

Core 定义三类 adjustment：

| kind | 契约 | Consumer 行为 |
|---|---|---|
| `constant` | `offset` | 在当前对象指针上增加固定字节位移 |
| `table` | `table_pointer_offset`、`table_entry_offset`、`table_entry_size`、符号性和 `displacement_base_offset` | 从对象中加载表指针，再加载位移项并计算目标基类地址 |
| `operation` | 一个经过验证的 operation reference | 通过 Direct 可调用 helper 或 Stub 完成调整 |

`table` 是语言无关的小型调整契约。Adapter 负责把 vbptr/vbtable、virtual base offset table 或其他生产端机制归一化为上述字段；Consumer 只执行 load/add/sign-extend 等操作，**MUST NOT** 识别 MSVC、Itanium、C++ 或其他生产端专名。若 Adapter 无法生成完整且目标相关的 adjustment，只能选择 `operation`/Stub 或拒绝导出。

多级继承的每一步都必须独立验证。菱形继承、虚基共享、空基类优化和 primary base 选择不得靠递归累加静态 offset 猜测。

### 5.3 符号与调用

- 逻辑名称和最终链接符号；
- linkage、visibility 和 symbol version；
- calling convention；
- 参数与返回类型；
- hidden parameters、`sret`、`this` 和寄存器/栈位置等必要信息；
- variadic、const、static、virtual 等影响 ABI 的标记；
- 必要的 receiver 或返回值调整。

Adapter 必须导出已经选定的最终符号。Consumer 不负责重新进行外部 overload resolution 或 name mangling 推导。

同一 binding identity 可以重复携带完全等价的声明，但不得对应不同的 symbol ABI 契约。binding identity 由规范化实体种类、owner、member/name、static/const 限定和 source-level 参数/返回类型等绑定事实组成；其 `link_name`、calling convention、参数/返回 ABI lowering 或 control-flow 契约发生冲突时，Validator 和 Consumer 必须拒绝，不能依赖数组顺序任取一个。

`link_name`（以及旧 Descriptor 的兼容字段 `mangled`）还是全局机器链接身份。同一身份被多个 symbol 声明时，即使没有任何 lifecycle/runtime operation 引用它们，参数与返回 ownership、规范化类型、calling convention、ABI lowering 和 control-flow 契约也必须等价；否则 Validator 和 Consumer 必须拒绝。比较前应解析 Descriptor 已声明的 type alias，因此两个名称解析到同一底层类型可以等价；解析到不同目标的 alias、仅仅相同的短名或源语言拼写相似都不能视为等价。参数名、deprecated `cpp_type` 等不参与机器契约的来源注释不得制造虚假冲突。

operation reference 可以按规范化 `name`、`semantic_id` 或 `link_name` 解析 symbol；所有被同一 reference 命中的候选必须具有等价契约。即使它们共享同一 semantic/link identity，只要参数或返回 ownership、ABI lowering、calling convention、control-flow/no-unwind 契约或最终链接符号冲突，也必须拒绝该 operation，不能选择第一个候选。

#### 5.3.1 类和聚合按值参数/返回

源级“按值”与机器 ABI 的传递方式是两层契约。参数/返回值首先声明 value/reference 与 ownership，然后由 `abi` lowering 精确声明：

- `direct`：直接按声明类型传递；
- `indirect` / `byval`：调用者物化临时存储并传地址，附带 size、alignment 和属性；
- `sret`：调用者分配返回存储，通过 hidden parameter 传入；
- `coerce`：按指定等宽 ABI 类型传递，再恢复聚合值；
- `split` / `inalloca`：按 Profile 描述的分片或聚合调用帧传递；
- `ignore`：零大小或 ABI 明确忽略的值。

Consumer **MUST NOT** 仅凭类型大小、是否为 class、源语言名称或宿主默认 ABI 推断上述方式。调用端声明、被调端声明、hidden parameter 顺序、calling convention、alignment 和 LLVM/backend attributes 必须来自同一经过验证的 symbol ABI 契约。

`indirect`、`byval` 和 `sret` lowering 必须显式给出正数 byte `size` 与正的 2 次幂 `alignment`；`coerce` 必须显式给出 `coerce_to` 与正数 byte `size`。任何 ABI lowering 中出现的 `alignment` 都必须是正的 2 次幂。缺失条件字段、零大小的间接传递或非 2 次幂 alignment 必须在代码生成前拒绝；零大小值只能使用目标 ABI 明确声明的 `ignore` 或其他 Profile 契约。

`abi.parameters` 必须完整覆盖全部 source parameters：lowering 数量必须一致，每个 source parameter index 必须在合法范围内且恰好出现一次，数组顺序可以与 source order 不同。缺项、重复 index、越界 index 或无法关联到 source parameter 的 lowering 都必须在声明和 call-site lowering 前拒绝。hidden parameters 另行按契约顺序验证，不得用缺失的 source lowering 隐式补位。

所有表示 byte size、alignment、source parameter index、register/stack slot 或 offset 的整数 ABI 字段都必须是真正可表示的整数；JSON 小数、布尔值、溢出值或经浮点转换后发生截断的数不得接受为整数。Consumer 不支持的 ABI shape 也必须显式拒绝：例如声明为 variadic、非空的显式 register assignment、未知 attribute、`split`/`inalloca` 或目标相关 register-class 规则时，不能忽略字段后按普通 Direct 调用生成代码。只有 Profile 与 Consumer 同时实现并验证了该 shape 才能执行。

按值 ABI 可传递不等于可以任意复制。non-trivial 类型在参数物化、返回接收、异常失败和作用域退出时仍必须遵守 5.4 的 copy/move/destroy 契约。描述只给出布局而未给出所需 lifecycle 时，Consumer 必须拒绝该按值操作。

### 5.4 生命周期与所有权

- 可用的构造、复制、移动、赋值和销毁操作；
- 分配与释放操作及其配对约束；
- 借用和所有权转移规则；
- retain/release 或其他引用管理能力；
- 操作是否可能失败或触发非本地控制流；
- 类型是否允许按值跨界。

推荐的规范化模型由三层组成：

1. `layout.lifecycle` 声明 `ownership_model`、`copy_semantics`、`move_semantics`、`destruction`、`moved_from_state` 和 `allocator_domain`；
2. `lifecycle.operations` 将 create/default-construct/copy/move/copy-assign/move-assign/destroy/retain/release 等能力绑定到 symbol、dispatch slot、intrinsic 或 unavailable；
3. 每个 parameter/return 用 `ownership` 声明本次调用的方向，例如 `borrow`、`borrow_mut`、`copy`、`move`、`share`、`out` 或 `owned`。

Portable strict Descriptor 的每个 source parameter 和每个非 `void` return 都必须显式声明 `ownership`；字段缺失不能解释为 `borrow`、`copy` 或任何其他默认方向。旧 `.abi.json` 可以在非 strict 兼容读取中保留缺失字段，但不得据此获得 portable DCI 符合性。

当 `copy_semantics`、`move_semantics` 或 `destruction` 等 lifecycle 字段要求某项 operation 时，相应 operation reference 不仅必须存在，还必须指向 symbol、dispatch table/slot 或 intrinsic 之一，并且引用必须唯一解析。只有 `availability = required` 而没有任何实现的对象不是有效 operation。

契约验证至少必须拒绝：复制被标为 forbidden 的值、移动后继续读取、借用值被销毁、owned 返回值无人接管、allocator domain 不匹配、缺失 non-trivial destroy，以及同一路径上的重复 destroy/release。

`moved_from_state = valid` 只说明源值仍满足类型不变量，不自动允许读取全部业务状态；`destructible_only` 只允许销毁或重新赋值；`unspecified` 不允许 Consumer 假设清零。relocate 只有在 Profile 明确保证目标地址、别名、pinning 和自引用约束时才可使用。

### 5.5 对象模型与动态派发

- 方法所属类型和 receiver 形式；
- vptr 位置、vtable address point 和 slot；
- override/final/pure 等派发性质；
- 多继承下的基类选择与 `this` 调整；
- reverse callback 或宿主 override 所需的 Stub 能力。

DCI 1.0 JSON 编码以 `exports.dispatch_tables` 作为语言无关的动态派发表主字段。`exports.vtables` 仅是早期 Descriptor 的兼容别名；若两者同时出现且非空，内容必须一致，否则验证器必须拒绝。Consumer 应优先读取 `dispatch_tables`，只在主字段为空或缺失时回退到 `vtables`。别名不允许改变 dispatch slot、receiver adjustment 或 symbol ABI 的语义。

#### 5.5.1 运行时类型与 cast

RTTI 是可选能力，不是 L3 的隐式组成部分。契约必须通过语言无关的 `runtime_type_operations` 声明可用能力：`query_type`、`is_instance`、`cast`、`downcast`、`upcast` 和 `type_name`。每项能力必须指向一个 operation，或明确标记 unavailable。

同一 Descriptor 中每个 `runtime_type_operations.type_name` 必须唯一；重复项即使文本相同也会造成 capability 选择歧义，必须拒绝。operation 引用的 symbol 还必须唯一解析到 ABI 兼容的 binding；同一 binding identity 下存在冲突 symbol ABI 时，不能生成或调用 runtime helper。

Direct checked downcast symbol 的规范化签名必须是一个参数的 nullable borrowed pointer 到 nullable borrowed pointer：source parameter 必须精确匹配 `source_type`、`reference = pointer`、`nullable = true`、`ownership = borrow`；返回值必须以相同四项约束精确匹配 `target_type`。参数和返回都必须使用 Direct pointer ABI lowering，并继续满足已声明的 calling convention 与 `no_unwind`。consume/move/owned、non-null、按值、类型不匹配或其他 ABI 形式都必须拒绝。

Adapter 可以读取 C++ RTTI、Rust 自定义 type ID、Objective-C runtime 或其他生产端机制，但这些原始格式只能留在 Profile 扩展。Consumer **MUST NOT** 解析 Complete Object Locator、type_info、Rust metadata 或任何生产语言私有结构；它只能调用或内联 Profile 已归一化且经过验证的 operation。

`dynamic_cast` 是 C++ Converter 对 `cast`/`downcast` 能力的一种源级映射，不是 DCI Core 指令。跨语言 cast 必须声明：源/目标类型身份、成功结果、失败表示、null 行为、必要的 pointer adjustment、所有权变化和 control-flow 契约。缺少任一项时只能使用 Stub 或拒绝，不能退化为静态 pointer cast。

### 5.6 控制流契约

- 每个可调用操作的 `unwind: forbidden` 或共同 propagation ABI 标识；
- 文件或 Profile 可以声明默认 `no_unwind`，但具体操作不得放宽到未声明的传播 ABI；
- 若允许传播，双方共同支持的 propagation ABI 标识；
- Stub 的捕获、转换、终止或返回值策略；
- callback/re-entry 对线程和运行时状态的要求。

未声明传播却发生跨边界 unwind，属于 DCI 契约违例。DCI 只要求 Consumer 忠实执行和验证契约，不保证外部实现在运行时一定遵守该契约。

### 5.6.1 跨模块生命周期与异常事实

参与 shared unwind 的契约可以在根节点 `obligations` 发布版本化事实图：

```json
{
  "version": "1",
  "lifecycle": [{"subject": "eh::Guard", "ownership": "unique",
    "on_unwind": "release", "required_operations":
    [{"name": "destroy", "symbol": "guard_destroy", "when": "unwind"}]}],
  "exceptions": [{"subject": "contract_throw", "boundary": "shared_abi",
    "abi": "dci.eh.msvc-cxx.v1", "cleanup": "unwind"}],
  "edges": [{"from": "exception:contract_throw",
    "to": "lifecycle:eh::Guard", "kind": "cleanup-before-propagate"}]
}
```

`profile.obligation_mode = "required"` 会使事实图成为必需项。DCI 校验器在消费前合并全部 JSON/DCIB 输入，因此异常事实和生命周期事实可以由不同模块分别发布。未解析节点、冲突声明、依赖环，以及没有清理边的 `shared_abi` 异常都会被拒绝。事实图只提供 Consumer 的证据，不推断外部 ABI，也不取代目标相关的运行时展开实现。Vyx 编译器消费显式 `--dci <file.dcib>` 或源码自动发现的 `@[dci_import]` 时执行同一组检查；所有契约在 HIR lowering 前进入当前 事实语义所有权系统 EffectPlan。写入 Effect manifest 时，异常和生命周期主题分别规范化为 `dci:exception:<subject>` 与 `dci:lifecycle:<subject>`，边的 digest 与 kind 分开保存。

---

## 6. Adapter 规范

Adapter 是生产端事实的获取与交付角色，生产端编译器是语义与 ABI 事实的权威。
Adapter 可以离线提取、由生产端编译器直接实现，也可以作为构建期可调用的 Active Adapter。

Adapter **MUST**：

- 以指定目标和 ABI 配置运行；
- 解析或读取外部工具链的权威信息；
- 只导出已经由生产端工具链完成 overload、特化和具体实例选择后的结果；
- 输出确定、规范化、可验证的 `.dcib`，并可附带等价 JSON 诊断视图；
- 对无法确定的事实给出错误，不得填入猜测值；
- 声明自己实际支持的 DCI 层级和 Profile 能力。

Adapter 身份 **MUST** 分别标明源语言、事实提取工具链与目标 ABI，例如 `cpp/clang/x86_64-msvc`。Adapter **MAY** 使用 Clang、GCC、MSVC 或其他编译器获取事实，也可以直接内置于生产端编译器。列出某种 Adapter 不代表该实现已存在；实现状态必须与协议扩展建议分开说明。

---

## 7. Converter 规范

Converter 负责生成宿主语言能表达的绑定表面，例如类型声明、方法声明、模块名和导入属性。

Converter **MUST**：

- 从契约读取事实，而不是独立猜测外部 ABI；
- 为每个绑定保留可追溯的外部实体身份；
- 对宿主无法表达的能力给出诊断或要求 Stub；
- 保持 const、ownership、lifecycle 和 control-flow 限制；
- 不把 concrete generic instance 重新包装成需要宿主参与外部推导的开放泛型。

Converter **MAY** 是自动工具、IDE 功能或人工流程。人工绑定仍必须由 Consumer 与 `.dcib` 交叉验证。

Vyx SDK 的独立工具入口为 `vyx-dci convert`（别名 `converter` / `convertor`）：

```sh
vyx-dci convert contracts/Api.dcib --module native.api --header Api.hpp -o src/native_api.vyx
```

输出保留 `extern "dci"` 的可见类型和方法签名，供查看、维护及派生覆写；已有文件
需要显式 `--force` 才能覆写。构建期生成是可选模式。ownership 与 lifecycle 的提取、
验证和生成属于 Adapter；Converter 不得由指针拼写猜测借用、保留或转移关系。
可自动确定的语义与需要库协议提供的事实均进入同一原始契约。

导出范围由用户声明。Adapter 不得从 Consumer 调用、inline 定义或库名猜测导出范围。
范围内可表达的实体全部输出，不能表达的实体记录诊断；基类/布局闭包提供验证依赖。
可选构建准备要求精确的 `export_types` / `export_functions`，或显式
`export_all = true`。未知 ownership 必须由用户在 `dci-ownership` 中手写一次，
或显式选用可复用的 `ownership_headers`，不得猜测为 borrow。

项目契约是持久输入，通常保存在 `contracts/`。已有离线 Adapter 契约经验证后
直接消费，不重新提取或覆写。删除 `.cache` 只使临时定义、物化源码和构建戳
失效；准备过程 **MUST NOT** 把缺失缓存戳当成契约 ABI 事实无效的证据。
无效或不兼容的输入契约必须给出诊断。

可选的 `source.preparation_inputs` 扩展记录第 1 版生产端头文件哈希、目标、
C++ 标准/参数、异常边界和用户声明的导出范围。构建生成的契约用它独立于缓存
识别已知的生产端输入变化。编译器版本信息不是消费时必须相等的条件。普通离线
契约可以不携带该扩展，库版本由提供者管理。显式 `cpp-import --force` 可以
要求重新测量。所选 Qt 连接操作必须已有实测桥接事实，才能从输入契约重建
物化源码；缺失操作必须诊断。

`parameter.default` 是生产端声明事实，不是 ABI 参数或额外重载。`constant` 保存
与参数类型/取值范围一致的标量/null 值；`producer_expression` 记录尚未支持或需要
生产端求值的默认表达式。可支持的常量默认值成为原始签名上的 Vyx 调用处默认参数；
未解析的默认值记录诊断并要求显式参数。两者都不生成默认参数包装。
原生定义引用原始 API 头文件；可选生产端适配操作单独选择。

---

## 8. Consumer 规范

Consumer 是宿主编译器中验证并执行 DCI 契约的一方。它具有两种正交的消费模式。

### 8.1 Direct 模式

Direct 模式由 Consumer 直接生成目标 ABI 调用：

- 直接引用外部链接符号；
- 按契约放置参数和返回值；
- 直接访问已验证的字段 offset；
- 直接调整基类/receiver 指针；
- 直接加载 vptr 与 vtable slot；
- 直接调用声明的生命周期操作。

Direct 的“零损失”是指 **没有额外 DCI 桥接调用层（zero bridge overhead）**。它不承诺消除原生 ABI 本身的虚调用、指针调整或调用约定成本，也不承诺所有优化器都产生相同机器码。

### 8.2 Stub 模式

当宿主无法直接构造外部对象模型，或者 reverse callback、虚表布局、运行时状态和生命周期需要外部语言参与时，Consumer 使用 Stub 模式。

Stub 模式的标准流程是：

1. Core Consumer 根据契约产生语言无关的 Stub 请求；
2. 与显式 Profile 和 capability 匹配的 backend 把请求转换为 ABI 原生源码、IR 或目标文件；
3. 若输出尚非目标文件，使用生产端语言的权威工具链或明确兼容的工具链编译 Stub；
4. Stub 在外部一侧实现必要的继承、虚表、thunk 或转换；
5. Stub 再暴露一份可由 Direct 模式消费的确定 ABI 表面，宿主通过 Direct 调用该表面。

Stub 通常增加一次或多次桥接跳转，并可能引入转换成本。实现 **MUST** 公开这一事实，不得把 Stub 宣称为 zero bridge overhead。

#### 8.2.1 Stub 请求与可插拔 backend

Core Consumer **MUST** 先产生语言无关的 Stub 请求，其中只包含：所需输入/输出 ABI、对象布局能力、override/callback 集合、生命周期操作、pointer adjustment、运行时类型操作和 control-flow 边界。随后由与目标 Profile 匹配的 Stub backend 把请求转成源码、IR 或目标文件。

backend 的选择依据是显式 Profile/target/capability 匹配，不是对 `source.language` 的硬编码判断。C++ backend 可以生成 C++，Rust backend 可以生成 Rust 或兼容 IR，其他 backend 也可以直接生成目标文件；这些都是实现选择，不改变 Consumer 所见的规范化请求。

#### 8.2.2 Stub 限制

Stub 不是任意语言语义的逃生通道。以下限制必须显式校验：

- backend 必须声明支持的 Profile、target、对象模型、继承形态、calling convention、runtime 和工具链版本；
- Stub 发射只能消费已闭合的符号、布局和操作，不能猜测私有成员或缺失 ABI；需要新实例时，后端可以先通过 Active Adapter 请求生产端完成语义工作，再消费闭合结果；
- reverse override 必须完整描述构造、销毁、每个虚方法签名、receiver/thunk adjustment、callback ownership、线程附着和 re-entry；
- 多继承、虚继承、协变返回、虚析构和 runtime cast 分别是独立 capability，backend 不得因“支持 class”而默认支持；
- variadic、异常/panic 传播、异步/协程、TLS、GC root、pinning 和跨 runtime allocator 只有在 Profile 与 backend 同时声明时才可用；
- backend 生成的桥接边界必须保持原契约的 ownership、lifecycle 和 `no_unwind`，不得通过额外一层调用静默放宽；
- 生成制品必须纳入 target、编译选项、Profile、Descriptor 摘要和工具链版本的缓存键，并接受与 Direct 制品相同的身份验证。

缺少 backend、capability 或权威工具链时，Consumer 必须拒绝该 Stub 请求。不得生成一个“尽量可编译”但 ABI 未验证的桥接层。

### 8.3 模式选择

Direct/Stub 与 DCI 层级相互独立。例如：

- L3 虚调用可以在布局完全已知时使用 Direct；
- 同一 L3 Profile 的 reverse override 可以使用 Stub；
- 一个类型的普通方法可以 Direct，而特定 callback 使用 Stub。

Consumer **MUST** 为每个操作选择契约允许的模式；不能因为同模块其他操作可 Direct，就假定所有操作都可 Direct。

### 8.4 Consumer 的语言无关性检查

一个 Core Consumer 只有满足以下条件，才能声明语言无关：

1. 更改 `source.language` 而保持规范化 ABI 契约不变，不改变其 Direct IR；
2. symbol 解析只使用 `link_name`，不自行推导某种语言的 mangling；
3. aggregate passing、bitfield、base adjustment、dispatch 和 lifecycle 都由 Descriptor 字段驱动；
4. 语言私有元数据只能由 Adapter/Profile 转成 Core operation，不能在 Consumer 内解析；
5. Stub 通过 capability 接口选择 backend，Core lowering 不包含生产语言名称分支。

跨至少两种编译型生产语言的 fixture 是上述声明的必要证据之一，但不能替代逐字段和 IR 结构验证。

---

## 9. DCI 能力层级

DCI 层级是严格累积的。正式声明符合某一层级时，实现 **MUST** 同时满足该层级及其依赖的全部最低能力。只实现其中一部分的实现只能声明为该层级的 **feature subset（能力子集）**，并逐项列出能力；它不得声明完整符合该层级。

Descriptor 中的 `profile.level` 表示其事实所触及的最高能力类别，不单独构成完整符合性声明。`profile.conformance = feature_subset` 表示只携带该层级的一组显式能力；只有 `profile.conformance = full` 才表示严格累积的完整层级声明，并必须满足本节全部要求。字段缺失不应被解释为 `full`。当前 Vyx C++ Adapter 固定输出 `conformance = feature_subset`，即使某份 Descriptor 包含 L3 dispatch 事实也不宣称完整 L3。

| 层级 | 名称 | 最低能力 | 典型实体 |
|---|---|---|---|
| **L0** | FFI 等价层 | C ABI、标量、指针、简单聚合、自由函数 | `extern "C"` 函数、系统调用、传统 C 库 |
| **L1** | 高级 ABI 导出层 | 非 C 调用约定、修饰符号、按值复杂聚合、具体实例符号、高级参数/返回 ABI | 自由 C++ 函数、已解析 overload、具体模板实例 |
| **L2** | Class 层 | 构造/析构、字段布局、方法、继承、基类 offset、RAII | 原生类、non-trivial value、容器封装类 |
| **L3** | 完整虚表多态层 | vptr/vtable、slot、thunk、虚析构、动态派发、`this` 调整、multiple inheritance、reverse override | 多态基类、抽象类、跨语言 override/callback |

L3 的“完整”只表示在所声明 Profile 内，虚表多态所需事实是完整的。RTTI、`dynamic_cast`、异常传播、协程或其他未列出的语言设施不会因 L3 自动获得支持，必须作为独立 Profile 能力声明。

符合性声明 **MUST** 同时给出：

- DCI Core 版本；
- Profile 与目标 ABI；
- Adapter、Converter、Consumer 各自支持的最高层级；
- Direct 和 Stub 的支持方向；
- unwind 策略；
- 已知受限子集。

以下只演示符合性声明的格式，不代表当前 Vyx C++ Profile 已取得该符合性：

```text
DCI Core 1.0
Profile: C++ / MSVC x86_64
Adapter: L3
Consumer: L3
Modes: Direct host->foreign, Stub+Direct foreign->host callback
Control flow: no_unwind
```

---

## 10. 与传统 FFI 的关系

DCI 在能力层面包含传统静态 ABI FFI 的可表达面，但不要求现有 FFI 声明格式与 `.dci` Schema 相同，也不把所有互操作强制降级为 C ABI。

| 场景 | DCI 表达 |
|---|---|
| C 自由函数 | L0 Direct |
| C ABI struct / callback | L0 Direct 或 Stub |
| C++ 自由函数 | L1 Direct；最终 mangled symbol 由 Adapter 给出 |
| 按值 non-trivial 对象 | L1/L2，必须具有明确 lifecycle |
| C++ class 和继承 | L2 Direct 或 Stub |
| 完整虚表多态 | L3 Direct 或 Stub |

因此：

- 已有 C ABI FFI 可以作为 DCI L0 实现继续工作；
- 上层语言仍可绑定 C 函数和自由 C++ 函数；
- DCI 不要求把类压缩为 `void* + create/destroy/call_*`；
- ABI、目标或控制流不兼容时，Consumer 仍必须拒绝或选择 Stub，不能以“兼容 FFI”为理由忽略差异。

---

## 11. 异常、panic 与 unwind

### 11.1 默认规则

所有未声明传播 Profile 的 DCI 边界均为 `no_unwind`。调用双方必须把边界视为不可展开帧。

### 11.2 显式传播

只有同时满足以下条件时，非本地控制流才可以跨越：

1. 契约声明共同 propagation ABI 及版本；
2. 双方 Consumer/runtime 明确声明支持；
3. personality、cleanup、对象销毁和线程状态规则兼容；
4. 自动化测试验证双向传播和清理行为。

仅仅因为两个实现都使用 LLVM、DWARF 或 Windows SEH，不代表它们具有相同语言传播 ABI。

### 11.3 转换策略

无法共同传播时，Adapter **MUST** 在生产语言里生成侧边 translator：捕获 C++ `throw`、Rust `catch_unwind` / 稳定 `Result::Err`，填入闭合 `dci.Failure`，经 tagged 记录 `dci.Translated_*` 按值返回。原符号保持 `may_unwind` 且 Direct 拒绝。translator 自身是 `no_unwind` 叶，`synthesis.strategy = translate_unwind` 表示 **backend 编译函数体**。Core Consumer **MUST NOT** 为该策略合成 LLVM EH（不得登记 `__CxxFrameHandler3` / `__gxx_personality_v0`），也不得 `forward_direct` 到会抛的原符号。

Vyx 宿主绑定是 Converter/前端：`extern "dci"` 名字仍指向原操作，链接 translator，调用后按字段搬运到 `Result<T, dci.Failure>`（禁止把 tagged 记录 bitcast 成 Result）。`dci.Failure` 不是 Core 类型。生产库头文件 / crate 源码零改动。MSVC 捕获走 `dci_msvc_capture_current`（ThrowInfo，`producer_tag=1`）；Itanium 捕获走独立的 `dci_itanium_capture_current`（`__cxa_current_exception_type` / `__cxa_exception`，`producer_tag=2`），禁止复用 MSVC EH 表。Zig error union 仍走各自 Adapter。

转换必须写入契约，不能静默改变语义。

---

## 12. 泛型与具体实例

### 12.1 请求与闭合事实

开放实体 `Box<T>` 不能直接成为 ABI 实体；`Box<i32>` 与选中的成员函数可以。
新实例不必在消费端请求前已经导出：Active Adapter 可以请求生产端生成它。

1. 宿主生成带有实体身份、具体实参、操作和目标上下文的请求。
2. Adapter 请求生产端执行约束检查、重载或特化选择、实例化。
3. Adapter 取得所需的闭合事实和实现产物；缺少事实或生产端拒绝时，请求失败。
4. 构建系统验证结果，将匹配的对象文件或库纳入链接。

开放请求属于控制面，不得作为未解决的语义问题泄漏到 canonical ABI 契约。
Consumer 必须在发射每项操作前取得所需事实；不能先猜布局，再等待后续构建修正。

### 12.2 类型接入条件

基础类型、DCI 导入的原生类型，以及受支持的宿主记录表示，都可以成为泛型实参候选。
表示敏感操作必须验证大小、对齐、字段布局和有效表示；调用还需要传参方式、生命周期与生产端约束。

不要求把所有宿主对象转换成外部对象。能满足具体操作要求的类型可以接入；
无法证明兼容的组合必须拒绝，不得用同尺寸、名字相近或裸指针转换替代证明。

### 12.3 当前 Vyx 路径

C++ / Rust 项目通过 `.dci_open` 提交闭合函数请求，由 external 后端调用生产端物化。
泛型记录在 Vyx 代码生成之前需要布局，因此当前使用的实例布局必须预先进入契约。
字段布局声明合并后还须由生产端编译器复核。

**实例需求由消费方驱动**：lowering 把它**实际物化**的每个泛型实例写成
`.dci_open` 里的 `instance <类型文本>` 行——包括只作为某个被调方法**返回类型**
出现的实例。构建内置的闭合器
[dci_close_instances.py](../tools/dci/dci_close_instances.py) 据此让生产端 adapter
闭合**恰好这些**实例，输出一份补充 `.dcib`（原契约不被改写）；构建再对新契约
在失败后重新 lowering，每个 job 最多三轮实例补充重试；尚非完整全局语义/制品固定点。
因此 Vyx 构建内不需要手写 `--export-instance`
名单；该旗标保留为显式覆盖手段（契约未携带必要参数的生产端，或闭合器把生产端
以「返回类型无实测布局」拒绝的实例回喂时）。

同一实例在消费方拼写（`native.SinkG<i32>`）、生产端拼写（`crate::SinkG::<i32>`）
或 C++ 原语拼写（`SinkG<unsigned int>`）下必须匹配到同一形状，闭合器先做规范化：
折叠空白、模块前缀与 C++ 原语短语（`unsigned int` -> `u32`）。
后端收到的 `--descriptor` 是**换行拼接的多根列表**（主契约 + 补充契约），
各根分别解码，生产端检查只作用于第一根。

`active_protocol.py` 定义环境、实体、请求与产物包身份；
`artifact_bundle.py` 提供内容校验、清单和原子发布。
协议库与当前 sidecar 构建集成是不同层次，不表示所有编译器查询都已接入持久会话，
也不表示所有构建产物均通过产物包存储发布。

完整工程与命令见 [dci_opengeneric](../tests/projects/dci_opengeneric/README.md)。

---

## 13. 验证、兼容性与失败策略

Consumer 在使用 `.dcib` 前 **MUST** 验证：

- Core/Schema/Profile 版本可接受；
- target triple、pointer width、endianness 和 ABI family 匹配；
- 描述中的 build ID、摘要或等价身份与实际链接制品匹配；
- 所有被引用类型均有闭合布局；
- 符号、调用约定和 lifecycle 操作存在；
- vtable/base offset 等 L2/L3 信息完整；
- 位域的 storage range、bit range、符号性和读写策略合法且不越过布局；
- 每条 virtual-base adjustment 的表指针、entry 宽度和位移规则能被当前 Consumer 执行；
- 按值参数/返回的 source ownership 与 ABI lowering 一致，hidden parameter 顺序和属性完整；
- copy/move/destroy/retain/release 的可用性与每个调用点的 ownership 相容；
- runtime type/cast operation 完整描述成功、失败和 pointer adjustment；
- control-flow 契约被双方支持；
- Stub 所需工具链与输出可用。

验证失败时必须给出诊断并停止相关绑定的代码生成。禁止使用默认 offset、猜测 slot、按名称猜 mangling 或回退到未声明的 C ABI。

兼容性由三部分共同决定：

```text
Schema 兼容 + Profile 兼容 + 目标 ABI 兼容
```

源代码名称相同不代表 ABI 兼容；同一语言由不同编译器或不同编译选项产生的实体也可能不兼容。

---

## 14. DCI-Interp 的边界

DCI-Interp 是为解释型语言和 VM 设计的独立协议名称。它至少需要单独定义：

- 动态值和 runtime type tag；
- VM/解释器对象 handle；
- GC root、pinning 和移动对象；
- event loop、协程和异步调度；
- 动态异常与 stack ownership；
- re-entry、线程附着和 runtime lock；
- JIT code identity 和失效规则。

DCI Core 的原生布局、静态符号和编译期 lifecycle 契约不能直接替代这些规则。实现不得把 DCI-Interp 支持混入 DCI Core 的符合性声明。

---

## 15. 符合性

### 15.1 Adapter 符合性

Adapter 必须证明同一输入、目标和配置生成确定的契约，并通过布局、符号、调用约定、lifecycle 和 vtable fixture 验证。

### 15.2 Converter 符合性

Converter 必须证明生成的宿主声明与契约一一对应，对无法映射的能力给出明确诊断，并且不引入隐藏 ABI 假设。

### 15.3 Consumer 符合性

Consumer 必须通过：

- 正向调用测试；
- 按值与生命周期测试；
- 错误目标/错误版本拒绝测试；
- Direct 路径 IR 或机器码结构测试；
- Stub 生成、编译和链接测试；
- 声明支持 L3 时的多继承、虚表和 reverse callback 测试；
- 声明支持相应能力时的 copy/move/ownership 负例与精确一次销毁测试；
- 位域边界值、有符号扩展、相邻位保持和 address-of 拒绝测试；
- class/aggregate 的 direct、indirect/byval、sret 和 coerce 测试；
- 虚继承的动态表调整、菱形继承和错误 adjustment 拒绝测试；
- RTTI/cast 的成功、失败、null、pointer adjustment 及无能力拒绝测试；
- Stub backend capability 缺失、工具链不匹配和控制流越界拒绝测试；
- `no_unwind` 或共同 propagation ABI 测试。

符合性是按 **Core 版本 + Profile + 目标 + 层级 + 模式 + 方向** 声明的，不能只写“支持 DCI”。

---

## 16. Vyx：首个实现与标准基线

Vyx 是初始 Consumer 实现。Core 规则、工具行为和实际支持的子集分别描述；
存在某个字段或代码路径，不等于完整符合 DCI Core 1.0 或完整 L3。

当前 Python SDK 1.1 通过统一注册表抽象 Adapter、命令和 Stub 后端。
`OptionSpec` / `param()` 声明共享及语言专用参数，`AdapterRequest` 是 Adapter 的
`build_argv`、`run`、`doctor` 钩子的统一输入。参数按语义目标合并，
例如 `--compiler` 与兼容别名 `--rustc` 使用同一参数；CLI 同时验证参数的语言归属。

内置实现、本地插件和已安装包的 entry point 使用同一套注册接口。
`dci plugins --json` 输出注册结果与合并后的参数模型。
SDK 1.1 与 Core 契约格式 1.0 分别版本化。
命令与扩展入口见 [DCI SDK 指南](../tools/dci/README.zh-CN.md)。

### 16.1 绑定与后端配置

Vyx 使用 `extern "dci"` 与 `@[dci_import("...dcib")]`。
CLI 的 `--dci` 可重复，清单支持 `dci_file` / `dci_files`。
多份契约独立加载，分别关联其生产端与产物，不合并成来源不明确的“mixed”契约。
`extern "cpp"` 和 `@[cpp_include]` 是 C++ Profile 的兼容表面。

`dci_stub_backend` 支持 `direct`、`clang-cpp`、`rustc`、`auto` 和 `external`。
默认值为 `direct`；外部声明不会隐式选择 Stub 编译器。

external 后端配置如下：

| 清单字段 | 环境覆盖 | 作用 |
|---|---|---|
| `dci_stub_backend_tool` | `VYX_DCI_STUB_BACKEND_TOOL` | 后端可执行程序，必需 |
| `dci_stub_backend_tool_args` | `VYX_DCI_STUB_BACKEND_TOOL_ARGS` | 固定启动参数 |
| `dci_stub_backend_source_extension` | `VYX_DCI_STUB_SOURCE_EXTENSION` | 生成 Stub 时必需的源码或 IR 后缀 |
| `dci_stub_backend_dependencies` | `VYX_DCI_STUB_BACKEND_DEPENDENCIES` | 影响产物的脚本、配置与其他文件 |
| `dci_stub_backend_version` | `VYX_DCI_STUB_BACKEND_VERSION` | 后端版本或配置盐 |
| `dci_stub_backend_capabilities` | `VYX_DCI_STUB_BACKEND_CAPABILITIES` | 后端能力集合 |

环境变量覆盖清单；清单先读取 `[target.<name>]`，再读取 `[build]`。
相对路径按项目目录解析。依赖文件必须存在；以 `python` 启动脚本时，
脚本本身也必须列入依赖，不能仅以解释器路径充当实现身份。

构建按顺序调用：

```text
tool [args] emit --descriptor <dcib> --host-source <unit> --output <source> ...
tool [args] compile --source <source> --output <object> ...
```

后端通过规范化请求获得所需操作，不得解析宿主源码来猜测缺失的外部 ABI。
Active 函数请求还通过请求文件交付。完整配置见
[项目清单](PACKAGE_MANIFEST_ZH.md)与 [DCI 工具](../tools/dci/README.zh-CN.md)。

### 16.2 LLVM Consumer 支持范围

| 范围 | 当前行为 | 边界 |
|---|---|---|
| 契约输入 | canonical `.dcib`；多契约独立加载 | JSON 仅用于工具、检查与转换 |
| 目标 | 当前实现要求 64 位、小端 | 不是 Core 对其他实现的限制 |
| 符号与 ABI | 使用 `link_name`、调用约定和受支持的 direct/indirect/byval/sret/coerce/split/inalloca 形态 | 未实现的形态按使用点拒绝 |
| 类型与派发 | 布局、字段、位域、基类调整、dispatch table、受支持的 runtime cast | 不代表完整虚继承或 RTTI 矩阵 |
| 生命周期 | `automatic` 下的局部 copy/move 选择及作用域清理，allocator domain 校验 | 非完整控制流所有权分析；retain 仍显式 |
| Wrapper | 受支持的 `forward_direct` 和 `reverse_override` 合成 | 不代表任意桥接均可在 Consumer 中合成 |
| 失败转换 | `translated` 使用生产端编译的 no-unwind translator | 不把可能抛出的符号直接转为 nounwind 转发 |
| 共享 unwind | AOT：x86_64 Windows MSVC 的 `dci.eh.msvc-cxx.v1`；x86_64 Linux 的 `dci.eh.itanium-cxx.v1`；实际传播及对象清理 | JIT 暂缓；不是通用外部 catch/throw 合成 |
| 整数事实 | cJSON 精确范围 `±2^53` | 不支持以普通 JSON 数字无损携带范围外整数 |

生命周期操作需要契约证据。默认或 `declaration` 绑定不启用自动 binder；
`automatic` 在已支持的局部初始化场景选择 copy 或 move，并安排 destroy/release。
`destructible_only` 移后值的再次使用受到检查。这些局部规则不能替代完整的路径敏感分析。

AOT 共享传播要求模块和被调操作均参与目标对应的版本化 ABI；未知标识、版本、
目标或未参与传播的调用方会被拒绝。原生 C++ 异常穿越 Vyx 帧后保留原身份。
异常展开按逆构造顺序调用已完成对象的 MIR Drop，包括 DCI 内联栈记录、
Vyx 构造器、工厂返回和聚合初始化的局部对象。DCI 指针对象析构后释放存储；
DCI 构造失败只释放已分配存储。正常作用域退出仍使用现有 Vyx 顺序。
清理操作必须具有完整对象的 `void(pointer)` ABI；展开期间析构再次抛异常会终止进程。
Rust panic、Zig error 和 `longjmp` 不会因为填写 C++ unwind 标识而自动获得共享传播能力。

Direct 不增加额外 DCI 桥接层，但保留目标 ABI 本身的调用、派发等成本。
Stub 的成本取决于所生成桥接。

### 16.3 C++ Adapter

`dci_adapter_cpp.py` 是入口，`dci_adapter_msvc.py` 保留实现和兼容入口。
`--toolchain` 指定 ABI 权威编译器；Clang 可以辅助结构化提取，
最终布局、符号与传参事实必须对应选定生产端。

已有路径包括嵌套记录布局、枚举表示、受支持的聚合传参、
类生命周期与派发事实。非平凡类必须具备相应生命周期证据；
不能因一个类可用而假定所有类或目标均受支持。

复用真实编译参数使用 `--cmake-build-path` 与 `--compile-flags`。
版本字符串用于溯源，不要求与宿主编译器版本相同。
`source.language` 不作为 Core Consumer 选择 ABI 算法的依据。

相关项目：[dci_complex_abi](../tests/projects/dci_complex_abi/)、
[dci_opengeneric](../tests/projects/dci_opengeneric/README.md)。

[dci-vector](../probes/gates/dci-vector/README.md) 从仅包含 `<vector>` 的头文件接入
原始开放模板 `std::vector<T>`。Vyx 声明 `std.vector<T>`，直接写
`var a = std.vector<i32>{1,2,3,4,5};` 并调用标准成员。消费方选择 i32/f64 实例，
生产端测量布局、检查 brace 构造与 narrowing；生成的 placement 构造入口承接
initializer-list 边界，普通成员和析构链接标准库原始符号。夹具未预先导出 vector
特化或替代 API。当前该门覆盖 Windows MSVC ABI。

[dci-cpp-ecosystem](../probes/gates/dci-cpp-ecosystem/README.md) 使用固定版本与哈希的
ICU 78.3 原始头、import library、DLL 和 Unicode 数据；接入原始 `UnicodeString`
对象及方法，O0/O2 AOT 输出与独立 C++ 参考一致。入口别名用于让扫描器看见 ICU
宏定义的命名空间，保留原类和符号；借用返回值需显式 ownership 事实。

### 16.4 Rust 与 Zig Adapter

Rust Adapter 通过选定的 rustc 测量布局和 lowering。
`repr(C)` / `repr(transparent)`、C/system 导出是其中一个子集。
已有 Rust 原生路径还覆盖默认记录表示、Rust ABI 调用、thin reference、
fat pointer 和 crate 内 trait 派发。

这是一份与选定 rustc 环境匹配的契约，不是跨 rustc 版本稳定的 Rust ABI。
更换生产端编译器、目标或相关选项后，应重新验证并生成匹配的契约与实现产物。
带数据的枚举、缺少生命周期契约的 `Drop` 类型和无法验证的签名仍可能被拒绝。

未闭合泛型定义不能作为具体 ABI 导出；Active Adapter 可请求生产端生成受支持的实例。
SDK 1.1 已将 `--export-instance` 与 `--export-active-requests` 注册到统一 CLI 的 Rust 参数表。
在 Vyx 构建内，实例需求由消费方驱动（见 §16.5），`--export-instance` 是等价的显式请求路径。
不能用离线导出的限制否定这条构建期路径。

内置 `rustc` Stub 后端分别编译身份生产 crate 与 Stub staticlib，
以保持原生符号身份；Windows 链接还需要 rustc 声明的 native static libraries。
相关项目：
[dci_rust_trait](../tests/projects/dci_rust_trait/)、
[dci_rust_generic](../tests/projects/dci_rust_generic/)、
[dci_multilang](../tests/projects/dci_multilang/)、
[dci_opengeneric](../tests/projects/dci_opengeneric/README.md)。

Cargo 路径复放 Cargo 选定的 locked/offline 依赖图、features、build-script 环境和依赖产物；
可按消费方声明测量 Rust fat reference 的显式 view，并为普通 public 函数生成保留 rustc 身份的
native bridge。真实生态门 [dci-rust-ecosystem](../probes/gates/dci-rust-ecosystem/)
直接使用 registry 中的 `crc32fast` 与 `adler2`，再将 Vyx AOT 输出与独立 Cargo oracle 对比。
Cargo 专用参数当前通过独立入口 `dci_adapter_rust.py` 使用，尚未注册到统一 SDK CLI。
该门包含显式生成的 native bridge，不证明 Rust 跨界调用无需桥接，也不证明任意 crate、
枚举、trait 或标准库泛型均可接入。生产端测量和能力准入仍是必要条件。

**泛型**生产端 trait 只能以**闭合实例**被继承。`pub trait SinkG<T>` 的实例由构建
从消费方发现：lowering 写 `instance` 行，闭合器让生产端 adapter 闭合该实例，
适配器据此产出布局（`native.SinkG<i32>` -> rustc 路径 `crate::SinkG::<i32>`）、
vtable 与 `consume` 符号；消费方写
`class VyxHostG : native.SinkG<i32>`，生成的 stub 是
`impl native::SinkG::<i32> for __dci_vyx_stub_VyxHostG`。
显式传 `--export-instance 'SinkG<i32>'` 产出同样的事实，仍是 Vyx 构建之外
请求实例的受支持方式。开放的 `SinkG` 在契约里
没有 vtable，因此基类子句若不带实参地写它，会在**生成 Stub 时**带源码位置报错，
而不是拖到链接期。

Zig Adapter 已注册到 CLI；支持范围以其验证的导出为准。
C++ / Rust 的 Active Adapter 能力不自动扩展到 Zig。

### 16.5 开放泛型与缓存

当前 C++ / Rust 项目经 `.dci_open` 向 external 后端提交闭合函数请求；
生产端负责约束检查与物化。泛型记录在 Vyx 代码生成前需要实例布局，
项目通过契约、名称映射和生产端布局复核提供这些事实。

**消费方驱动的实例闭合**（2026-09-24 落地）：lowering 把实际物化的每个泛型实例
记为 `.dci_open` 的 `instance <类型文本>` 行，构建内置的
[dci_close_instances.py](../tools/dci/dci_close_instances.py) 让生产端 adapter 闭合
恰好这些实例并输出补充 `.dcib`，失败的编译 job 再对新契约重新 lowering，
每个 job 最多重试三轮实例闭合。这是有上限的实例补充重试，尚非完整的全局语义与制品闭包。
原契约不被改写；不需要手写 `--export-instance` 名单。sidecar 与既有请求按行
取并集，成功重降级不会抹掉失败轮发现的实例行。闭合器按规范化形状匹配实例名
（折叠空白、模块前缀与 C++ 原语短语，如 `unsigned int` -> `u32`），并把生产端
以「返回类型无实测布局」拒绝的实例回喂为追加 `--export-instance` 重试。

编译单元缓存纳入 Descriptor 内容，而不是仅依赖文件大小与修改时间。
Stub 缓存还包括命令、后端身份、声明依赖与版本盐。
Active 协议库分别记录实体、请求和产物包身份，产物包支持内容校验与原子发布。
缓存复用须处于相同有效域，不能把一次语义检查结果无条件复用于其他生产环境。

### 16.6 源码与验证入口

| 范围 | 入口 |
|---|---|
| CLI 与编码 | [dci.py](../tools/dci/dci.py)、[CLI 测试](../tools/dci/tests/test_dci_cli.py)、[DCIB 测试](../tools/dci/tests/test_dcib.py) |
| Active 请求协议 | [active_protocol.py](../tools/dci/active_protocol.py)、[协议测试](../tools/dci/tests/test_active_protocol.py) |
| 产物身份与发布 | [artifact_bundle.py](../tools/dci/artifact_bundle.py)、[产物包测试](../tools/dci/tests/test_artifact_bundle.py) |
| 生产端实例化 | [C++ 测试](../tools/dci/tests/test_active_cpp.py)、[Rust 测试](../tools/dci/tests/test_active_rust.py)、[开放泛型项目](../tests/projects/dci_opengeneric/README.md) |
| 消费方驱动的实例闭合 | [dci_close_instances.py](../tools/dci/dci_close_instances.py)、[开放泛型项目门](../tests/projects/dci_opengeneric/run_vyx.sh) |
| 生命周期与 wrapper | [HIR binder](../bootstrap_compiler/src/hir/resolve/dci_binder.vyx)、[LLVM Consumer](../bootstrap_compiler/src/codegen/llvm/llvm_lower.vyx)、[Consumer 测试](../tools/dci/tests/test_dci_consumer.py) |
| 存储与异常清理 | [llvm_lifetime.vyx](../bootstrap_compiler/src/codegen/llvm/lifetime/llvm_lifetime.vyx)、[llvm_unwind.vyx](../bootstrap_compiler/src/codegen/llvm/lifetime/llvm_unwind.vyx)、[存储回归](../probes/gates/dci-storage/README.md)、[异常门](../probes/gates/dci-exceptions/README.md) |
| 生态与重复压测 | [C++ ICU](../probes/gates/dci-cpp-ecosystem/README.md)、[Rust Cargo](../probes/gates/dci-rust-ecosystem/README.md)、[DCI 工业压测编排](../probes/gates/dci-industrial/README.md) |

契约验证、生产端事实复核和编译链接运行分别检查不同问题。
应按修改的操作范围运行对应检查，不能仅凭 Schema 通过就宣称端到端可用。

### 16.7 各语言能力矩阵（实测基线，2026-10-01）

本节记录夹具覆盖，不代表完整 Core 符合性或已经达到工业级。最新 AOT 结果主要覆盖
Windows x86_64；异常门还将 Linux x86_64 对象在 WSL 下链接运行。较早条目说明既有
验证入口，换工具链或目标仍需重跑。**「未验证」表示该形态没有已确认的端到端结果，
既不得当作已支持宣传，也不能反推为不可能**。「不支持」表示有负向测试或实测限制，
会以诊断拒绝。

消费方 → 生产端的 Direct 调用：

| 能力 | C++（clang / gcc / msvc 工具链） | Rust（rustc） | Zig |
|---|---|---|---|
| 自由函数、标量、指针 / 引用 | 是（[dci_abi_stress](../tests/projects/dci_abi_stress/)） | 是（[dci_rust_generic](../tests/projects/dci_rust_generic/) direct 契约） | 是（[dci_zig_abi](../tests/projects/dci_zig_abi/)） |
| 聚合按值 / direct、indirect、byval、sret | 是（[dci_complex_abi](../tests/projects/dci_complex_abi/)） | 是（[dci_opengeneric](../tests/projects/dci_opengeneric/README.md)：`Vec2`、`Pair2` 16 字节 sret） | 是（`Pair` 传值） |
| 位域、枚举表示 | 是（dci_abi_stress） | 是（dci_rust_generic 位域契约） | 未验证 |
| Consumer 自有局部存储 alloc/destroy/free 成对 | 是（[dci-storage](../probes/gates/dci-storage/README.md)：重复作用域、早退、重赋值、循环；[dci-exceptions](../probes/gates/dci-exceptions/README.md)：构造失败与展开） | 不由这些 C++ 用例证明 | 是（dci_zig_abi 独立的 `native_heap` / `native_free` 路径） |
| 类布局、继承、基类调整、虚方法 direct 调用 | 是（dci_complex_abi 多继承 + `virtual`；dci_abi_stress 模板/多态层级） | 是（dci_rust_generic） | 部分（record 类有；无虚表用例） |
| 真实第三方库 | 是（[dci-cpp-ecosystem](../probes/gates/dci-cpp-ecosystem/README.md)：固定 ICU 78.3、原始 `UnicodeString` 与原生符号、O0/O2 参考一致）；Qt/spdlog 为较早夹具，不作为本次当前结果 | 是（[dci-rust-ecosystem](../probes/gates/dci-rust-ecosystem/README.md)：locked Cargo `crc32fast` + `adler2`、实测 `&[u8]` view、生成 native bridge、O0/O2 oracle 一致） | 未验证 |
| 原始标准库开放模板 | 是（[dci-vector](../probes/gates/dci-vector/README.md)：`std::vector<T>`、消费方选择 i32/f64、brace 构造检查、增长与越界展开压力） | 任意标准库泛型未验证 | 未验证 |
| 外部工具链 provider（nvcc / CUDA） | 是（[dci_cuda](../tests/projects/dci_cuda/)：契约由纯 C++ 头导出；自由函数 `void*` 走 `Translated` 回退，须自查） | — | — |
| 开放泛型（调用点闭合 + Active Adapter 实时物化，泛型 record 成员方法） | 是（dci_opengeneric cpp target） | 是（dci_opengeneric rust target） | 不支持（C++/Rust 的 Active 路径不自动扩展到 Zig） |
| 多份点对点契约共存于一个消费方 | 是（[dci_multilang](../tests/projects/dci_multilang/)：Rust + Zig + MSVC C++ ABI 同一工程） | 是 | 是 |

生产端 → 消费方的反向覆写（Stub；vtable 由生产端实测，Vyx 产出 stub 并转发 thunk）：

| 能力 | C++ | Rust | Zig |
|---|---|---|---|
| 继承具名基类 / trait + `override` | 是（[dci_cpp_trait](../tests/projects/dci_cpp_trait/)：实测 C++ 虚派发）；[Qt counter](../probes/gates/dci-qt-counter/README.md) 验证 Windows AOT 构造、原生事件覆写与清理 | 是（[dci_rust_trait](../tests/projects/dci_rust_trait/)：`SinkG<i32>`） | 未验证（无测试） |
| 继承**闭合**泛型 trait 实例 | 是（[dci_cpp_trait](../tests/projects/dci_cpp_trait/)：`VyxHostI32 : SinkG<i32>`，消费方发现需求，由生产端补充实例；不手写 `--export-instance`） | 是（[dci_rust_trait](../tests/projects/dci_rust_trait/)：同一消费方发现与生产端闭合路径；显式 `--export-instance 'SinkG<i32>'` 产出等价事实） | 未验证 |
| 泛型 Vyx stub 类（`class VyxHostTG<T> : native.SinkG<T>`，实例自动分化） | 是（dci_cpp_trait，2026-09-24：`VyxHostTG<i64>` / `VyxHostTG<VyxBox>`，直接虚调用） | 是（dci_rust_trait，2026-09-23：i32 / i64 / char 三实例 + 直接虚调用与跨界派发） | 未验证 |
| 消费方自有 `@[repr(C)]` record 作泛型实参 | 是（dci_cpp_trait，2026-09-24：`VyxBox` 12 字节 = indirect + sret） | 是（dci_rust_trait，2026-09-23：`VyxBox` 12 字节 = indirect + sret；需双侧同布局） | 未验证 |
| 生产端 record 作**继承泛型基类**的类型实参 | 是（dci_cpp_trait，2026-09-24：8 字节 `Sample`。Win64 MSVC 非静态成员函数返回任意 UDT 一律走隐藏指针——适配器把成员返回记为 `sret`；覆写 thunk 则按 `extern "C"` 自由函数跨界，8 字节 record 双向 coerce 成单寄存器。此前实测的三处独立限制随开放泛型路径 + 该 ABI 修正解除） | 不支持（`SinkG<Sample>` 形态实测三处独立限制；record 作**自由函数**泛型实参不受此限，见 dci_opengeneric） | 不支持 |

机制门（[probes/gates](../probes/gates/)，覆盖跨语言通用机制而非绑定特定生产端语言）：

| 门 | 覆盖 |
|---|---|
| [dci-active](../probes/gates/dci-active/) | Active Adapter：rust + cpp 按需闭包泛型调用、生产端对象发布入 bundle 缓存、源码与适配器删除后仅凭 bundle 离线 replay、llvm-nm 符号唯一性核对 |
| [dci-failure](../probes/gates/dci-failure/) | 错误路径：`Result<i32, dci.Failure>` 的 Err 载荷（message / payload / producer_tag）跨契约传递与匹配 |
| [dci-inalloca](../probes/gates/dci-inalloca/) | 大聚合按值（4×i64 `Blk`）的 inalloca 传参路径 |
| [dci-landingpad](../probes/gates/dci-landingpad/) | 契约调用异常传播（landingpad）路径 |
| [dci-storage](../probes/gates/dci-storage/README.md) | Consumer 自有局部存储的分配、析构、释放配对；重复作用域、早退、重赋值和循环退出 |
| [dci-exceptions](../probes/gates/dci-exceptions/README.md) | AOT MSVC/Itanium 共享传播、析构顺序、构造失败、内联记录和二次展开 |
| [dci-vector](../probes/gates/dci-vector/README.md) | 原始标准 vector 模板、生产端 narrowing 拒绝、分配追踪与原生越界异常 |
| [dci-cpp-ecosystem](../probes/gates/dci-cpp-ecosystem/README.md) | ICU 78.3 原始 C++ API 与独立原生参考 |
| [dci-rust-ecosystem](../probes/gates/dci-rust-ecosystem/README.md) | locked/offline registry crates、Cargo 环境复放与原生 oracle |
| [dci-industrial](../probes/gates/dci-industrial/README.md) | 真实夹具重复运行、有界并发、子进程耗时与进程树 RSS 采样 |

Qt counter 门已验证 Windows AOT 的独立 Adapter 准备、可见 Vyx 定义、原生事件派发，
以及 O0/O2 连接生命周期和 shared ABI 清理。其他保留的 Qt 门未在本轮执行；早期失败记录见
[dci-storage](../probes/gates/dci-storage/README.md)。旧 `tests/projects/dci_spdlog`
夹具及 `probes/gates/gate-c` 已于 2026-09-28 退役。DCI 必跑项目为
`dci_cpp_trait`、`dci_rust_trait`、`dci_multilang`、`dci_zig_abi`，再按改动范围运行机制门。

明确不支持（有负向测试或实测锁定，按使用点以诊断拒绝）：

- 带数据的 ZST 参数（dci_rust_generic `zst_unsupported`）；
- 非平凡 class 按值传参 / 返回（`class_value_unsupported` / `class_return_unsupported`）；
- 缺生命周期证据的按值对象（`lifecycle_value_unsupported`）、隐藏参数 ABI（`hidden_parameter_unsupported`）；
- 8 / 16 字节多字段聚合的 `split` 传参形态（12 字节 indirect + sret 是支持路径）；
- 派生 Vyx stub 类声明自己的字段（stub 对象布局照抄基类描述）；
- `x.func()` 形式的虚派发语法与 C shim 泛型导出（均不存在）；
- CUDA 独立语言族（C++ 工具链族仅 clang / gcc / msvc；CUDA 经 C++ 头接入）。

已知现状（如实记录，随基线更新）：

- 消费方驱动的实例自动发现已于 **2026-09-24 落地**：lowering 把物化的泛型实例记为
  `.dci_open` 的 `instance` 行，构建最多三轮补充实例重试，工程无需手写 `--export-instance`
  名单（机制见 §16.5）；`--export-instance` 保留为 Vyx 构建之外的显式请求路径。
- `dci_rust_generic` 此前 9 项子用例失败已修复，本基线在自举编译器下全量通过。

---

## 17. 非规范性 JSON 诊断示例

以下 `.dci` JSON 仅展示逻辑信息结构，不是 Consumer 输入，也不定义正式 Schema；实际项目须将等价契约编码为 `.dcib`：

```json
{
  "$schema": "dci-1.0.schema.json",
  "dci": "1.0",
  "kind": "abi",
  "profile": {
    "id": "native-example",
    "version": "1.0",
    "level": "L2",
    "consumer_modes": ["direct", "stub"]
  },
  "source": { "language": "example-compiled-language" },
  "target": {
    "triple": "x86_64-pc-windows-msvc",
    "pointer_width": 64,
    "endianness": "little"
  },
  "control_flow": { "default_boundary": "no_unwind" },
  "exports": {
    "layouts": [
      {
        "type_name": "example.Multi",
        "size": 40,
        "alignment": 8,
        "representation": "native",
        "bases": [
          {
            "type_name": "example.LeftBase",
            "adjustment": { "kind": "constant", "offset": 0 }
          },
          {
            "type_name": "example.VirtualBase",
            "is_virtual": true,
            "adjustment": {
              "kind": "table",
              "table_pointer_offset": 8,
              "table_entry_offset": 12,
              "table_entry_size": 4,
              "table_entry_signed": true,
              "displacement_base_offset": 8
            }
          }
        ],
        "fields": [],
        "lifecycle": {
          "ownership_model": "unique",
          "copy_semantics": "forbidden",
          "move_semantics": "operation",
          "destruction": "operation",
          "moved_from_state": "destructible_only",
          "operations": {
            "move": { "symbol": "example_multi_move" },
            "destroy": { "symbol": "example_multi_destroy" }
          }
        }
      }
    ],
    "symbols": [],
    "dispatch_tables": [],
    "runtime_type_operations": []
  }
}
```

正式 Schema 必须独立版本化，并精确定义字段类型、默认值、canonical encoding 和兼容规则。

---

## 18. 规范总结

DCI 的判断标准不是“能否调用一个外部函数”，而是：

1. 外部差异是否已被 Adapter 转化为完整、具体、可验证的 ABI 契约；
2. Converter 是否忠实生成宿主绑定而没有重新解释外部语义；
3. Consumer 是否在 Direct/Stub 中忠实执行契约；
4. lifecycle、unwind、泛型和安全边界是否明确；
5. 实现是否按 L0-L3、模式、方向、Profile 和目标给出可验证的符合性声明。

只要这些条件成立，DCI 就可以在不要求被消费语言主动配合的情况下，实现编译型语言之间超越 C ABI FFI 的对等互操作。

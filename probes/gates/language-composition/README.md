# 语法组合生成门

这套门用规则生成程序，不维护一份手写输入清单。`model.py` 定义产生式、类型约束和
独立解释器；`suite.py` 枚举组合、生成源码、运行 SDK 编译器、比较结果并缩减失败。
只使用 Python 标准库。Windows 另需 PowerShell 7，复用仓库的进程 Job 限额工具。

## 覆盖的含义

`--depth 2` 枚举规则表中所有零层、一层、**有序两层**组合，允许规则重复。
每条规则接受 i64 观测值，在内部引入数组、元组、泛型、class、接口、枚举、
Option/Result、借用、闭包、Vec、String 或 StringBuilder，再产生 i64 观测值。
例如 `builder,enum` 把 StringBuilder 的结果交给枚举构造和 match；反向组合
`enum,builder` 单独生成。`inline` 和 `bound` 分别保留嵌套表达式和显式局部绑定。
常量域默认为 `-7,0,13`，可显式扩大。

类型树另外枚举 i64、String、带 drop 的 Tracked 三种叶子，以及 Vec、Option、Result
三种类型构造器；默认穷举两层嵌套。例如 `Vec<Option<String>>` 和
`Option<Vec<String>>` 都会构造真实值、取出拥有的 payload、检查内容和最终清理。
`--type-context-depth 1` 把每棵类型树的观测值再交给全部一层表达式规则。
当前默认域有 47 条表达式规则，共 13,776 个合法组合、3,168 个非法组合，合计
16,944 个输入。随机样本另外计数，不包含在这个穷举数中。

合法程序的预期结果由 Python 解释规则树计算，**不从 Vyx 编译结果提取**。
还检查调用次数、drop 次数、defer 次数及资源内容断言。负例在规则树叶子嵌入
非法操作，检查预期诊断，单纯非零退出不能算通过。

这是指定规则、常量域和深度内的穷举。它不穷尽任意程序，也不代表全部语言特性
已经覆盖。`plan` 和 `suite.json` 明列未建模的 DCI ABI、async、Effect、Reflection、
Migrate、动态分发、裸指针、异常、浮点和独立库/VYI 链接。
手写的真实项目门和 DCI 门仍需运行。

`check_edges.py` 补充拥有 payload 的 `?`、临时对象借用、参数包遮蔽、泛型集合遍历、
推导返回类型的闭包、循环内返回、捕获环境与聚合参数/返回值，以及非法传播等定向输入。
自定义迭代器的拥有 payload 检查每轮析构、defer、break、continue、return、
`?` 提前返回和将元素所有权移出循环；不同 nominal 迭代器使用不同观测结果。
标准库遍历另外检查 String 深拷贝、自定义 Clone/析构次数、24 字节值类型返回、
仅提供 next 的迭代器、提前返回 None，以及与标准类型同名同布局的用户迭代器。
值参数取得传入资源的清理责任；函数正常返回时自动析构，`return` 转移或显式借用时由
移动/借用事实停用参数清理。`owned_param_lifetime` 同时覆盖普通 class、Vec、共享借用
和按值返回，检查不会泄漏也不会双重析构。
`conditional_raw_param_lifetime` 覆盖值参数经 C `memcpy` / `memmove` 转入裸存储的路径：
分支、提前返回、零次/一次循环、defer 和短路求值均分别执行转移与不转移两种情况。
清理使用运行时所有权标记，不能因为函数体某处有拷贝就取消所有出口的参数析构。
该夹具持有真实 String，并进入下述分配计数门，检查转移前后内容及最终分配/释放平衡。
`conditional_owned_param_lifetime` 检查三元表达式返回拥有参数或新对象，选中参数时不能
在返回前析构它；未选中的参数仍须清理。两个分支均检查 String 内容和最终释放计数。
具名调用按参数名绑定类型、泛型参数与所有权；实参表达式按源码顺序各求值一次，
方法接收者和管道前缀先于显式实参求值。位置实参依次填入未由名字指定的参数，
省略项使用声明中的默认值。未知名称和重复名称必须在 Sema 报错。
`fn(...)` / `cfn(...)` 值的类型不包含参数名，间接调用只接受位置实参，不能静默忽略名称。
`named_arguments`、`named_argument_order`、`named_owned_arguments` 与 `named_raw_param_lifetime`
分别检查重载/默认值/泛型、求值顺序、拥有值与借用值交错，以及 C 字节拷贝转移。
拥有值夹具同时验证普通函数、实例方法和泛型函数，进入真实 String 分配计数门。
`named_c_varargs` 检查 C ellipsis 尾部完整传递及交错具名/位置实参的求值顺序，
`c_vararg_promotions` 检查窄整数和 f32 的 C 默认类型提升，`named_pack` 检查 Vyx 异构参数包。
`c_varargs` 规则通过 libc snprintf/strtoll 往返数值，独立检查格式前缀和调用次数，
与其他产生式组合；它只覆盖这一有界缓冲区路径，不代表通用裸指针操作已建模。
普通参数名 `args` 与其他名字遵守相同的类型、参数个数和移动检查；只有语法中的
`...` 标记定义参数包。对应正例覆盖具名调用、泛型、方法及真实 String 清理，负例
检查类型错误、缺少/多余实参和移动后读取。`args_parameter` 将此路径纳入生成规则。
`pipe_default` 将管道前缀、默认参数与嵌套调用组合；前缀占据第一个用户形参，不能
再求值它的默认表达式。`pipe_default_arguments` 额外检查副作用次序及异构形参。
管道前缀和后续显式实参都按各自形参登记移动；不能在传走拥有值后继续读取它。
对应负例检查前缀移动和显式实参移动，分配门同时检查拥有值传递和共享借用。
合法输入比对完整输出与清理计数；负例必须匹配诊断，
访问冲突、超时或内存限额不能代替预期拒绝。默认执行 O0/O2 × 默认/4 CGU：

```powershell
python probes/gates/language-composition/check_edges.py --compiler $compiler --out .cache/grammar-edges
```

Windows 的 `check_str_cleanup.py` 另外拦截生成 IR 中的 malloc/calloc/free 与运行时字符串
分配、释放入口，转发给同次 SDK 的真实分配器。C++ 夹具自身的分配不被改写。
Vec<str> 在值形式与共享借用中各遍历一次，必须观测到四个新字符串，并全部释放：

```powershell
python probes/gates/language-composition/check_str_cleanup.py --compiler $compiler --out .cache/str-cleanup
python probes/gates/language-composition/check_str_cleanup.py --compiler $compiler --cases str_clone_iteration,str_array_iteration,owned_array_clone,owned_array_lifetime,format_cleanup --out .cache/iteration-allocation
python probes/gates/language-composition/check_str_cleanup.py --compiler $compiler --cases conditional_raw_param_lifetime --out .cache/raw-transfer-allocation
```

此计数仅覆盖被测程序发起的分配，不是整个进程的泄漏审计；Linux 对应的拦截门尚未实现。

分配门还覆盖资源数组及标准库 Display 输出。自定义 Drop 按标准库约定显式清理它管理的字段；
数组负责调用元素 Drop，不能把用户析构函数漏写字段清理归为数组清理成功。
格式化检查同时输出自定义 Display、数字、借用 str 与 String，确认格式化临时值被释放、
已有字符串仍可继续使用。

当前标量域主要是 i64，另测 i8 显式窄化、i32 全局值提升；普通数组和元组元素为
i64，资源数组规则另外使用有 Clone/Drop 的 class。自定义迭代器覆盖普通 class 与 `Cursor<Tag, Item>`，元素带非零高 32 位，
用于识别把元素类型误猜为 i32 或第一个泛型参数的截断。
Option/Result 规则覆盖 Some/None、Ok/Err 分支和 `?` 的成功、提前返回路径；
传播函数同时持有带 drop 的局部对象和 defer，检查两个出口的清理次数。
数组遍历同时检查值与共享借用；变参遍历展开 i64、u8、i32 实参及空包。
`own_iterator` 生成每次创建新 Tracked 的迭代器，按输入选择 break、continue 或完整遍历，
独立计算结果、调用、析构和 defer 次数；`closure_loop` 在 while 内返回，检查推导的 i64 签名。
`next_only_iterator` 由 None 结束，计入最后一次 next 调用；`borrowed_owned_vec` 遍历
共享借用的 Vec<String>，在副本清理后再次检查源元素内容。
`owned_array` 检查共享借用遍历中的真实 Clone 调用、源元素内容和原值/副本的清理次数；
`inherited_iterator` 检查从声明父类取得 next 协议。负例加入数组移动后读取及重复资源初始化。
`named_call` 将参数重排与泛型推导嵌入生成表达式，并检查子表达式调用次数。
类型树仍主要覆盖 Some/Ok 构造和拥有 payload 的取出。泛型继承布局、其余整数宽度、
Dict、带约束的复杂参数包等还未进入这个规则表。
这些范围不能从当前组合通过推断为已经验收。
Vec 和标准 Iterator 的非平凡元素经真实的 next/Clone 方法产生独立值，缺少 Clone 时必须拒绝。
固定数组的非平凡元素通过普通 MIR 调用 Clone；嵌套数组按结构递归复制和清理。
数组离开作用域时逆序调用叶子析构，清理代码使用循环，不按元素数量展开。
DCI shared_abi 展开尚不能执行数组的结构化清理；拥有此类数组的展开帧明确拒绝，
由 dci-exceptions 的 reject_array_unwind.vyx 验证，不能算作异常数组清理已实现。

## 生成

从仓库根目录执行。先查看规模，再生成不可覆盖的结果目录：

```powershell
python probes/gates/language-composition/suite.py plan --depth 2
python probes/gates/language-composition/suite.py generate --depth 2 --seed 20261014 --out .cache/grammar-corpus
```

规则表、生成器哈希、种子、全部 recipe、源码哈希和预期值写入 `suite.json`。
生成产物保留在本地缓存。默认一次程序装入 128 个合法组合；每个非法组合单独编译，
避免第一条诊断掩盖其他输入。规模超过 `--max-cases` 时明确拒绝，不截断枚举。

调整域示例：

```powershell
python probes/gates/language-composition/suite.py generate --rules builder,enum,closure,drop --depth 3 --values=-9,-1,0,1,9 --negative-depth 2 --out .cache/grammar-small
python probes/gates/language-composition/suite.py generate --depth 2 --random-cases 500 --seed 42 --out .cache/grammar-extra
python probes/gates/language-composition/suite.py generate --rules builder,drop,closure --type-depth 3 --type-context-depth 1 --out .cache/grammar-owners
```

随机样本是额外的深度 1..6 输入，不替代穷举部分。最大深度 6、叶子范围
[-1000,1000] 保证当前规则的 i64 算术不会溢出，显式窄化也先归一到 i8 范围。
增加规则时必须同时维护这个约束及解释器。
`plan` 的批次数统计穷举部分；随机尝试数另列。执行报告按实际去重后的输入计数。

## 执行

显式传入本次源码构建的 SDK 编译器及同目录的配套 backend/runtime。示例中的
`$compiler` 按根 AGENTS.md 取得，不使用旧 seed 代替。

```powershell
python probes/gates/language-composition/suite.py run --suite .cache/grammar-corpus/suite.json --compiler $compiler
```

默认矩阵：单文件、项目同模块、项目跨模块 × O0/O2 × 默认/1/4 CGU，共 18 个单元。
项目路径调用 `--src=project ... --target probe -j20`。构建采用 2 个 Vyx slot，
CGU 发射线程为 1，Windows 编译/运行进程树默认限额 2048 MiB private commit、120 秒。
限额或超时失败单独记为 `resource`，不能记为语义诊断通过。
POSIX 使用每进程 `RLIMIT_AS` 和进程组超时终止；它不等于 Windows 的整树 commit 限额。

迭代时可显式选择一部分矩阵；结果会记录未选输入及未执行单元：

```powershell
python probes/gates/language-composition/suite.py run --suite .cache/grammar-corpus/suite.json --compiler $compiler --kind positive --layouts file --optimizations 0 --cgus default
```

`results.json` 区分生成、选择、执行、通过、失败和漏跑，保存每次编译/运行的命令、
源码、日志、编译器及配套库哈希、资源读数。某批编译失败时不会把该批全部组合
写成已经运行。默认继续收集失败；`--stop-on-failure` 会明确留下未执行单元。
结果的 `pass` 仅指所选矩阵和输入通过，不能解释成整个生成集已经通过。
`--family numeric` / `--family types` 可分别选择表达式规则树或类型树。

## 重放和缩减

从 `results.json` 取 case ID，原 recipe 重放：

```powershell
python probes/gates/language-composition/suite.py replay --suite .cache/grammar-corpus/suite.json --compiler $compiler --ids <case-id> --layouts modules --optimizations 2 --cgus 4
```

把失败单元目录内的 `result.json` 交给缩减器：

```powershell
python probes/gates/language-composition/suite.py reduce --suite .cache/grammar-corpus/suite.json --compiler $compiler --result <failed-cell>/result.json --max-attempts 30
```

缩减器先重现原失败，再削减批次成员、规则层、常量和绑定形式。每次重新计算
预期值，保留同一退出码和诊断族/失败类型，保存全部尝试及 `minimal/` 源码。
预算耗尽会注明，不能称为已经获得最小程序。资源限额失败不参与语义缩减。
已有生成目录不覆盖；生成器版本或源码被改过时拒绝静默重放，需显式重新生成。

## 扩展规则

在 `model.py` 注册规则的输入/输出类型、涉及特性和前置条件，再分别实现解释器
语义和 Vyx 发射。只有所需的辅助声明进入生成程序，避免未使用的声明污染诊断。
未知规则直接报错。新规则进入全部有序组合，不允许给已知编译器缺陷添加跳过项。
新增更宽的类型域、生命周期流或生态协议时，应增设真实语义模型，不能靠返回一个
固定整数宣称那项能力已覆盖。

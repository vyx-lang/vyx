# IDA O3 reverse-engineering comparison

该测试把同一套 token 校验算法分别写成 Vyx、C++ 和 Rust，并为每种语言同时生成
有符号和无符号的 Windows x86_64、O3 PE 文件：

| 语言 | 有符号（CodeView + PDB） | 无符号 |
|---|---|---|
| Vyx MIR2LLVM | `out/vyx.symbols.exe` + `out/vyx.symbols.pdb` | `out/vyx.exe` |
| Clang C++20 | `out/cpp.symbols.exe` + `out/cpp.symbols.pdb` | `out/cpp.exe` |
| rustc | `out/rust.symbols.exe` + `out/rust.symbols.pdb` | `out/rust.exe` |

Vyx 样本静态链接规范 `vyx_runtime`，运行时不需要额外 DLL。

## 构建

在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  .\benchmarks\ida_reverse_comparison\build.ps1
```

脚本使用打包在 `dist/vyx-windows-x86_64-llvm22/` 下的 Vyx 编译器，以及
PATH 中的 `clang++`、`rustc` 和 LLVM 工具。精确版本和公共约束写入
`out/toolchains.txt`，SHA-256 写入 `out/SHA256SUMS.txt`。

单独复验：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  .\benchmarks\ida_reverse_comparison\verify.ps1
```

## IDA 盲测方法

1. 只打开 `out/`，先不要看 `src/` 和 `verify.ps1`。
2. 第一轮打开不带 `.symbols` 的三个文件，分别新建 IDA 数据库并保持相同分析选项。
3. 从 `INVALID`、`ACCEPT `、`REJECT ` 三个字符串的交叉引用定位 CLI 主链。
4. 恢复 64 字符输入格式、四个算法内核及接受条件。
5. 记录定位时间、手工重命名数、伪代码可读性和恢复出的控制流。
6. 第二轮打开对应的 `.symbols.exe`；将 `.pdb` 保持在同一目录，让 IDA 自动加载，
   对照函数名、源码行和局部变量检查第一轮结果，最后再看三份源码。

不要用文件大小直接判断哪种语言更难逆向：Rust 和 Vyx 的启动/运行时边界与
C++ 不同。应比较从入口定位到四个等价算法内核之后的代码，而不是启动代码总量。

## 公平性边界

- 三者使用相同算法、常量、64 位 wrapping 算术、输入和可观察输出。
- 三者均为 O3，无 LTO、PGO、混淆或语言专属保护。
- 四个主要内核均禁止内联，避免 O3 把整个实验折叠进入口。
- 无符号版以 `debuginfo=0`/不带 `-g` 独立构建，再统一执行
  `llvm-strip --strip-all`；验证会拒绝仍含 CodeView/PDB 引用的文件。
- 有符号版使用相同 O3 参数，附带 CodeView 和独立 PDB；验证要求每个 PDB 都包含
  四个主要内核的函数记录。
- C++ 关闭异常和 RTTI；Rust 使用 `panic=abort`。
- `verify.ps1` 覆盖 4 个合法输入和 2 个非法输入，要求全部 6 个 PE 的 stdout 与
  退出码逐项一致。

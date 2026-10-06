# 借用 Vec 与 String 移动回归

使用本次源码构建的 SDK 编译器：

```powershell
.\probes\gates\borrowed-vec-string\run.ps1 -Compiler .\bootstrap_compiler\out\vyxc.exe
```

原生 AOT 项目 `tests/projects/borrowed_vec_ownership` 在两个模块之间传递
`&Vec<i32>`、`&Vec<i64>`、`&Vec<f64>` 和 `&mut Vec<i32>`，检查空容器、
重复遍历、直接借用表达式、遍历后的原容器读取及追加。
String 路径重复移动内联与堆缓冲区，检查 clone、借用、重新初始化、传参和返回。
文本转换另检查 `str -> String` 后原视图可读，`String -> str` 后原缓冲可读，
以及声明、赋值和参数转换不会被误记为所有权转移。
默认覆盖 `-O0/-O2` 与 `VYX_CODEGEN_UNITS=1/4`，验证退出码及完整输出。

负例要求 SDK 在原文件位置报告 `E3100`，覆盖 var/let 初始化、赋值、
按值传参、移动后借用、含 String 的聚合对象、类型别名、条件分支和循环中的转移。
基础 `str` / `string` 的 Copy 规则保持不变。
编译器身份、输出和诊断保存在本目录的 `.runs/`。

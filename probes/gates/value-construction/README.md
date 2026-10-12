# 值构造

使用当前源码构建的 SDK 编译器及配套后端、运行时：

```powershell
python probes/gates/value-construction/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

原生 AOT O0 / O2 覆盖 `Type.new(...)`、`construct::<T>(...)`、泛型参数包、
显式语言构造函数、静态工厂、`Box` 的构造转发及 `dyn` 类型。
`str` 转 `String` 必须包含实际转换函数体，并正确处理内联与堆字符串及移动清理。

没有匹配构造函数的普通记录不能接受字符串、其他名义类型或额外参数；
两个模块中的同名记录也必须保持独立，类型别名则保留原类型身份。
负例必须给出构造位置及类型诊断，且不产出可执行文件。

编译器哈希、调用命令和结果保存在本地 `.cache/`。

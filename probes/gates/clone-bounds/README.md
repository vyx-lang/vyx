# 容器读取与泛型方法的 Clone 约束

使用当前源码构建的 SDK 编译器和配套后端、运行时：

```powershell
$env:LLVM_ROOT = (Resolve-Path ./clang).Path
python probes/gates/clone-bounds/run.py --compiler bootstrap_compiler/out/vyxc.exe
python probes/gates/full-expression/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

原生 AOT 下分别检查 `-O0`、`-O2`：

- `Box.get`、`Ref.get`、`Vec.get/get_unchecked` 以及独立拥有载荷的克隆操作要求载荷实现 `Clone`。
- 嵌套容器递归检查载荷约束；泛型函数和 lazy 方法在具体实例中也执行检查。
- 方法同时含有类参数和方法参数时，约束绑定对应的实参。
- 泛型聚合字面量先替换 `Box<T>` 等嵌套字段类型；不匹配的显式类型实参仍报 `E1000`。
- 非泛型类型的结构化 `Clone` 成员也实际执行；其他 trait 使用同一约束检查。
- 缺失实现时报告调用处的 `E1100`，不进入 ABI 失败路径，也不留下可执行产物。
- 不可克隆对象仍能存入容器、借用、移动和析构。`Ref.clone` 共享载荷，不要求载荷可克隆。
- `Box`、`Vec`、`Dict`、`Stack`、`Queue` 保存共享句柄时，销毁槽位不额外增加引用计数；最后一个句柄释放载荷。
- 显式记录和枚举的 `Clone` 实现实际执行一次，整数读取及嵌套 `Vec` 克隆保持原值。

生成用例、命令、诊断、运行结果与编译器 SHA-256 保存在本地 `.cache/verification.json`。
这些用例覆盖上述操作，不代表所有泛型谓词和生命周期组合已经验证。

# JSON 包源与真实构建回归

包管理器使用 `tools/packages/` 中同一个解析器处理源码、预编译库和 DCI 包。
清单和使用说明见 [项目清单](../../../docs/PACKAGE_MANIFEST_ZH.md)。

## 验证

需要 Python 3.11 或更新版本。网络测试只连接临时本地 HTTP 服务，不访问公共源。

```powershell
python probes/gates/package-sources/test_sources.py
python probes/gates/package-sources/run.py `
    --compiler bootstrap_compiler/out/vyxc.exe `
    --cxx clang/bin/clang++.exe --ar clang/bin/llvm-ar.exe `
    --triplet x86_64-pc-windows-msvc --abi msvc
```

Linux 可使用同样的入口，显式指定本次构建的 SDK 编译器、Clang、llvm-ar 和
`x86_64-unknown-linux-gnu` 或 `aarch64-unknown-linux-gnu`。

`test_sources.py` 覆盖源选择、版本约束、目标与传递依赖、来源隔离、私有认证、
锁与缓存完整性、离线、失败原子性和归档攻击。`run.py` 创建实际消费工程：

- 从 HTTP 源下载 Vyx 源码包，构建传递的静态库并运行消费程序。
- 下载预编译原生库与 Vyx 定义，直接链接而不重编生产端。
- 下载 SDK 编译器产出的 Vyx 静态库与 `.vyi` 接口，不分发实现源码。
- 下载共享库并按声明部署运行库；删除已部署文件后，热缓存离线构建须恢复它。
- 将 Adapter 实测的 `.dcib`、Converter 定义和 C++ 原生库打包，下载后调用
  C++ 原生符号；生产端不添加 C ABI 包装。
- 每个工程再执行 `build --run=aot --offline --locked`，检查没有源请求。
- 两个包携带同名、不同内容的运行库时，构建须给出文件冲突诊断。

门使用传入的编译器，不自动选择旧 SDK 或 seed。成功时清理临时包、归档和工程；
失败时将工程复制到本地 `.cache/failed-aot/` 供排查。
一次运行的输出可重定向到本地 `.cache/`。通过 Windows 门不能证明 Linux、Android
或跨平台产物已经运行验证。

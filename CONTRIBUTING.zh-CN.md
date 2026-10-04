# 贡献 Vyx

[English](CONTRIBUTING.md) · [文档目录](docs/README.md)

Vyx 的贡献以可复现的短闭环为准：使用兼容 Release SDK 编译器作为 Stage 0 构建本
checkout，再运行能证明改动的最小测试。冻结的 C++ host 与旧 `out/` 二进制不是
当前验收路径。

## 贡献许可

除非你明确另作声明，有意提交并纳入 Vyx 的贡献采用与项目相同的 `MIT OR Apache-2.0`
许可，不附加其他条件。请保留已有第三方许可证与版权声明。完整条款见
[MIT](LICENSE-MIT) 和 [Apache-2.0](LICENSE-APACHE)。

## 开始前

先阅读按编译器源码重写的 [语言设计](docs/设计文档_ZH.md) 和
[测试指南](docs/TESTING_GUIDE_ZH.md)，再用 Release SDK 编译器构建当前工作树：

```powershell
$env:VYX_BOOTSTRAP_VYXC = (Get-Command vyxc -ErrorAction Stop).Source
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
$compilerTarget = [regex]::Match((Get-Content .\bootstrap_compiler\Vyx.toml -Raw), '(?m)^name\s*=\s*"([^"]+)"').Groups[1].Value

Push-Location .\bootstrap_compiler
& $env:VYX_BOOTSTRAP_VYXC build --target $compilerTarget -j4
if ($LASTEXITCODE -ne 0) { throw 'bootstrap build failed' }
Copy-Item -LiteralPath ".\out\$compilerTarget.exe" -Destination .\out\vyxc.exe -Force
Pop-Location

$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path
$runtime = (Resolve-Path .\bootstrap_compiler\out).Path
```

## 验证最小相关契约

```powershell
# 语言 module 或 manifest project 改动。
powershell -File .\tests\run_all_modules.ps1 `
  -BootstrapOnly -BootstrapCompiler $compiler -RuntimeDir $runtime -RuntimeLib vyx_runtime `
  -PathFilter '<suite-or-file>'

# 编译器、运行时或 ABI 专项契约。
powershell -File .\tests\checks\run_checks.ps1 -ListOnly
powershell -File .\tests\checks\run_checks.ps1 `
  -Check '<canonical-check-name>' -BootstrapCompiler $compiler
```

语言可见的 compiler/runtime/ABI 改动应增加源码 case；跨越 build、ABI 或多代边界
时，使用 project fixture 或 named check。历史测试数量不能替代当前
compiler/source 组合实际生成的日志。

用户可见的改动须在同一次提交里更新 `docs/` 下对应活文档（测试指南、清单、
标准库、[语言表面](docs/语言表面_ZH.md)或 README）。新的语言表面必须进教程课
**并且**进语言表面对照表，不要让用户去读编译器源码。

## 提交时附带

- 改动内容及用户可见的结果；
- Stage 0 compiler 的 hash、版本和路径；
- source commit 与实际运行的命令；
- 聚焦测试输出，以及需要时的 IR/project 日志；
- 不兼容行为或已知失败。

## 不提交生成物

不要提交 `build/`、`bootstrap_compiler/out/`、`.cache/`、`target/`、
`benchmarks/out/`、`.ll`、`.obj`、`.exe`、`.dll`、`.pdb` 与临时调试日志。提交
源码、manifest、可维护的 case 和长期脚本即可。

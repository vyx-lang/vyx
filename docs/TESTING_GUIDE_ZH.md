# Vyx 构建与验证指南

[English: Testing guide](TESTING_GUIDE.md) · [文档目录](README.md)

本文描述当前执行路径。已冻结归档的 C++ host 和已有的 `bootstrap_compiler/out/`
不能作为当前源码树的依据。显式选择兼容的 Stage 0，并记录其身份。
当前验收以原生 AOT 为基准，JIT 的能力对齐与性能属于后续任务。

## 设置 Stage 0 编译器并构建

从 [Releases latest](https://github.com/vyx-lang/vyx/releases/latest) 下载兼容的 SDK，
将 `VYX_BOOTSTRAP_VYXC` 指向其中的 `vyxc`，或显式指定兼容的仓库 seed。
`dist/` 是被 Git 忽略的本地打包目录。

```powershell
$env:VYX_BOOTSTRAP_VYXC = (Get-Command vyxc -ErrorAction Stop).Source
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
if (-not (Test-Path $env:VYX_BOOTSTRAP_VYXC)) { throw 'missing Stage 0 compiler' }
$compilerTarget = [regex]::Match((Get-Content .\bootstrap_compiler\Vyx.toml -Raw), '(?m)^name\s*=\s*"([^"]+)"').Groups[1].Value

Push-Location .\bootstrap_compiler
& $env:VYX_BOOTSTRAP_VYXC build --target $compilerTarget -j10
if ($LASTEXITCODE -ne 0) { throw 'bootstrap build failed' }
Copy-Item -LiteralPath ".\out\$compilerTarget.exe" -Destination .\out\vyxc.exe -Force
Pop-Location

$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path
$runtime = (Resolve-Path .\bootstrap_compiler\out).Path
```

预检（针对仓库内 Windows SDK 布局）：

```powershell
foreach ($tool in @('clang.exe', 'clang++.exe', 'llvm-ar.exe')) {
  $path = Join-Path (Join-Path (Resolve-Path .\clang).Path 'bin') $tool
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "仓库 LLVM SDK 缺少 $tool`: $path"
  }
}
```

不要把已有的 `out/vyxc.exe` 当作 Stage 0 编译器。宿主无法运行 Windows 编译器时，
必须通过 `-SeedCompiler` 显式指定兼容 Release compiler 并记录
其 hash；脚本不会静默切换工具链。

## 后端策略

自举 manifest 生成私有 `vyx_compiler_backend`、静态 `vyx_runtime`、SDK 编译器入口 `vyxc`
及自举工具。LLVM 只属于编译器依赖；普通程序静态链接 `vyx_runtime`，不加载 LLVM
或 Vyx 运行时 DLL。
编译器目标从清单的 `[package].name` 读取；复制步骤确保 SDK 入口在自动别名部署
被跳过时也能更新。被测 SDK 编译器必须配合同次构建的 backend/runtime，不能混用
Release SDK 或其他阶段的 `out/`。

重建后用规范脚本打包 SDK：

```powershell
.\bootstrap_compiler\scripts\package_sdk.ps1 -Archive
```

Linux 打包时追加 `-BundleLlvm`，会内置仅供编译器使用的精简 LLVM 22 工具链；
这不会改变应用契约，普通应用仍只包含自身代码和静态链接的 Vyx 运行时。

MIR2LLVM 是受支持的自举后端。MIR2CPP/`--emit=cpp` 仍为实验性、不稳定后端，
不作为 Release 或固定点验收门禁。两个输出路径都使用 `--triplet=<triple>` 选择
目标系统；指定目标系统不会改变后端的稳定性等级。
本指南的回归使用 AOT，JIT 运行结果不能代替相同验收。

项目集里 MIR2CPP 专属的用例因此默认自跳过（退出码 77，计为"跳过"），
需要手动验证该后端时设 `VYX_ACCEPT_MIR2CPP=1` 强制运行。

## CLI 与诊断

```powershell
# 打印包版本 1.0.0-alpha.1（ea）与本机构建身份（二进制路径、大小、LLVM）。
& $compiler --version
& $compiler version
```

一次可复现调用应明确指定 source mode 和输出 mode：

```powershell
# 原生可执行文件（默认输出同样是 exe）。
& $compiler --src=file .\path\to\main.vyx --emit=exe -o .\out\main.exe

# 只检查结构化编译器数据，不生成程序。
& $compiler --src=file .\path\to\main.vyx --dump-hir2
& $compiler --src=file .\path\to\main.vyx --dump-mir2

# 对成功编译的输入选择运行模式。
& $compiler --src=file .\path\to\main.vyx --run=aot

# 包命令（cwd 是 Vyx 包/探针夹具，不是编译器树）。
& $compiler test                          # tests/ 下的 @[test]
& $compiler bench                         # benches/ 下的 @[bench]；打印 ms
& $compiler fmt --check .\src\main.vyx    # token 重排；有差异则退出 1
& $compiler doc .\src\main.vyx            # 从 /// 出 markdown
```

支持的 `--emit` 值为 `ir`、`obj`、`exe`、`dll`、`lib`、`vyi`、`cpp` 和
`dci-stubs`。其中 `cpp` 仍受上面的实验后端策略约束；`hir2`/`mir2` 检查是独立
的 debug 操作（`--dump-hir2`、`--dump-mir2`），不是 emit 值。CLI 选项缺值或无效时，
编译器在读入源码前报告 `E0003` 并以非零状态退出。

源码诊断包含稳定 code 和位置，例如
`path/to/file.vyx:3:14: error: E1000: ...`。工具需要机器可读记录时设置
`VYX_DIAG_JSON=1`；流程应以编译器退出状态判定失败。`--verify-hir2` 与
`--verify-mir2` 只检查编译器 IR 不变量，必须和用户程序的编译/运行 contract 叠加，
不能单独代替验收。

## 交叉编译（Windows 宿主）

`--triplet` 选择 LLVM/后端目标。Windows 上可见 NDK 时，Android 是受支持的
交叉链接路径。macOS triple 可用本仓库 LLVM SDK 产出 LLVM IR 与 Mach-O 目标文件；
链接 Darwin 可执行文件仍需要 macOS sysroot，**不是**当前验证门。

```powershell
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
$env:ANDROID_NDK_HOME = $env:ANDROID_NDK_ROOT  # 或 NDK_HOME，或 Android SDK 下最新 NDK
$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path

# 每个 triplet 先编一次 Android runtime（产物：out/runtimes/android/arm64/ndk/）。
Push-Location .\bootstrap_compiler
& $compiler build --target vyx_runtime --triplet aarch64-linux-android23
Pop-Location

# 再用同一 triplet 编项目。
& $compiler build --triplet aarch64-linux-android23
```

NDK `clang++` 默认会拉 `libc++_shared`。Android 上的 Vyx 默认加 `-nostdlib++`，
纯 Vyx 二进制只依赖 Bionic（`libc` / `libm` / `libdl`），不依赖 libc++。
需要 libc++ 时再选择：

- `Vyx.toml` 里 `libs_android = ["c++_shared"]` 或 `["c++_static"]`
- 含 C++ 源或 DCI C++ stubs（自动 `c++_shared`）
- `VYX_ANDROID_LIBCXX=shared` 或 `static`

`VYX_ANDROID_LIBCXX=none`（或 `0` / `off`）即使有 C++/DCI 也不自动加。
Bionic 没有 `libpthread`，Android 链接不加 `-lpthread`。

`@[platform("posix")]` 同时匹配 Linux、Android、macOS。OS 相关 std
（`os` / `fs` / `path` / `sync` / `threading` / `vio`）是 `windows` vs `posix`。
`io_uring` 与 x86_64 `syscallN` 仍仅 Linux。Bionic 没有
`getcontext`/`swapcontext`；Android fiber API 是 no-op stub。

Windows 上不要把 `--src=file` 的输入放在仓库根目录 `out/` 下。

## 按改动运行验证

`tests/run_all_modules.ps1` 会分别统计成功、跳过和失败。只有明确绑定平台的 fixture
（例如 x86_64 MSVC ABI descriptor）才可用退出码 `77` 标记不适用；跳过不会计为
成功。

```powershell
# 模块与 manifest project sweep
powershell -ExecutionPolicy Bypass -File .\tests\run_all_modules.ps1 `
  -BootstrapOnly -BootstrapCompiler $compiler -RuntimeDir $runtime -RuntimeLib vyx_runtime

# 发现并运行专项 contract
powershell -ExecutionPolicy Bypass -File .\tests\checks\run_checks.ps1 -ListOnly
powershell -ExecutionPolicy Bypass -File .\tests\checks\run_checks.ps1 `
  -Check reference_receiver_ergonomics -BootstrapCompiler $compiler

# 冷 S1/S2/S3 project fixed point
powershell -ExecutionPolicy Bypass `
  -File .\bootstrap_compiler\scripts\test_project_selfhost_fixpoint.ps1 `
  -SeedCompiler $env:VYX_BOOTSTRAP_VYXC -LlvmRoot .\clang -Target $compilerTarget -TimeoutSec 1800
```

Linux 使用同一套固定点脚本：

```bash
export LLVM_ROOT=/usr/lib/llvm-22
export PATH="$LLVM_ROOT/bin:$PATH"
export VYX_BOOTSTRAP_VYXC=$(command -v vyxc)
compiler_target=$(awk -F '"' '/^name[ \t]*=/{print $2; exit}' bootstrap_compiler/Vyx.toml)
pwsh -NoProfile \
  -File bootstrap_compiler/scripts/test_project_selfhost_fixpoint.ps1 \
  -SeedCompiler "$VYX_BOOTSTRAP_VYXC" \
  -LlvmRoot "$LLVM_ROOT" \
  -Target "$compiler_target" \
  -TimeoutSec 1800
```

脚本会让每代 compiler 优先加载同代 runtime，避免机器上旧的
`LD_LIBRARY_PATH` 覆盖 `$ORIGIN`。

`-Target $compilerTarget` 选择编译器固定点门，不额外构建 LSP/DAP。脚本默认使用 3 个隔离
阶段与 10 个构建任务；报告应记录设置、各代编译器 fingerprint 与比较结果。

## AOT 编译器专项门

### 门 A：精确 hello MIR

hello 必须包含 `print("hello")`。Windows 上将临时源放在编译器项目目录，
并在该目录运行：

```powershell
$expected = 'mir2.unit functions=1 blocks=1 locals=1 places=1 values=6 instrs=2 cases=0 types=5'
$helloGateCreated = $false
Push-Location ./bootstrap_compiler
try {
    if (Test-Path ./hello-gate-doc.vyx) { throw 'hello-gate-doc.vyx already exists' }
    Copy-Item ../probes/gates/hello_gate.vyx ./hello-gate-doc.vyx
    $helloGateCreated = $true
    $summary = & $compiler --src=file ./hello-gate-doc.vyx --dump-mir2 2>$null |
        Select-String '^mir2.unit'
    if ($LASTEXITCODE -ne 0 -or $summary.Line -ne $expected) { throw 'hello MIR gate failed' }
    $summary.Line
} finally {
    if ($helloGateCreated) { Remove-Item ./hello-gate-doc.vyx -ErrorAction SilentlyContinue }
    Pop-Location
}
```

比较完整一行，尤其不能让 `values=6 instrs=2` 在没有解释的情况下漂移。
门 A 必须与可执行回归及门 B 自举固定点一起使用。

### 语言、接口和构建调度

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/match-expression/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/generic-direct-return.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/extended.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/generic_interfaces/long-chains.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/compiler-industrial/run.ps1 -Compiler $compiler -Scale 8 -Jobs 4
```

分别覆盖 [match 值与负向诊断](../probes/gates/match-expression/README.md)、
[跨模块 global/enum 与泛型返回](../probes/gates/cross-module/README.md)、
[模板 artifact 校验和双消费者链接](../probes/gates/generic_interfaces/README.md)、
[冷/暖/增量构建](../probes/gates/compiler-industrial/README.md)。这些入口不会由
`tests/run_all_modules.ps1` 或 `tests/checks/run_checks.ps1` 自动调用。
8 模块 / 4 jobs 是已记录的 Windows smoke 规模；大工程性能结论还需要更大规模、
重复采样、内存配置与真实工程。任务并发不代表共享一次前端或并发 LLVM lowering。

### 门 C：DCI 端到端

修改 DCI、class ABI 或 lowerer 后，必须用被测 SDK 编译器编译并运行对应 project，
再执行受影响的 `probes/gates/dci-*` 契约：

```powershell
foreach ($name in @('dci_cpp_trait', 'dci_rust_trait', 'dci_multilang', 'dci_zig_abi')) {
    powershell -NoProfile -ExecutionPolicy Bypass -File "./tests/projects/$name/run.ps1" `
        -BootstrapCompiler $compiler -RuntimeDir $runtime
    if ($LASTEXITCODE -eq 77) { Write-Host "$name skipped on this platform" }
    elseif ($LASTEXITCODE -ne 0) { throw "$name failed" }
}
```

这四个脚本当前要求 Windows 与对应 native toolchain，退出码 77 不算通过。
按改动追加 [DCI 存储](../probes/gates/dci-storage/README.md)、
[异常](../probes/gates/dci-exceptions/README.md)、
[原始 C++ vector](../probes/gates/dci-vector/README.md)、
[Rust 生态](../probes/gates/dci-rust-ecosystem/README.md)与
[C++ 生态](../probes/gates/dci-cpp-ecosystem/README.md)门。
有 Qt SDK 时另跑 [Qt Widgets counter](../probes/gates/dci-qt-counter/README.md)：
它不包含在 `tests/projects/dci_*` 扫描中，验证多层指针契约、真实事件循环、
Qt 到 Vyx 的虚方法派发、按钮与 LCD 状态及退出清理。
旧 `dci_spdlog/run.sh` 与 `probes/gates/gate-c/` 已于 2026-09-28 退役，不能继续
列作当前验收命令。`--verify-mir2` 或单纯 Vyx FFI 程序不能代替 DCI 端到端执行。

### 2026-10-01 已记录结果

最终 artifact 后续修复（`61d31e51`）以 `build --target $compilerTarget -j4 -O0` 连续进行
两次本树自举，S2/S3 逐字节相同，SHA-256 为
`13BBC891263F818FD25E9D10AC4CA92668EA863115B5A2DD98B876E4D66F06EF`；
门 A 保持上方完整输出。日志保存在 `bootstrap_compiler/.runs/artifact-fixpoint/`。
这是特定源码、选项的 Windows 结果，不表示隔离冷构建脚本、Linux、其他优化模式
或 JIT 使用同一 binary 完成了相同验收。生成的日志与产物不提交到源码树。

## 分发包

Windows 与 Linux 固定点包分别输出为：

- `dist/vyx-sdk-windows-x86_64-llvm22.zip`
- `dist/vyx-sdk-linux-x86_64-llvm22.tar.gz`

验收时应解压 archive 后运行其中的 `bin/vyxc --help`，不能只检查 staging 目录。
Windows 和 Linux SDK 均内置 LLVM 后端、runtime、标准库与 DCI 工具，普通 SDK
使用者无需另装 LLVM。从源码重建 LLVM 桥接需要 LLVM 22 开发工具、库及上方
说明的宿主构建环境。

每次全量结论都应记录 Stage 0 的 hash/版本、source commit、实际命令和日志。host 时代的
IR snapshot corpus 仅用于迁移研究，未迁移前不能当作当前 bootstrap 通过门；benchmark
也应传入刚构建的 `$compiler` 并使用 `-SkipHost`。完整英文说明见
[TESTING_GUIDE.md](TESTING_GUIDE.md)。

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
& $env:VYX_BOOTSTRAP_VYXC build --target $compilerTarget -j20
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
& $compiler test                          # src/ 与 tests/ 下的 @[test]
& $compiler bench                         # benches/ 下的 @[bench]；校准后统计 ns/op
& $compiler fmt --check .\src\main.vyx    # token 重排；有差异则退出 1
& $compiler doc .\src\main.vyx            # 从 /// 出 markdown
```

支持的 `--emit` 值为 `ir`、`obj`、`exe`、`dll`、`lib`、`vyi`、`cpp`、`dcib` 和
`dci-stubs`。`dcib` 原生导出 `@[dci_export]` 选中的函数契约，验证与支持范围见
[原生导出门](../probes/gates/dci-native-export/README.md)。其中 `cpp` 仍受上面的实验后端策略约束；`hir2`/`mir2` 检查是独立
的 debug 操作（`--dump-hir2`、`--dump-mir2`），不是 emit 值。CLI 选项缺值或无效时，
编译器在读入源码前报告 `E0003` 并以非零状态退出。

源码诊断包含稳定 code 和位置，例如
`path/to/file.vyx:3:14: error: E1000: ...`。工具需要机器可读记录时设置
`VYX_DIAG_JSON=1`；流程应以编译器退出状态判定失败。`--verify-hir2` 与
`--verify-mir2` 只检查编译器 IR 不变量，必须和用户程序的编译/运行 contract 叠加，
不能单独代替验收。

## 交叉编译（Windows 宿主）

在 Android/Termux 上运行 `vyxc` 时，Android 是宿主平台。编译器直接使用
Termux 提供的 `clang`、`clang++`、`llvm-ar` 以及它们的默认 sysroot；不读取
`ANDROID_NDK_ROOT`、`ANDROID_NDK_HOME` 或 Android SDK 下的 NDK，也不要求
额外安装 NDK。安装包的启动脚本会设置 `LLVM_ROOT` / `LLVM_BIN`；自行部署时
也可以让这些变量或 `PATH` 指向 Termux 的 LLVM。此路径使用普通 SDK runtime
搜索，不会生成 `runtimes/android/.../ndk` 目录。

Termux 包通过私有 SONAME 和 ELF `RUNPATH` 加载 SDK 私有 LLVM，不把这些目录导出到
`LD_LIBRARY_PATH`。否则系统 `clang` 会继承 SDK 库路径，可能加载不同版本的
`libLLVM.so` 并在链接前报符号缺失。打包时检查 `verneed` 与依赖库 SONAME
和私有版本定义的关联，保留普通运行时库的应用链接名称。
启动脚本使用 Termux 的固定 prefix；
重新打包和 ELF 环境回归见 [脚本说明](../bootstrap_compiler/scripts/README.md#termux-packaging)。

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

语法组合使用[生成门](../probes/gates/language-composition/README.md)：
按规则、深度和常量域穷举有序组合，以独立解释器检查运行结果和清理次数。
`suite.py plan` 查看规模，`generate` 生成语料，`run` 执行，`replay` / `reduce`
重放与缩减失败。报告分别记录生成、执行和漏跑；未建模特性明确列出，不能用
有限深度通过宣称任意程序正确。该门与真实项目、DCI、自举门共同使用。

`tests/run_all_modules.ps1` 会分别统计成功、跳过和失败。只有明确绑定平台的 fixture
（例如 x86_64 MSVC ABI descriptor）才可用退出码 `77` 标记不适用；跳过不会计为
成功。

每次运行在 `tests/.cache/` 下使用独立目录存放模块产物、可执行文件、运行时 DLL
和进程日志。并发分片分别部署配套运行时。默认日志名包含进程 ID 和运行标识；
显式传入 `-LogPath` 时，请为各分片指定不同路径。

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
$helloGateCreated = $false
Push-Location ./bootstrap_compiler
try {
    if (Test-Path ./hello-gate-doc.vyx) { throw 'hello-gate-doc.vyx already exists' }
    Copy-Item ../probes/gates/hello_gate.vyx ./hello-gate-doc.vyx
    $helloGateCreated = $true
    $dump = & $compiler --src=file ./hello-gate-doc.vyx --dump-mir2 | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'hello MIR gate failed' }
    $main = [regex]::Match($dump, '(?m)^  fn #(?<id>\d+) .*name=main .*block_count=1 ')
    if (!$main.Success) { throw 'missing one-block main' }
    foreach ($entry in @(@('block',1),@('local',1),@('place',1),@('value',6),@('instr',2))) {
        $count = [regex]::Matches($dump, '(?m)^  '+$entry[0]+' #\d+ fn=#'+$main.Groups['id'].Value+' ').Count
        if ($count -ne $entry[1]) { throw "hello main $($entry[0]) count drifted" }
    }
    [regex]::Match($dump, '(?m)^mir2\.unit .*$').Value
} finally {
    if ($helloGateCreated) { Remove-Item ./hello-gate-doc.vyx -ErrorAction SilentlyContinue }
    Pop-Location
}
```

`print` 现在调用标准库，因此完整 unit 包含库声明与可达函数体。历史 unit 计数
为 `functions=1 ... values=6 instrs=2`；main 仍应是 1 block、1 local、1 place、
6 values、2 instrs。保存完整 unit 计数并解释漂移。固定点脚本的 `-HelloGate`
还检查 print 目标为有函数体的普通库函数。
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
python probes/gates/platform-interfaces/run.py --compiler $compiler --out .cache/platform-interfaces
```

分别覆盖 [match 值与负向诊断](../probes/gates/match-expression/README.md)、
[跨模块 global/enum 与泛型返回](../probes/gates/cross-module/README.md)、
[模板 artifact 校验和双消费者链接](../probes/gates/generic_interfaces/README.md)、
[冷/暖/增量构建](../probes/gates/compiler-industrial/README.md)。这些入口不会由
`tests/run_all_modules.ps1` 或 `tests/checks/run_checks.ps1` 自动调用。
平台接口门还会把 `.vyi` 的 `@[platform]`、嵌套 `extern "C"`、重复平台条件和
外部返回存储契约带到 Windows、Linux、Android 与两个 macOS 目标，确认目标专属
运行时声明不会泄漏到其他平台；详见 [平台接口门](../probes/gates/platform-interfaces/README.md)。
8 模块 / 4 jobs 是已记录的 Windows smoke 规模；大工程性能结论还需要更大规模、
重复采样、内存配置与真实工程。任务并发不代表共享一次前端或并发 LLVM lowering。

### AST 存储与基础类型字符串方法

```powershell
python probes/gates/ast-storage/run.py --compiler $compiler
python probes/gates/primitive-string-methods/run.py --compiler $compiler
```

[AST 存储门](../probes/gates/ast-storage/README.md)以 AOT O0/O2 覆盖
`Vec<Ref<T>>`、间接函数调用、继承字段、派生类 trait 实现与编译期调用。
可选的 Windows 内存保护模式使用同次编译器对象重新链接，暴露失效 AST 视图与
记录越界；该调试产物和普通 SDK 编译器的身份分别记录。
[字符串方法门](../probes/gates/primitive-string-methods/README.md)检查基础类型的
两种方法名、返回类型、含 NUL 的文本，以及同名用户方法的派发。

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


## SDK 测试与基准命令

`vyxc test` / `vyxc bench` 从编译器 AST 发现函数，注释、字符串和其他属性参数中的
名字不算测试。跳过 `.cache`、`target`、`out`、`build`、`.git`、`snapshots` 等生成目录。
显式文件或目录替换默认扫描范围，重复绝对路径去重。

有 `Vyx.toml` 时，扫描所选 target 的源码及导入闭包，再加入 `tests/` 或 `benches/`。
多 target 无默认目标时必须提供 `--target`。测试沿用普通项目构建器的依赖解析、原生
C/C++ 源码、DCI 准备、Effect schema、编译选项、hooks 和链接设置。调度入口追加到
被测文件的逻辑模块中，保留原始源码位置和私有函数访问。原有 `main` 仍可作为普通
函数调用，不会替代测试结果；测试构建也不会覆盖应用的输出别名。

没有清单时，扫描 `src/` 与 `tests/` 或 `benches/`，按独立文件编译。在项目内使用
`--standalone` 可明确选择此模式；它不继承项目依赖与链接配置。

### 测试

```vyx
@[test]
fn arithmetic() { assert(6 * 7 == 42); }

@[test]
fn status_test() -> i32 { return 0; }

@[ignore("requires external hardware")]
@[test]
fn device_test() { /* assertions */ }
```

支持有函数体、无参数、无泛型参数的同步自由函数，返回 `void`（可省略）或 `i32`。
正常返回或整数 0 通过；任何非 0 整数都失败，包括 256。断言、崩溃、超时、无法启动
和输出超限均单独计为失败。异步测试、测试方法、模块块内测试会明确拒绝。

```sh
vyxc test --target app -j4
vyxc test --list
vyxc test tests/parser.vyx --filter arithmetic --exact
vyxc test --repeat 10 --timeout-ms 5000 --fail-fast
vyxc test --ignored
vyxc test --include-ignored --nocapture
vyxc test --no-run
vyxc test --report results/test.json --junit results/test.xml
```

筛选对 `绝对路径.vyx::函数名` 作单向子串匹配；`--exact` 匹配完整身份或完整函数名。
`--list` 只解析验证，不编译。空选择默认失败，只有 `--allow-empty` 才接受空结果，
它不会吞掉发现或构建错误。`@[ignore]` 默认列出并计为忽略；`--ignored` 只运行这些
测试，`--include-ignored` 同时运行两组。

每个文件只编译一次，每项测试及每次重复独立启动子进程。`-j N` / `--jobs N` 限制
并发数（1..64，默认 1），同时传给项目构建器；`--build-jobs N` 可独立调整构建并发数（默认 4），基准仍串行测量。文件之间顺序构建，同一文件的测试
可并发运行。运行默认限时 30000 ms，构建默认限时 300000 ms，可分别用
`--timeout-ms` / `--build-timeout-ms` 调整。超时终止自有进程树。每个输出流默认限
16 MiB（`--output-limit-kb N`，1..1048576 KiB），每 10 ms 检查一次，超限会终止进程。失败时显示输出，`--nocapture` 在进程
结束后也显示通过项的输出。工作目录就是执行命令的目录。
显式 `--build-jobs` 的优先级高于 `-j`，不受参数顺序影响。

`--fail-fast` 在首个已观察到的失败后停止发起新任务，已经运行的项正常收尾。
同文件未开始的项记录为 `not_run`，后续文件注明未处理。`--no-run` 只编译，计为
未运行，不能算通过。发现错误、构建错误和运行失败分别统计。退出码：0 接受成功，
1 测试/发现/构建/报告失败，2 参数或输入路径无效。

JSON 报告版本为 `schema_version: 1`，包含完整命令参数、编译器身份、宿主、优化等级、
逐项状态与耗时、stdout/stderr 及日志位置；测试还可输出 JUnit XML。写报告失败会令
命令失败。日志保留在 `.cache/vyxc-cli/<kind>_<pid>_<time>/`，调度源码使用内容寻址
目录。项目构建器负责源码与依赖缓存失效；不会复用先前的测试通过结果。
按模块构建时，不同调度器共享未变化的模块对象，只有加入调度入口的编译单元使用独立
对象身份。清单选择 unity 编译时，入口变化仍需要重编该翻译单元；测试不会暗改项目的
编译策略。

清单上下文每次命令只解析一次；即使省略 `--target`，报告也记录实际选中的目标。
发现过程复用行索引与按完整路径、名称比较的哈希表。JSON 与 JUnit 目的路径必须不同，
且不能覆盖选中的源码、配置的原生源码、`Vyx.toml` 或基线文件。日志文件保留原始字节；
JSON 内的 NUL 不会截断后续内容，无效 UTF-8 显示为 U+FFFD。JUnit 省略 XML 1.0 不允许
的字符。超过 1 MiB 的日志在报告中给出路径，不内嵌全文。

### 基准

```vyx
use std.benchmark;
var input: i64 = 17;

@[bench]
fn arithmetic_benchmark() {
    let x = black_box(input);
    let result = black_box(x * x + 3);
    assert(result == 292);
}
```

`black_box<T>` 通过独立编译的内存屏障暴露输入与结果，避免无可见结果的计算直接
消失；它不保证阻止所有优化。签名要求与测试相同，`i32` 返回值表示成败，不是测量值。
一次函数调用是一轮操作；函数内的准备工作计入耗时。

```sh
vyxc bench --target app --report results/base.json
vyxc bench --filter arithmetic --samples 50 --warmup-ms 1000 --sample-ms 100
vyxc bench --baseline results/base.json --max-regression-percent 5 --report results/current.json
```

默认 O2，可用 `-O0`..`-O3` 覆盖。基准串行执行。子进程先将迭代批量翻倍到指定采样
时长，再预热，最后重复采样单调时钟纳秒值。编译、进程启动、校准和预热不计入样本。
默认 30 个样本、1000 ms 预热、每个样本目标 100 ms；`--samples` 范围 3..10000，
时长参数为 1..3600000 ms。运行超时仍约束整个基准，长采样计划需相应提高限时。
时钟失败、非正数或无效样本会报错。

报告保留每批迭代数与总纳秒，并给出每轮平均值、中位数、最小/最大、p95、样本标准差
和变异系数；不偷偷丢弃离群值，变异系数超过 10% 会提示。基线每次命令只加载一次，
上限 16 MiB；验证格式、宿主平台及架构、实际目标、优化等级和完整基准身份，从原始
样本重算统计。重复 JSON 字段或身份、解码后的 NUL、小数计数或时长、样本数不一致和
数值越界均在执行基准前拒绝。比较结果包含基线 SHA-256；缺少成功的对应项也会失败。

百分比阈值为 `p` 时，令 `s = 1 + p/100`，比较量为
`mean(new) - s*mean(base)`，标准误为 `sqrt(var(new)/n + s*s*var(base)/m)`。
比较量超过保守的 95% t 误差界才判为回退，自由度不大于 `min(n-1,m-1)`。
这是对缩放后基线的[独立均值检验](https://www.itl.nist.gov/div898/handbook/eda/section3/eda353.htm)：
基线均值缩放后，其方差也须乘以 `s*s`。报告保留比较量与误差界，单位均为 ns/iter；
样本排序使用 O(n log n) 时间，支持上限 10000 个样本。区间依赖样本
相对独立的假设，不能消除系统负载、自相关和硬件差异，发布判断应在同机稳定条件下做。

应用测试入口和编译器的语言符合性、诊断、组合生成、DCI 生态及自举门不是同一层次，
后者继续使用 `tests/run_all_modules.ps1` 和 `probes/gates/`。
具体回归见[测试与基准执行器门](../probes/gates/test-runner/README.md)。
当前不提供覆盖率插桩、异步测试执行、文档测试、自动 fixture、参数化发现或跨设备执行。
测试准备和数据组合可用普通辅助函数与循环表达。

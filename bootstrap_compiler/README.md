# Vyx SDK compiler

`bootstrap_compiler/` contains the active frontend, HIR/MIR, LLVM lowering,
project build system, and standard-library packages.

The compiler implements the Fact Semantic Ownership System core features described in
[Fact Semantic Ownership System](../docs/MOSP.md): Migrate, Reflection, DCI, DCE, and Effect.

## Build from source

Use a compatible SDK from this repository's
[latest Release](https://github.com/vyx-lang/vyx/releases/latest)
as Stage 0. Install LLVM 22 for the host, including the tools and libraries needed
to build the compiler backend. On Windows, provide the MSVC / Windows SDK build environment.

The HIR, MIR and LLVM source layout requires a Stage 0 that publishes public inherent `impl`
methods in `.vyi` and collects export roots from every file in a logical module.
Early SDKs with the same `1.0.0-alpha.1` version label may lack these fixes; record
the compiler hash and check the [module contract](../probes/gates/compiler-modules/README.md)
when selecting a bootstrap compiler. An old compiler's successful build of the
unsplit source does not validate the split source tree.

With the Release SDK's `bin` on `PATH`, run from the repository root in PowerShell:

```powershell
$env:VYX_BOOTSTRAP_VYXC = (Get-Command vyxc).Source
# If using a repository-local LLVM SDK:
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
$env:PATH = "$env:LLVM_ROOT\bin;" + $env:PATH
$compilerTarget = [regex]::Match((Get-Content .\bootstrap_compiler\Vyx.toml -Raw), '(?m)^name\s*=\s*"([^"]+)"').Groups[1].Value

Push-Location .\bootstrap_compiler
& $env:VYX_BOOTSTRAP_VYXC build --target $compilerTarget -j10
if ($LASTEXITCODE -ne 0) { throw "bootstrap build failed" }
Copy-Item -LiteralPath ".\out\$compilerTarget.exe" -Destination .\out\vyxc.exe -Force
Pop-Location
```

For another LLVM installation, set `LLVM_ROOT` to its actual path.
The build produces `out/vyxc.exe` on Windows or `out/vyxc` on Linux,
plus backend and runtime artifacts. The example reads the compiler target from
the manifest and copies its output to the SDK entry point. Use that freshly
built SDK compiler with its matching backend/runtime to validate the checkout.

See the [testing guide](../docs/TESTING_GUIDE.md) for Linux commands,
fixed-point builds, runtime paths, and validation.

## Backend

Project commands use one manifest builder:

```sh
vyxc --run=aot --src=project . --target app
vyxc --emit=ir --src=project . --target app
vyxc --emit=dcib --src=project . --target library
vyxc build --target app
```

`build` selects the current project. Both forms share target selection, dependencies,
DCI preparation, hooks, caches and compilation options. IR output stops before
executable linking and publishes `<output_dir>/<target>.ll`; `-o` overrides this path.
`--src=file` uses the single-file driver.

`--emit=dcib` emits a native binary DCI contract for explicitly selected
`@[dci_export]` functions. Project output is `<output_dir>/<target>.dcib`,
and `-o` overrides it. It does not link or run an executable. ABI evidence
comes from the current LLVM module; unsupported signatures or unproven unwind
behavior are diagnosed. See the [native export gate](../probes/gates/dci-native-export/README.md)
for the supported surface and a separate AOT consumer.

MIR2LLVM is the supported native and self-hosting backend.
`--emit=cpp` is experimental and is not a release or fixed-point gate.
Native AOT is the current development and regression baseline. JIT feature
parity and performance are deferred; AOT results do not certify the JIT path.
Target selection and cross-compilation are documented in the
[testing guide](../docs/TESTING_GUIDE.md#cross-compilation-windows-host).

## Current regression contracts

The 2026-10-01 changes (`ef86b928`, `61d31e51`) add versioned, SHA-256 checked
user-template AST artifacts in `.vyi`, preserve defining-module scope and
private helpers, support value `match`, and stabilize cross-module enum/global
identity. Static string initializer metadata no longer overlaps module metadata.
Generic ODR definitions use COMDAT on COFF/ELF/Wasm; duplicate-consumer linking
has been tested on Windows COFF.

Focused AOT gates live under `../probes/gates/`:

- [Generic interfaces](../probes/gates/generic_interfaces/README.md): artifact
  rejection, private access, free generics, independent consumers, and long lists.
- [Cross-module definitions](../probes/gates/cross-module/README.md): globals,
  string initializers, enums, and direct generic return typing.
- [Match expressions](../probes/gates/match-expression/README.md): executable
  value/control-flow checks and negative diagnostics.
- [Compiler usability](../probes/gates/compiler-usability/README.md): derived
  aggregates with trait implementations, index printing, interpolation syntax,
  and async frames and aggregate calls in native streaming codegen.
- [Compiler pressure](../probes/gates/compiler-industrial/README.md): deterministic
  project generation and cold/warm/incremental `-j1`/`-jN` runs.
- [Compiler module contracts](../probes/gates/compiler-modules/README.md): public
  inherent implementation interfaces, multi-file module roots and helper visibility.

The final Windows AOT artifact follow-up passed self-host S2/S3 byte identity at
`build --target $compilerTarget -j4 -O0`, SHA-256
`13BBC891263F818FD25E9D10AC4CA92668EA863115B5A2DD98B876E4D66F06EF`.
This is a dated result for those sources and options; each compiler change needs
fresh gates. It does not establish shared frontend analysis, concurrent LLVM
lowering, or the roadmap's large-project performance targets.

## Source layout

The [compiler architecture](../docs/COMPILER.md) / [中文架构说明](../docs/COMPILER_ZH.md)
connects the HIR, MIR, LLVM, and CGU stages below.
The [source tree map](src/README.md) lists subsystem boundaries and entry points.

| Path | Contents |
|---|---|
| `src/core/` | Driver, parsing, semantic analysis, metadata, project builds |
| `src/hir/` | Typed high-level representation and resolution |
| [`src/hir/builder/`](src/hir/builder/README.md) | HIR construction organized by declarations, instances, expression resolution and body lifetime |
| [`src/mir/`](src/mir/README.md) | Separate model, build, analysis, passes, verification, backend and debug subtrees |
| `src/codegen/` | LLVM lowering and native ABI handling |
| [`src/codegen/llvm/`](src/codegen/llvm/README.md) | LLVM implementations organized by operation, ABI and streaming responsibility |
| `std_packages/` | Canonical self-hosted standard-library packages |
| `std/` | Compatibility modules used by older compiler stages |
| `scripts/` | Build, packaging, fixed-point and probe tools |
| `../tools/dci/` | DCI Adapters, contracts, and Active Adapter tooling |
| `../tests/` | Language cases and project tests |

## SDK packages

- [Windows x86_64](https://github.com/vyx-lang/vyx/releases/latest/download/vyx-sdk-windows-x86_64-llvm22.zip)
- [Linux x86_64](https://github.com/vyx-lang/vyx/releases/latest/download/vyx-sdk-linux-x86_64-llvm22.tar.gz)

`scripts/package_sdk.ps1` writes local packages under the Git-ignored repository
`dist/` directory. Published packages are downloaded through Releases.
See [distribution verification](../docs/TESTING_GUIDE.md#distribution-packages).

For editor tools, first build the current SDK compiler and matching runtime,
then use `out/boot[.exe] build --target vyxc-lsp` and `--target vyxc-dap`.
Prepare the debugger bundle with `python scripts/prepare_debug_adapter.py`
(Cargo/Rust, Git and clang++ are source-build dependencies). SDK packaging
includes the patched engine, its matching LLDB/Python/formatters and notices.
It checks the adapter manifest hashes and prefers the primary compiler artifact
over an old alias held open by an IDE. See the
[editor service gates](../probes/gates/editor-industrial/README.md) and
[debugger build](../tools/debugger/README.md).

## 中文

从本仓库 [最新 Release](https://github.com/vyx-lang/vyx/releases/latest) 下载兼容 SDK
作为 Stage 0，准备 LLVM 22 与宿主构建环境，再按上方示例从清单读取编译器目标并构建。
Stage 0 必须支持公开 inherent `impl` 方法的接口导出，以及多文件逻辑模块的完整导出根。
相同版本号的早期 SDK 可能尚无这些修复，需记录实际哈希并运行上方模块契约门。
使用本次构建的 SDK 编译器入口 `out/vyxc` 验证源码。完整流程见 [构建与验证指南](../docs/TESTING_GUIDE_ZH.md)，
核心特性见 [Fact Semantic Ownership System](../docs/MOSP_ZH.md)。
当前以 AOT 为验收基准；泛型接口、跨模块定义、match 表达式与编译压力门见上方
回归入口。这些门尚不能证明共享前端、并发 LLVM lowering 或大工程性能目标。

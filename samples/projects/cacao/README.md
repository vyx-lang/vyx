# Cacao Direct DCI Example

This example consumes `dci/cacao-cpp-bridge.dcib` and links the vendored
Cacao C++ DCI bridge and Cacao import libraries. It does not use
`@[cpp_include]`, does not parse `CacaoC.h`, and does not invoke a DCI adapter
during compilation.

The `.dci` files are diagnostic JSON only; the compiler consumes the binary
`.dcib` contract. `third_party/cacao/include/CacaoC.h` is retained only as the
producer's reference header. The executable creates a real Cacao triangle
application through Direct DCI calls.

Build from this directory with the SDK compiler freshly produced from the
current checkout using a compatible Release SDK as Stage 0. Use
`vyx_compiler_backend` / `vyx_runtime` from the same build:

```powershell
$compiler = (Resolve-Path ..\..\..\bootstrap_compiler\out\vyxc.exe).Path
.\run.ps1 -BootstrapCompiler $compiler
```

Running `target/cacao_dci.exe` starts continuous rendering; `run.ps1` uses
`--smoke` for its finite 120-frame regression run.

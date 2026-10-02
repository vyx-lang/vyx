# win32_vulkan

Win32 + Vulkan minimal demo as a Vyx project.

## Layout

```
samples/projects/win32_vulkan/
├── Vyx.toml          # package manifest
├── src/
│   └── main.vyx      # entry point
└── run.ps1           # convenience runner
```

## Prerequisites

- The SDK compiler `vyxc` on PATH. Repository regressions use
  `bootstrap_compiler/out/vyxc.exe` freshly built from the current checkout
  using a compatible Release SDK as Stage 0, with `vyx_compiler_backend` /
  `vyx_runtime` from that same build.
- The LunarG Vulkan SDK installed at `D:/Vulkan` (override via
  the `lib_paths` field in `Vyx.toml` or by passing `-L` on the
  command line).
- `vulkan-1.dll` reachable at runtime (any GPU vendor's ICD).

## Build & run

From the project directory:

```powershell
vyxc build              # produces target/win32_vulkan.exe
.\target\win32_vulkan.exe
```

For a repository regression, pass the freshly built SDK compiler and its
matching runtime directory explicitly to the helper script:

```powershell
powershell -File run.ps1 `
  -BootstrapCompiler ..\..\..\bootstrap_compiler\out\vyxc.exe `
  -RuntimeDir ..\..\..\bootstrap_compiler\out
```

## Notes

The project deliberately does not import `std/vulkan_raw.vyx`: its
auto-generated bindings declare every parameter as `rawptr`, which
breaks value-typed flags such as `VkPipelineStageFlags`. Each
Vulkan entry point used by the demo is declared locally with the
proper Win64 ABI types in `src/main.vyx`.

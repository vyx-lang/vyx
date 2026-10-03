# win32_imgui_vulkan

Vyx project that embeds Dear ImGui with Win32 + Vulkan.

## What it shows

- Win32 window creation and message pump in Vyx
- Vulkan instance/device/surface setup in Vyx
- Vulkan swapchain/render-pass frame submission driven from Vyx
- Dear ImGui C ABI calls (`ig*`) from Vyx, including varargs `igText`
- No project-specific C++ app wrapper; native files are third-party cimgui/ImGui/Vulkan backend sources

## Layout

```
samples/projects/win32_imgui_vulkan/
├── Vyx.toml
├── run.ps1
├── src/
│   └── main.vyx
└── third_party/cimgui/
```

## Requirements

- Windows
- `VULKAN_SDK` pointing to a local Vulkan SDK install
- An SDK compiler freshly built from the current checkout using a compatible Release SDK as Stage 0 (`bootstrap_compiler/out/vyxc.exe`), with `vyx_compiler_backend` / `vyx_runtime` from the same build

## Run

```powershell
powershell -File run.ps1 `
  -BootstrapCompiler ..\..\..\bootstrap_compiler\out\vyxc.exe `
  -RuntimeDir ..\..\..\bootstrap_compiler\out
```

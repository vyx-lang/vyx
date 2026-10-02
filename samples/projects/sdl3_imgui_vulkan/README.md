# sdl3_imgui_vulkan

Vyx project that embeds Dear ImGui with SDL3 + Vulkan.

## What it shows

- SDL3 window creation and event pumping in Vyx
- Vulkan instance/device/surface setup in Vyx using SDL3's Vulkan C ABI
- Vulkan swapchain/render-pass frame submission driven from Vyx
- Dear ImGui C ABI calls (`ig*`) from Vyx, including varargs `igText`
- No project-specific native app wrapper; native files are third-party SDL3 and cimgui/ImGui backend sources

## Layout

```
samples/projects/sdl3_imgui_vulkan/
├── Vyx.toml
├── run.ps1
├── src/
│   └── main.vyx
└── third_party/
    ├── SDL3/
    └── cimgui/
```

## Requirements

- Windows for this packaged SDK layout
- `VULKAN_SDK` pointing to a local Vulkan SDK install
- An SDK compiler freshly built from the current checkout using a compatible Release SDK as Stage 0 (`bootstrap_compiler/out/vyxc.exe`), with `vyx_compiler_backend` / `vyx_runtime` from the same build

## Run

```powershell
powershell -File run.ps1 `
  -BootstrapCompiler ..\..\..\bootstrap_compiler\out\vyxc.exe `
  -RuntimeDir ..\..\..\bootstrap_compiler\out
```

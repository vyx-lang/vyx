# Samples

[Documentation](../docs/README.md) · [Project guide](../docs/PROJECTS.md) · [项目指南](../docs/PROJECTS_ZH.md)

All checked-in runnable examples and compiler probes live below this directory.
Tests belong in `tests/`; performance workloads belong in `benchmarks/`.
There is intentionally no top-level `examples/` tree: project examples are
all under `samples/projects/`.

```text
samples/
  bootstrap/  Self-host compiler regression probes used by the bootstrap gates.
  projects/   Standalone project examples, FFI integrations, and Zyn smoke apps.
```

## Bootstrap probes

`bootstrap/` is consumed by the self-host fixed-point and stage-conformance
scripts.  Add a probe here only when it exercises a bootstrap compiler path;
use `tests/` for general regression coverage.

## Project examples

`projects/` contains runnable packages and platform/FFI samples.  Each
directory that owns a `Vyx.toml` is self-contained; its local `run.ps1` or
build helper is part of that example rather than a repository-wide test gate.
Use paths relative to the project root in its manifest and keep generated
`.cache/`, `target/`, `out/`, and `tmp_*` material ignored.

## Zyn applications

Build Zyn application projects through `Zyn/bin/zyn.ps1` (or the matching
platform entry point) so the SDK stages native libraries, shaders, and assets.
The [Zyn guide](../Zyn/README.md) covers application actions, dynamic views,
recording, and platform setup.

| Project | Purpose |
|---|---|
| [`zyn_todolist_app`](projects/zyn_todolist_app/README.md) | Application-defined commands, state resources, recording, and timeline reply |
| [`zyn_recomposition_smoke`](projects/zyn_recomposition_smoke/README.md) | Composed controls, content sizing, keyed lists, display density, and a headless self-test |
| [`zyn_canvas_view_smoke`](projects/zyn_canvas_view_smoke/src/main.vyx) | Custom 2D painting, clipping, and pointer action payloads |
| [`zyn_custom_render_view_smoke`](projects/zyn_custom_render_view_smoke/src/main.vyx) | Cacao GPU content composited with ordinary UI |

The 2026-10-01 Zyn AOT regression built the full framework with the
self-hosted SDK compiler and ran the recomposition self-test with exit code
`0`. That test validates layout and state contracts without drawing a window;
GPU, visual, Android, and accessibility checks use their separate gates.

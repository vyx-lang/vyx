# Zyn recomposition and timeline smoke project

This project uses `FormField`, `SearchBar`, `Disclosure`, `EmptyState`,
`ChoiceGroup`, and `DataGrid` in an application component. Toggling the
disclosure changes the component's view tree after a command updates its
world state.

## Build and self-test

Build from this directory with `..\..\..\Zyn\bin\zyn.ps1 build`. Then run
`target\zyn_recomposition_smoke.exe --self-test`. The self-test writes
`target/smoke.zyns` and returns `0` on success. It runs without SDL, a window,
or a renderer.

It checks:

- A command changes the disclosure's view structure, and recorded commands
  replay twice from the same initial state.
- An MD3 card carries its elevation into the retained UI node.
- Panel padding is applied once, and a composed form leaves room before its
  following sibling.
- Explicit fixed height and `fillHeight()` use their separate sizing paths.
- List business keys identify both rendered rows and their action bindings;
  insertion and reordering keep the same key identities.
- Two physical pixel densities produce the same logical viewport and touch
  target, with the expected mapping back to window pixels.

On 2026-10-01 the self-hosted SDK compiler completed a full Windows AOT
Zyn build (`-j4 -O0`, 177 tasks including root chunks), then built this sample;
the executable's `--self-test` returned `0`. To reproduce that compiler gate
from the repository root:

```powershell
Push-Location Zyn
& ..\bootstrap_compiler\out\vyxc.exe build --target Zyn -j4 -O0
Pop-Location
Push-Location samples/projects/zyn_recomposition_smoke
& ..\..\..\bootstrap_compiler\out\vyxc.exe build -j4 -O0
& .\target\zyn_recomposition_smoke.exe --self-test
Pop-Location
```

The task count records that run; it is not a gate invariant. The self-test
does not inspect shadow screenshots, font coverage, multiline typography,
or a native accessibility bridge. Current lists render fixed-height checkbox
rows, with arbitrary row builders and variable-height virtualization still
pending. See [view composition](../../../Zyn/docs/COMPOSITION_ZH.md) for the
sizing and key-resource contracts.

## Record and replay

For the interactive paths, run the executable with:

```text
target\zyn_recomposition_smoke.exe --recording -o target\manual.zyns
target\zyn_recomposition_smoke.exe --reply target\manual.zyns
target\zyn_recomposition_smoke.exe --reply target\manual.zyns --headless --repeated 1000
```

The first command records actions until the window closes. The second opens
a window and renders the recorded frames; the third checks command results
without starting the renderer. A recording and its replay must use the same
application configuration.

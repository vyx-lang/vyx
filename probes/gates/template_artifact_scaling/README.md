# Template artifact scaling

Windows AOT regression for shallow `.vyi` emission and consumption. The fixture
combines a large string literal, 2,048 declarations, 2,048 statements and a generic
function. It checks payload hashes independently and executes a consumer
compiled from the generated interface.

```powershell
.\probes\gates\template_artifact_scaling\run.ps1 -Compiler <fresh SDK compiler> -BaselineCompiler <previous compiler>
```

`-BaselineCompiler` is optional and only processes the small compatibility
fixture. Its v1 surface, payload and node count must match the new encoder. The
old encoder must not process the large fixture: growing hex prefixes and nested
chain suffixes used quadratic arena storage.

Emission processes have a 2 GiB private-memory guard; the large consumer has a
512 MiB guard to catch copying of nested field suffixes during decoding. Every
process is also stopped if host physical or commit headroom falls below 2 GiB.
Logs and measured peaks are kept in `.runs/`.

The related scheduler classification regression is in `pipeline_scheduler/`:
`.vyi` actions use a Vyx frontend memory prior while remaining distinct from
code generation slots.

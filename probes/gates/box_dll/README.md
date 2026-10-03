# Cross-DLL `Box<dyn Handler>` regression

Run from the repository root on Windows with the SDK compiler built from current
source and its matching backend/runtime:

```powershell
$env:LLVM_ROOT = Join-Path (Get-Location) 'clang'
./probes/gates/box_dll/run.ps1 -Compiler ./bootstrap_compiler/out/vyxc.exe
```

To reproduce the historical failure, set `$unfixedCompiler` to a saved pre-fix
SDK compiler executable beside its matching backend/runtime, then run:

```powershell
./probes/gates/box_dll/run.ps1 -Compiler $unfixedCompiler
```

The runner builds fresh copies of three small projects in `.runs/`. It forces
root partitioning and requires at least two root object files. Compiler output,
probe output, the exact compiler path and exit status remain in that directory.
The two build environment switches are restored when the runner exits.

`BoxAbi.dll` declares a non-alphabetical interface and receives objects created
by the EXE. A DLL function clones the handlers into another `World`; the EXE
destroys the original and then dispatches on the clone. Each implementation
contains three strings and an integer, so the check also observes payload
preservation. `BoxFactory.dll` tests the reverse direction by creating an
object that the EXE calls, clones and drops.

The failure this gate targets is an incomplete interface method set in a
receiver-only codegen unit. Reachability pruning discarded unused interface
declarations, shifting the slots of remaining virtual methods. A module calling
`dup` could therefore use a different slot from the module that built the
vtable. Preserving the complete interface declaration set fixes the ABI without
changing `Box` allocation. A separate `unused_default.vyx` fixture contains an
unused interface default method that calls a deliberately undefined external
symbol. Linking it with only `main` as an emit root additionally checks that
retaining declaration shells does not make the default body reachable.

Expected success: `box_dll: OK` and exit `0`.

## Verified results (2026-09-27, Windows x64)

All three compiler variants successfully built both DLLs and the EXE with two
root codegen units in `BoxAbi`. Fresh source copies were used for each run.

| Compiler variant | Executable result |
| --- | --- |
| Original SDK compiler (before fix) | Access violation, exit `-1073741819` (`0xC0000005`) |
| SDK compiler with method-order sorting only | Exit `1`: the cloned handler returned the wrong label |
| Complete MIR interface declaration shells (SDK compiler) | `box_dll: OK`, exit `0`; unused-default-body probe also linked and exited `0` |

The sorting-only result confirms that ordering cannot repair a method set
whose contents differ between codegen units. The final change retains all
interface declarations and keeps the existing slot-ordering policy.

The fixed compiler also reached a self-hosting fixpoint: S2 and S3 binaries
had the same SHA-256:

```text
9CEA76C4C8989C8B14E2E0D8592CFBBA292CFA88D37512A39E4DF8B8FCEEF4B3
```

The existing Windows `tests/projects/dci_cpp_trait/run.ps1` was also attempted.
Its C++ adapter succeeded, but both the fixed compiler and the original compiler
reject `host as *SinkG<i32>` at `src/main.vyx:101` with parser error `E0002`.
This fixture did not reach linking or execution, so it is not a passing DCI
regression result. The Linux spdlog gate was not run because LLVM 22 was not
available in that environment.

## Zyn application validation

After rebuilding `Zyn.dll` and `samples/projects/zyn_todolist_app` with the fixed
compiler, the original `Box<dyn AppCommandHandler>` application passed:

| Check | Result |
| --- | --- |
| `zyn_todolist_app.exe --self-test` | Exit `0`; includes cloning, command dispatch, timeline export/load and replay |
| `zyn_todolist_app.exe --reply target/todo_manual.zyns --headless --repeated 3` | Exit `0` |
| `zyn_todolist_app.exe --visible` | `Task Deck` window opened; exit `0` after closing the window |
| `zyn_todolist_app.exe --reply target/todo_manual.zyns` | Visual replay confirmed working by the user |

The application and framework source were unchanged. The user also confirmed
that the rebuilt visible application works. The interrupted visual replay test
did not retain an automated exit status; its result above is user confirmation.

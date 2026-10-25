# Compiler root list inputs

After building the current SDK compiler, run from the repository root:

```powershell
python probes/gates/compiler-root-lists/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

`--export-root-names` selects export roots; `--emit-root-names` filters the
emission partition. Both accept comma-separated names and the project worker's
pipe-separated form. Their `-file` options accept the same syntax. Surrounding
whitespace is trimmed per name. Commas inside generic arguments are preserved:
`--export-root-names "choose<i32,i64>,third"` selects two roots.

The gate checks native LLVM function definitions and CUDA PTX `.entry` symbols,
including one, two and three roots, whitespace, pipe compatibility, mixed
separators, empty lists, generic arguments and file inputs. An unselected
`first_extra` function checks exact name matching. `--export-all` and
`--export-top-level-roots` remain control cases. A native AOT application calls
the selected functions and checks their results at O2.

PTX generation requires the SDK NVPTX backend; it does not require a CUDA device
or toolkit. The separate [CUDA symbol gate](../cuda-symbol-identity/README.md)
checks manifest builds, embedded host symbols, `ptxas` and actual GPU launches
when available. This gate does not certify GPU execution.

Inputs, compiler/runtime hashes, commands and results are written under the
ignored `.cache/<timestamp>/` directory. `--output-dir` selects a fresh directory;
existing evidence is not overwritten.

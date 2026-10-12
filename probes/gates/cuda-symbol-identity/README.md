# CUDA symbol identity

```powershell
python probes/gates/cuda-symbol-identity/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

The gate copies `tests/projects/cuda_kernel` into a fresh ignored result
directory. Its three device sources retain the original kernel body and use no
module header, `module gpu.compute`, or a versioned module with a variant.
The original project stays unchanged.

Each source is compiled by the SDK's NVPTX backend. On Windows with a CUDA
toolkit, each clone is also built through its manifest. The gate compares the
actual PTX entry with the embedded kernel symbol, validates PTX with `ptxas`,
and runs the host's GPU output assertion when a CUDA device is available.
Missing platform dependencies or devices are recorded as skips with exit `77`;
driver initialization, project compilation, symbol mismatches and launches that
fail are failures. The default architecture is `sm_120`; `--arch` selects another
installed device architecture.

Commands, compiler/runtime identities, outcomes and GPU availability are saved
in `.cache/<timestamp>/verification.json`. `--output-dir` selects a fresh result
directory; existing results are never overwritten.

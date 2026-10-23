# AOT CPU portability

The LLVM bridge must emit the target's baseline ISA. It must not implicitly use
the build host CPU's extensions. In particular, a Linux x86-64 SDK built on an
AVX-512 runner must work on an x86-64 machine without AVX. GPU targets continue
to honor explicit NVPTX CPU selection.

```powershell
python probes/gates/aot-portability/run.py --backend bootstrap_compiler/out/vyx_compiler_backend.dll --compiler bootstrap_compiler/out/boot.exe --output .cache/aot-portability
```

On Linux, use the matching `libvyx_compiler_backend.so` and compiler, with
`LD_LIBRARY_PATH` including their library directory. Select the matching
`VYX_RUNTIME_ROOT`, `VYX_STD_PACKAGES` and LLVM tools when testing a source build.

The gate constructs a 64-byte zero initialization through the actual bridge
API and emits optimized assembly for the host, x86-64, AArch64 and NVPTX.
It rejects AVX instructions in the x86-64 baseline and checks explicit `sm_80`
selection. The pattern reproduces the inline-buffer stores that caused
`StringBuilder.withCapacity` to trap with `SIGILL` in a distributed Linux SDK.
With `--compiler`, it also builds and runs a separate Vyx project that uses
`StringBuilder` and must print `4429`. Output directories must be new.

Release builds run the emission check before compiling the SDK. They build
the current native bridge first and load it beside Stage 0, so Stage 0's old
host-specific bridge cannot contaminate the newly emitted SDK code.

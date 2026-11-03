# POSIX compiler process startup

`python probes/gates/process-startup/run.py` compiles and runs the production
launcher from `vyx_codegen/src/bootstrap_process_posix.hpp` on Linux or macOS.
A native C++ compiler is required. Windows returns `77` (not a POSIX gate).

The launcher creates each process group in the child, then confirms startup
through a close-on-exec pipe before publishing its handle. The parent never
races the child's `setpgid`. Failures report the operation and errno and reap
the child. Neither startup failures nor failed compilations are retried.
Argument/environment storage is prepared before fork; the child only uses
async-signal-safe operations through exec.

The checks cover exact arguments, a working directory containing spaces,
environment isolation, separate output streams, relative PATH lookup, script
execution, missing executables, permission errors, invalid cwd/redirection,
closed standard descriptors, immediate process-group cancellation, and 512
child starts from four concurrent threads. Failures must leave no unreaped
children. A test-only syscall shim injects `EINTR` and `EPERM` at `setpgid`:
interruption is retried, a real startup failure is reported and reaped.
`verification.json` records the host, launcher hash and results.

This is the native startup gate; the separate [test runner gate](../test-runner/README.md)
also exercises the same runtime through the freshly built SDK compiler.

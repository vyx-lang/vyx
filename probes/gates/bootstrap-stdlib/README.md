# Stage 0 standard-library closure

```sh
python probes/gates/bootstrap-stdlib/run.py --compiler /path/to/stage0 --sdk /path/to/stage0-sdk --output .cache/bootstrap-stdlib
```

Use the same Stage 0 executable and compiler backend selected by the release
builder. This gate uses that SDK's runtime and canonical `std_packages` tree.
It builds a real parallel project with the compiler target's std exclusion list,
then runs Vec iteration/clone and StringBuilder. Both explicit
`parallel_std_sources` and implicit `VYX_STD_PACKAGES` must select the same root.
A missing registry, missing source, escaping source or split roots fails before
compilation. Generated module-source lists must also stay inside the SDK's
canonical tree or the probe project. There is no fallback to the compatibility
`std/` snapshot.

`collections/src/iter.vyx` belongs to the compiler's selected standard-library
closure: `Vec.iter()` returns `Iterator<T>`. Excluding it leaves the generated
`std.collections` interface without the return type's layout. The current-source
[native stdlib gate](../bootstrap-native-stdlib/README.md) runs this combination
at O0/O2 too, so a release cannot rely solely on an older Stage 0 passing it.

Release native Windows/Linux/macOS builds and the macOS cross exporter run this
gate before compiling the full compiler. Logs and `verification.json` include
the compiler/backend/runtime identities, registered and planned source hashes,
command and native run result.
The registry hash and source hashes are checked again afterward. This validates
the bootstrap input closure; it does not replace current-source standard-library
contracts after S1 is built.

Before publication, native Windows/Linux/macOS SDKs run the same preflight with
their packaged compiler, runtime and standard library. macOS runs it after
moving the SDK to a path containing spaces. This checks the next build's Stage 0
inputs; the result is recorded as `packaged_bootstrap_standard_library`.

# Real Rust ecosystem DCI gate

From the repository root, with the SDK compiler built from current source and
the locked crates available in the local Cargo registry cache:

```powershell
./probes/gates/dci-rust-ecosystem/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

This fixture uses the locked Cargo project under `native/`, with the real
`crc32fast 1.5.2` and `adler2 2.0.1` crates. Cargo resolves the dependency
graph and features; the adapter records and replays that environment instead
of guessing `rustc --extern` arguments. The contract retains Rust's original
`&[u8]` spelling and the producer-measured two-word slice view
`dci.RustSlice_u8` (pointer plus length).

The Vyx source consumes the public checksum APIs over files from the Cargo
oracle. It has no handwritten C ABI wrapper and no preselected generic export
adapter export list; the contract selects the original public item by path.
`run.ps1` is the gate entry point; it runs Cargo offline and locked,
checks native oracle output, generates both DCI contracts, and then performs
the Vyx AOT build when the selected Windows Rust target is available.

The script uses the standalone `dci_adapter_rust.py` Cargo entry point.
Cargo-specific flags are not yet in the unified SDK CLI parameter registry.
For each selected ordinary public function, `--native-lib-out` generates a
linkable Rust-native bridge and records its link identity. This bridge is an
explicit implementation artifact; the fixture's lack of a handwritten C ABI
wrapper does not imply a bridge-free call. The gate runs both O0 and O2 and
compares results with the independent Cargo oracle. Changing crate versions,
target, features or compiler requires matching new contracts and artifacts.

The upstream `crc32fast::hash` item is retained in the contract and native
bridge. The consumer adds the contract spelling `crc32_hash` because `hash` is
reserved by the Vyx host builtin; both names use the same measured ABI and
bridge link identity.

Coverage is Windows x86_64 MSVC AOT with the selected checksum functions and
measured slice view. Arbitrary crate generics, data-carrying enums, all trait
forms and unwinding Rust panic are not established by this result. JIT is
deferred. Repeat this fixture with the
[industrial runner](../dci-industrial/README.md) for workload measurements.

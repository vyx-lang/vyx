# Original `std::vector<T>` through DCI

The producer header includes only `<vector>`. It declares no concrete template
instance, exported vector wrapper, alternate allocator, or substitute method.
The Vyx consumer declares open `std.vector<T>` and constructs
`var a = std.vector<i32>{1,2,3,4,5};`. Its source also selects `f64`.
The build discovers both instances and asks the C++ compiler for their layouts
and original method ABIs. A generated ABI entry implements brace construction
with placement `std::vector<T>{...}`; ordinary member calls and destruction link
the original standard library symbols.

Run from PowerShell with the SDK compiler built from current source and its
matching backend/runtime:

```powershell
./probes/gates/dci-vector/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

The command selects `bootstrap_compiler/out/vyxc.exe` and the bundled LLVM toolchain.
It rebuilds a contract with no exported specialization and checks cold and warm
project builds. AOT programs at `O0` and `O2` each perform:

- 64 independent `i32` and `f64` vector runs, with 405,504 total pushes;
- 896 comparisons against independently constructed native C++ vectors,
  covering all elements, size, capacity, reserve, repeated growth, writes through
  borrowed `at`/`data`, pop, clear, and reuse of capacity;
- two genuine `std::vector::at` bounds failures, caught as `std::out_of_range`
  after propagation through Vyx and cleanup of both active vector objects;
- 132 consumer object allocations/releases plus standard allocator
  allocation/release accounting, checked after each scope and exception.

The allocation instrumentation changes only allocation calls in emitted Vyx IR.
The harness observes real vector objects and verifies their last registered
buffers have been released before consumer object storage is freed. Native
reference vectors allocate while accounting is suspended. Generated artifacts
and logs live under this fixture's `.cache`, `target`, and `out` directories.
An independent negative project constructs `std.vector<i32>{1.5}` and must be
rejected by the C++ producer's original brace narrowing rules.

`@[dci_list_init(true)]` requests producer-checked C++ brace construction.
Borrowed return metadata is explicitly requested for the original `at` and
`data` members. This gate currently targets the bundled Windows MSVC ABI and
uses AOT throughout.
The measured growth and bounds cases do not establish support for every element
type, allocator, STL implementation or standard library template. Run the
[industrial runner](../dci-industrial/README.md) for repeated fixture execution
and process measurements; JIT remains later work.

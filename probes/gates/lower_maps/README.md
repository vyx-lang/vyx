# LLVM lowerer ID-map regression

Run from the repository root on Windows:

```powershell
./probes/gates/lower_maps/run.ps1 -Compiler ./bootstrap_compiler/out/vyxc.exe
# Reproduce the old production helper defect without changing the checkout:
./probes/gates/lower_maps/run.ps1 -Compiler ./bootstrap_compiler/out/vyxc.exe -SourceRef 8e44b876 -ExpectLegacyResetFailure
```

The runner extracts the actual map functions from `llvm_lower.vyx` and their
raw-memory accessors from `ast.vyx`, compiles them with the selected native SDK
compiler built from current source, and runs a small Vyx caller. Map allocation,
indexing, growth, clearing and disposal are not reimplemented in the test. Only diagnostic display is
adapted to `print` so the probe does not import the complete compiler.

`-Compiler` selects a compiler beside its matching backend/runtime. `-SourceRef`
selects an older lowerer implementation using `git show`; it does not checkout,
overwrite or rebuild the compiler. Generated sources, binaries, implementation
hashes, exit codes and logs remain under ignored `.runs/`.

## Defect and fix

Previously every pointer map reserved at least 262145 pointer slots; every i32
map reserved at least that many integers. Larger creation hints allocated more,
but CGU reset always cleared exactly 262145 slots. Thus a stored LLVM pointer
above ID 262144 could survive reset and refer to the preceding LLVMContext.
Later MIR growth could also write beyond a map's originally allocated capacity.

The 51 initial maps alone reserved and zeroed approximately 81 MiB per lowerer
(30 pointer maps and 21 i32 maps). Three lazily created CGU i32 maps could add
another 3 MiB. Each CGU reset cleared roughly 43 MiB even for tiny input.
These are allocation sizes derived from the source, not measured process RSS.

Maps now use a stable descriptor with a data pointer, capacity, written high-water
ID and element width. Initial counts size the first allocation; subsequent writes
grow storage to accommodate their actual ID. Reads of unallocated entries return
the empty cache value. Stored pointer values and descriptor aliases remain valid
when the slot buffer moves. Growth uses allocate/zero/copy/free, so it never loses
the old buffer before allocation succeeds.

Clear operations intersect the requested range with the written extent and do
not allocate. Reset clears that extent before lowering the high-water mark;
merely lowering the mark would let later writes expose stale values again.
The existing set of CGU/body/instruction reset call sites is unchanged. Owned
payload chains retain their prior cleanup order; descriptor disposal releases
both the buffer and descriptor. This change does not introduce generation tags.

This also avoids relying on misleading creation hints: `cgu_retain_slot` is keyed
by function ID rather than collected-slot capacity, while
`cgu_static_defined_map` is keyed by HIR item ID rather than function count.

## Coverage

- Pointer and i32 reset above 262144, including the exact old boundary.
- Growth from a tiny map to ID 700000, preservation of low entries and stable
  descriptor aliases; negative i32 values remain intact.
- Unwritten/out-of-capacity reads, invalid IDs, empty/reversed ranges and clears
  extending to `INT32_MAX` without allocating or iterating the entire range.
- Partial clear preserving neighboring values, complete reset followed by a
  higher write, and absence of resurrected entries.
- Null disposal and 1000 allocate/write/read/dispose cycles.
- Real initial-allocation and growth-allocation failure on Windows: a launcher
  assigns the probe a 256 MiB process-memory Job Object limit, then requests a
  map requiring several GiB. `malloc` genuinely fails; no allocator is mocked.

OOM must print `LLVM MIR ID map allocation failed` and exit `1`. The launcher
returns success only for that expected child status, and the script also checks
the diagnostic text. A 30-second timeout prevents an unexpected allocator path
from leaving the probe running. The allocation error is fatal: silently losing
a function declaration or LLVM value write is not a valid cache miss.

## Verified results (2026-09-27, Windows x64)

| Production helper version | Result |
| --- | --- |
| Original helper, before source changes | Both pointer and i32 reset retained ID 262146; expected exit `17` |
| Dynamic descriptors | `lower_maps: OK`, exit `0` |
| Dynamic descriptors, growth OOM | Expected diagnostic, child exit `1` |
| Dynamic descriptors, initial allocation OOM | Expected diagnostic, child exit `1` |

Initial old-helper evidence is in `.runs/20260927-161832-639/`; the complete new
helper and both OOM checks are in `.runs/20260927-162114-600/`.
This is a focused helper regression, not a substitute for rebuilding the SDK
compiler, comparing hello IR, running cross-DLL Box/CGU tests, and reaching S2/S3 fixpoint.
Those integration gates belong to the parent change's final validation.

# Termux SDK library isolation

Run on a Linux host with Python 3.11+, a C compiler and `patchelf`:

```sh
python3 probes/gates/termux-sdk/run.py --patchelf /path/to/patchelf
```

The gate builds two real ELF libraries with the same `libLLVM.so` SONAME and
incompatible symbols. An SDK compiler loads one and launches a system clang
that requires the other. Exporting the SDK's library directory reproduces the
child's missing-symbol failure. The Termux packager's ELF binding helper then
keeps the compiler on its private library while system clang loads its own.
The test also executes a nested compiler and preserves a caller-supplied
library path; an unset library path is checked separately.

The private library exports `LLVM_21.1` versioned symbols. The gate reproduces
the old absolute-path `verneed` association, rejects it with Android's SONAME
lookup rule, and checks the repaired namespace with the native Linux loader.
A provider without the required version is rejected; the version name, hash
and index must survive packaging. Reusing a previous namespace is also tested.

This checks ELF process and dependency behavior on Linux. The Android package
checks its AArch64 closure, required private symbol versions and launcher
separately. A physical Termux device
must still run `vyxc build` to certify Android loading and native linking.

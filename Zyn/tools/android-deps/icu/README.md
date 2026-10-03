# ICU 78.3 for Android

This wrapper builds unmodified ICU common and i18n sources with the Windows NDK
toolchain. It reads the same `sources.txt` lists as the upstream `Makefile.in`:
202 common sources and 254 i18n sources. It builds full shared libraries and
keeps upstream `_78` symbol renaming.

```powershell
pwsh -NoProfile -File Zyn/tools/android-deps/icu/Build-Icu.ps1 -Jobs 2
```

Defaults: NDK `E:/Android/sdk/ndk/30.0.15729638`, Android API 29, `arm64-v8a` and
`x86_64`, CMake/Ninja from PATH, release `-O2`, and `c++_shared`. For one ABI use
`-Abi arm64-v8a`; `-ConfigureOnly` stops before building. Downloads and build
logs remain under `Zyn/tools/android-deps/.work/icu/`; existing build directories are reused.

Outputs are staged in `Zyn/vendor/ICU/lib/android/<ABI>/`:

- `libzynicuuc.so`, `libzynicui18n.so`, `libzynicudata.so` with matching private SONAMEs. Android also ships libraries named `libicu*.so`; private names prevent the loader from selecting the system ICU in place of the bundled ICU 78.
- The complete upstream `LICENSE`, including third-party notices.
- `manifest.json` recording source and artifact hashes, data size, ELF machine,
  SONAMEs, dependencies, NDK revision, commands, and validation status.

The matching NDK `libc++_shared.so` must be packaged once per ABI by the app.
The wrapper does not build ICU I/O, host generators, samples, or the upstream
test suite, and does not remove common/i18n features.

## Full ICU data

The official little-endian `icudt78l.dat` contains 4,305 entries and 33,107,232
bytes. Both supported Android ABIs use its little-endian, ASCII, 16-bit UChar
format. The assembly wrapper embeds those bytes into an exported, 16-byte
aligned `icudt78_dat` object, following the ELF object format used by upstream
`source/tools/toolutil/pkg_genc.cpp`. This avoids a Windows host ICU tooling
build. `source/stubdata/stubdata.cpp` is never linked.

The build checks the data hash, each ELF architecture and SONAME, ICU symbol
exports and library dependencies, and the data object's size. It extracts the
linked `.rodata` and requires a byte-identical SHA256 match with the official
`.dat`. These checks verify packaging, not execution on Android.

`icu_data_smoke` is also built under `Zyn/tools/android-deps/.work/icu/build/<ABI>-api29/`. On-device
execution must return zero and print `ICU 78.3 full data: locale, collation and
Thai dictionary OK`. It checks initialization, data version, Chinese locale and
collation data, and Thai dictionary segmentation. The staging manifest records
device execution as false until a separate device runner performs that test.

## Sources and licensing

Downloads are pinned to [the official ICU 78.3 release](https://github.com/unicode-org/icu/releases/tag/release-78.3):

| Asset | SHA256 |
| --- | --- |
| `icu4c-78.3-sources.zip` | `20b295e2c23c541aec17f35c546e4d3136b7ab7d58e3a5fc4e479958a69b1a2c` |
| `icu4c-78.3-data-bin-l.zip` | `982619632b78887f1895b063e96e8c3cc7f99283337c8abbd05aa71635de613c` |
| Extracted `icudt78l.dat` | `d5cf2a40dccbe471781ec7af85693bff542ff12f0b670c9630c4e72d60714b8b` |

The release is licensed under Unicode-3.0 with bundled third-party notices.
Keep the complete release `LICENSE` with redistributed libraries/data or their
associated documentation. The script stages that file without modification.

The [official ICU cross-build procedure](https://unicode-org.github.io/icu/userguide/icu4c/build.html#how-to-cross-compile-icu)
first builds host tools and then uses `--with-cross-build` for the target. The
current Windows setup has CMake/Ninja and a Windows NDK, while its Git Bash
environment has no GNU make and WSL has no Linux NDK. This local wrapper avoids
those extra installations while retaining upstream source lists and released
data. It is a project build wrapper, not an upstream ICU CMake build system.

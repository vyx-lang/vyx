# Native string-length cache regression

Run from the repository root on Windows:

```powershell
./probes/gates/pipeline_string_lifetime/run.ps1
# Select a real compiler backend DLL:
./probes/gates/pipeline_string_lifetime/run.ps1 -Runtime ./bootstrap_compiler/out/vyx_compiler_backend.dll
```

The small C++ probe loads the **actual DLL** and calls its exported runtime
functions. It does not extract, simulate or replace the production cache.
Building the probe does not rebuild the backend. A pre-fix DLL can be checked
with `-ExpectOldFailure`; this accepts only the specific stale stack-buffer
length failure (exit `10`).

## Regression and contract

An unknown external pointer used to be cached after its first length scan.
Mutating that buffer or reallocating the address could then retrieve the old
length. Cache collisions could even spill that unregistered pointer into the
shared length map. Native file-stamp functions returned temporary
`std::string::c_str()` pointers through that path, corrupting otherwise identical
build-cache records.

Unknown external scan results now remain uncached. Registered Vyx byte views
retain their explicit lengths and still require correct registration/lifetime
management. `from_cstr` accepts a valid NUL-terminated C string (or null for an
empty string) and determines its NUL length independently of registered view
metadata. `from_cstr_len` and `from_cstr_view_len` retain explicit byte lengths,
including embedded NUL bytes. Native `std::string` file-stamp results copy using
their actual `.size()`.

This change does not remove the legacy bounded scan used by pointer-only
`vyx_string_len` when given an unregistered address, or solve registration
lifetimes across arbitrary external memory owners. Those need the broader
length-carrying ABI and lifetime work; the C-string boundary no longer depends
on that compatibility scanner.

## Coverage

- Same stack address with different C-string lengths, including direct-cache
  lookup after another query.
- A freed/reallocated heap address with a new string length; the probe requires
  actual address reuse to occur.
- Registered embedded-NUL views, forget/re-query, explicit-length copies and
  null C strings.
- A valid C string larger than 1 MiB, without silent truncation.
- 32 files, each stat/content stamp observed in 64 rounds with unchanged bytes.

## Verified results

Windows x64, 2026-09-27:

- Old real DLL SHA-256
  `FD52D2C8309C5272FE87EABEE736919C372ECB26CFAD273F5F4B251DECEBFC29`:
  `FAIL unknown stack pointer reused stale length` (expected exit `10`).
- Rebuilt real DLL SHA-256
  `1D4BE452AAD600D5ECD5FB777C01D5B3C48B7BCCCB7AD84457BA80B1D5A37BBA`:
  `pipeline_string_cache: OK stack/heap/registered/forget/embedded-NUL/stamps`
  (exit `0`).
- Full modified runtime source passed `clang++ -fsyntax-only`.

The runner prints the exact DLL path and hash. Probe binaries and fixture files
remain in ignored `.runs/`.

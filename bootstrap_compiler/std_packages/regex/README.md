# Regular expressions

`std:regex` / target `std_regex` exposes `std.regex`. It is also a dependency
of the `std:std` aggregate. The static package compiles its pinned PCRE2 10.49
sources; consumers need no PCRE2 installation, CMake or network access.
The public API, validation, matching scratch and cleanup are implemented in
Vyx and call the original PCRE2 functions directly. There is no custom C
wrapper ABI. Native size_t parameters follow the SDK's supported 64-bit C ABI.

```toml
[dependencies]
regex = { path = "std:regex", target = "std_regex" }
```

```vyx
use std.regex;
let pattern = Regex.compile("^/jobs/(?<id>[0-9]+)$").unwrap();
let hit = pattern.full_match("/jobs/42").unwrap().unwrap();
let id = hit.group(pattern.group_number("id").unwrap() as i64).unwrap().unwrap();
```

`compile` and `compile_with` return located `RegexError` values. Options select
case folding, multiline anchors, dot-all and Unicode mode (UTF-8/UCP by
default). Search uses `find`, whole-subject matching uses `full_match`.
`find_from` accepts a byte offset, a full-match flag and `RegexLimits`.
With `full=true`, matching starts at the supplied offset and must reach the
subject's end. Use `full_match` to require the entire input.

Capture 0 is the entire match. Offsets are bytes; an unmatched group is
`None`, a matched empty group is `Some("")`. Match results own one subject
snapshot and offsets, so they survive dropping the input and pattern. Text is
copied only when a capture is requested. Out-of-range groups, missing named
groups, invalid UTF-8, an offset inside a code point and a destroyed pattern
produce errors. A nonmatch is `Ok(None)`.

The engine is the PCRE2 interpreter, with JIT disabled and `\C` forbidden.
This is a bounded backtracking engine, not a linear-time guarantee. Default
per-match limits: 1,000,000 steps, depth 1,000, heap 8 MiB. Step/depth limits
must fit a positive u32; heap is 1..65536 KiB. Patterns are at most 65536 bytes
and 256 capture groups; subjects are at most 16 MiB. Limit exhaustion is an
error. Pattern directives can reduce engine limits. No-match and errors are
never conflated. Destroy a pattern after all users finish; each invocation
allocates independent matching scratch.

The older ASCII classification and wildcard helpers remain. `glob_match`
recognizes only `*` and `?`: a dot is literal. `wildcard_find` retains its
older `.` / `?` wildcard behavior. Scan offsets outside 0..len panic before
reading memory. Global iteration, substitution, splitting and serialization
are not exposed by this wrapper.

Upstream sources, licence and SHA-256 input inventory are in `vendor/pcre2/`.
The portable config enables only the 8-bit interpreter and Unicode. The
distributed default character tables and generic public header are renamed
to their build names; there is no host-locale table generation.

The [contract gate](../../../probes/gates/stdlib-contracts/README.md) tests
the Vyx API against Python `re` for their shared syntax and exercises a
cross-module TOML-configured router. It also provides native sanitizer checks.

The compiler still has an [owning enum payload cleanup gap](../../../probes/gates/enum-payload-cleanup/README.md).
Discarding a Result error can retain its owned diagnostic String. Native
handle counters and bridge-only sanitizer success do not prove that ignored
Vyx Result/Option payloads are destroyed.

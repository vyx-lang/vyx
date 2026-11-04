# TOML documents

`std:serde`, target `serde_toml`, exposes `std.serde.toml`. It builds the
vendored tomlc17 R261003 parser and its native bridge as a static package.
No separate library installation or network access is required.

```toml
[dependencies]
toml = { path = "std:serde", target = "serde_toml" }
```

```vyx
use std.serde.toml;
let document = Toml.parse("[server]\nport=8080\n").unwrap();
let server = document.require("server").unwrap();
let port = server.require("port").unwrap().as_i64().unwrap();
```

Parsing covers TOML 1.1 strings/escapes, quoted and dotted keys, tables,
inline tables, arrays, arrays of tables, checked i64 integers, floats,
booleans and the four date/time types. Duplicate definitions and malformed
or overflowing scalars are errors. Input must be UTF-8; literal NUL bytes
are rejected, while escaped NUL bytes in strings/keys retain their lengths.

`Toml` is an immutable, move-owned handle. `get(key)` returns
`Result<Option<Toml>, TomlError>`; `require(key)` treats absence as an error.
Keys are exact strings: `require("a.b")` looks up the literal key `a.b`.
Traverse a dotted table using successive `require("a").require("b")`
operations, unwrapping each Result. No ambiguous automatic path splitting
occurs. `keys` returns owned names, `at` indexes arrays, `size` accepts only
containers. Typed getters reject coercion. `as_string` returns owned String.

Children and Clone retain an immutable reference-counted tree; they remain
valid after the parent or input text is destroyed. Reference counters are
atomic. This does not permit destroying one Vyx handle concurrently with
access through that same handle. `destroy()` on a handle is idempotent.

`TomlError` carries a kind, message and available source location. Parser
failures provide the upstream line number (0 when unavailable); their column
is 0. Type/missing/range failures use the node's line/column. Parser allocation
failures currently share the Parse kind and a diagnostic message; handle
allocation failures have Allocation kind.

`TomlDateTime.kind` distinguishes Date, Time, DateTime and OffsetDateTime.
Only corresponding fields are meaningful; `offset_minutes` applies to
OffsetDateTime. Fractional seconds truncate to microseconds. Lexical spelling,
comments and fractional digits beyond that precision are not preserved.

Limits: 16 MiB input, 30 nested array levels, 30 nested inline-table levels,
16384 entries per table/array, plus tomlc17's numeric/timestamp token limit
(`LITBUF_SIZE` in the vendored source). This is an immutable reader, not an
editable document, writer or round-trip formatter.

The older `toml_*` line/list helpers remain for the compiler's manifest and
CLI-list callers. They are **not** a TOML document parser; this change does not
silently switch the compiler's manifest grammar. New application configuration
code should use Toml. `tests/projects/std_regex_uuid_toml` is a real file-backed
consumer with strict typed port/UUID/route validation.

Upstream MIT licence, commit and original hashes are in `vendor/tomlc17/`.
The local patch replaces null-based member-address arithmetic in `page_create`
with `offsetof(page_t, data) + size`, preserving layout while passing UBSan.
The bridge validates UTF-8 without mutating tomlc17's global options.

Compiler boundary: [discarded owning enum payloads](../../../probes/gates/enum-payload-cleanup/README.md)
are not yet automatically destroyed. Ignoring a Result error (including a
temporary `is_err()` check) can retain its owned diagnostic String. Binding
and handling the error value runs its destructor. Parser/node-handle cleanup
tests passing does not close this compiler gap.

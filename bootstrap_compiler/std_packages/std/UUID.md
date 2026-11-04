# UUID values

`use std.uuid;` is provided by `std:std`, target `std`.

```vyx
use std.uuid;
let id = Uuid.try_new_v4().unwrap();
let text = id.toString(); // owned String, lowercase canonical form
let restored = Uuid.try_parse(text.as_str()).unwrap();
assert(restored == id);
```

`Uuid` is a Copy value containing two private network-order u64 halves.
`try_parse` requires exactly 36 ASCII bytes in `8-4-4-4-12` hex form; upper
and lowercase hex are accepted. Braces, URN prefixes, whitespace and compact
hex are not accepted. Parsing preserves the version and variant bits, including
nil/max UUIDs; it does not require version 4. `version()` reads the version
nibble, `variant_bits()` reads the top two bits (2 means the RFC `10` variant).

`try_from_bytes(&Vec<u8>)` requires exactly 16 bytes. `to_bytes()` uses network
order, rather than Windows GUID's mixed-endian memory layout. `toString()`
normalizes case. `nil`, `is_nil`, Clone, Eq, Hashable and Ord support values
as collection keys; ordering compares network-order bytes.

`try_new_v4()` fills 128 bits from the OS, then sets the version/variant bits:

- Windows: UCRT `rand_s`, backed by the operating system's secure random source.
- Linux, macOS and Android/Termux: `/dev/urandom`, accumulating short reads
  and retrying interrupted reads. Open, EOF, read and close failures are errors.

Errors are `InvalidLength`, `InvalidFormat` or `EntropyUnavailable`. There is
no shared mutable generator, clock seed, deterministic RNG fallback or
success value on entropy failure. `uuid4() -> string` is the legacy convenience
function; it panics if the OS entropy request fails. Browser/WASM entropy,
name-based UUIDs and time-ordered UUID generation are not supplied by this API.

The [contract gate](../../../probes/gates/stdlib-contracts/README.md) checks
conversion against Python's UUID implementation, collection semantics and
4096 generated values. `std_uuid_faults` overrides entropy only inside its test
executable to exercise failure/partial-read paths; the library has no test
provider override. Uniqueness in that sample is a smoke check, not a proof
about entropy quality.

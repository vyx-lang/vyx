# Standard-library contracts

Run from the repository root with a compiler built from the current source
and its matching backend/runtime:

```sh
python probes/gates/stdlib-contracts/run.py --compiler /path/to/vyxc --output .cache/stdlib-contracts --negative --model
```

The default runs every listed project at O0 and O2. `--projects` selects an
explicit comma-separated subset, and `--opt 0` selects one optimization level.
An empty or unknown selection fails. A timeout, missing success marker or
nonzero exit fails. Logs and `verification.json` retain commands, compiler and
companion hashes, package manifests, elapsed times and individual results.

The inventory checks manifest sources against the implicit registry. It does
not measure API coverage or establish correctness for modules a project never
calls. New package manifests are discovered automatically; behavioral coverage
must be added explicitly.

Projects cover:

| Project | Contract |
|---|---|
| `std_text_contracts` | bounded text views, embedded NUL equality/order/search/replacement |
| `std_ownership_contracts` | discarded Result/enum payloads, match expressions, guards, early returns, borrowed receivers and multiple payloads |
| `std_sync_contracts` | u8/i64/owning messages, ring wrap, full/empty/closed states, failed-send ownership, unread destruction, three concurrent producers and exactly-once receipt |
| `std_owned_collections` | Deque/List/Slice owning reads, clone, replacement, iteration, clear, reuse, repeated destruction and embedded-NUL StringView conversion |
| `std_value_collections` | Eq/Hashable/Ord value semantics, independent string storage, signed/unsigned/float order, owning sorting and a worst-case comparison bound |
| `std_iterator_contracts` | owned callback arguments/returns, borrowed callbacks, eager adapters, selected values, independent storage and unread buffer destruction |
| `std_priority_queue_contracts` | typed heap order, nonempty min/max rebuild, growth, Clone/transfer distinction, move-only owning values, comparison bounds, exactly-once cleanup and over-aligned storage |
| `std_ordered_contracts` | move-only owning map/set keys, borrowed queries, replacement/removal cleanup, sorted storage, independent result/collection clones and binary text keys |
| `std_testing_contracts` | bool/integer expectations and repeated mock queries that preserve their logs |
| `std_fs_contracts` | binary/text lengths, embedded NUL, explicit empty/error results, append/copy, invalid paths, literal directory names and close |
| `std_system_contracts` | target-specific drive/UNC/POSIX path roots, native clock IDs, independent steady-clock elapsed time across sleep and UTC timestamp |
| `std_time_faults` | native clock failure, invalid/overflow values, large QPC frequency, DWORD sleep chunks, POSIX EINTR continuation and permanent sleep failure |
| `std_sqlite_contracts` | real SQLite: owning bindings/results, embedded NUL and BLOB/NULL distinctions, typed/range/state errors, reset, finalization, deferred connection destruction, native allocation balance and persisted transactions |
| `std_scalar_contracts` | bit boundaries, negative hex, strict Base64/hex errors, all-byte round trips, independent PCG reference sequence and range extremes |
| `std_semver_contracts` | public cross-module API, strict/located SemVer errors, prerelease precedence, checked core/increments, owning Clone, Vec sorting, Dict key agreement and selection from a real release catalog |
| `std_url_contracts` | all-byte percent/form codecs, literal plus, IPv4/IPv6 and port validation, empty delimiters, repeated query fields, owning Clone and a cross-module request-preparation consumer |
| `std_ini_contracts` | indexed case-sensitive configuration, CRLF/BOM/root entries, located syntax/typed errors, i32/i64 limits, missing/empty distinction, owned String results, Clone and real file settings |
| `std_regex_uuid_toml` | file-backed router configuration across modules; PCRE2 captures/Unicode/bounds, UUID parsing/OS randomness/value keys, TOML types/tables/dates/lifetime; deterministic Python re/uuid/tomllib oracle |
| `std_uuid_faults` | injected entropy errors, partial provider failure; POSIX short reads, EINTR, EOF, read and close errors; no PRNG fallback |
| `std_generic_contracts` | a real static library with generic trait-bound methods and a separate consumer |
| `std_serde_contracts` | quoted multiline CSV, CRLF, blank/trailing fields, embedded NUL, escaped quotes and located malformed-input errors |
| Existing collections/JSON/miniz/I/O/umbrella projects | package resolution and resource/error contracts already maintained by those projects |

The PCG sequence is computed independently with unsigned 64-bit state,
multiplier 6364136223846793005, initial state zero, increment 85, one step,
addition of seed 42, and another step. Tests compare XSH-RR outputs with the
stored vector, rather than using a second call to the Vyx implementation as
the oracle.

`--negative` checks missing trait implementations, moved callback arguments,
iterator invalidation and checked swap bounds. Compiler rejection must contain
a semantic error and leave no executable; a backend failure does not count as
the intended rejection. Runtime checks compile first and must then fail with
the expected panic message.

`--model` generates 800 operations across i32/i64/u64/f64/string Vecs, including
growth, insertion, removal, replacement, cloning, lookup, clear, reverse and
sort. Expected lengths and contents come from Python lists. The report stores
the seed, generator hash and generated source hash; `--model-seed` reproduces
or changes a trace. This is reproducible model testing, not exhaustive language
or standard-library coverage.

Checks share a typed verification function and operation traces cross borrowed
function boundaries. `--model-only` runs just the generated projects.
`--model-block-size 128` keeps each type's full trace in one function for
compiler stress investigation; the default is 16. Neither option changes the
operations or the complete contents checked after each step.
Generated models have a separate 600-second compile/run budget
(`--model-timeout`); ordinary projects default to 180 seconds (`--timeout`).
The larger budget is a test limit, not a compiler performance claim. Compiler
phase timings should be measured separately; the initial monolithic traces
exceeded the ordinary budget on the development host.

Release builds on native Windows, Linux and macOS run this gate before
packaging and retain a checks/passed count and report hash in the SDK build
record. Failed runs upload the detailed reports with compiler diagnostics.
The runner checks source hashes before and after execution; editing sources
during a run invalidates that run. Timeouts terminate the owned fixture process
tree.
On Windows each invocation has a private Job Object; timeout and close terminate
its descendants without relying on WMI process-tree discovery. The runner's
unit checks verify prompt termination as well as seed reproducibility,
unchanged operations across function boundaries and duplicate source ownership.

Protocol interoperability remains in
[network-protocols](../network-protocols/README.md). GPU bindings require
their native backend and device. These checks do not certify those packages,
Android execution, all allocation-failure paths, or every public standard-library
API. CI must record each actually executed platform.

Selecting `std_semver_contracts` also generates and runs a SemVer model at each
selected optimization level. Its Python oracle uses a full-match ASCII grammar,
arbitrary-precision integers and tuple sorting, independent of the Vyx scanner.
It checks 80 valid round trips, more than 200 invalid versions and 192 comparison
pairs. The report retains its seed and generator/source hashes. Public convenience
methods also have negative panic fixtures under `--negative`; malformed input
and arithmetic overflow must not become a successfully parsed version.

Selecting `std_url_contracts` also runs a deterministic model with 80 byte
inputs (including all 256 byte values), 96 URL structures, 64 malformed escapes
and 32 repeated/empty query sets at each selected optimization level. Expected
encodings and components come from Python's `urllib.parse` and `ipaddress`.
The form oracle adjusts urllib's treatment of `*` and `~` to the form alphabet.
Reports retain the seed and generator/source hashes. This is a strict byte and
absolute HTTP/WS URL contract, not full WHATWG parsing or DNS validation.

Selecting `std_ini_contracts` also runs a deterministic model using Python's
`RawConfigParser` for the documented common subset. It checks 24 documents,
480 raw/typed lookups, strict syntax rejection and a 1024-entry growing index
under a 4096-byte section name. Section IDs keep index-key storage bounded.
Python integers provide independent width-bound results. Generated inputs mix
newlines, comments, empty values, case-sensitive keys, literal percent signs,
Unicode and conversion errors. Missing values remain distinct from empty
values and rejected values. Reports retain seed and generator/source hashes.

Selecting `std_regex_uuid_toml` also generates 96 Regex searches checked against
Python `re` (shared syntax only), 64 UUID conversions checked against `uuid`,
and eight TOML documents plus 15 invalid inputs checked against `tomllib` and
the signed-i64 bounds required by TOML. These run at both O0 and O2, separately
from hand-written feature and file-backed router tests. A 4096-ID uniqueness
smoke test is not a statistical or cryptographic proof of entropy quality.

Native ownership/failure checks are also available independently:

```sh
python probes/gates/stdlib-contracts/native_engines.py --cc /usr/bin/clang --output .cache/native-engines
```

This compiles pinned PCRE2 and tomlc17 ABI projections with ASan/UBSan (and LSan
where supported), repeatedly parses/frees native result storage and matches
capture buffers. The Vyx project tests validation, Ref-based child/clone
lifetimes and real native resource counters separately. The Regex fixture
also injects match-data/context allocation failures and requires zero live
scratch buffers after successful matches, no match, engine errors and both
partial-allocation failures. Release
Linux/macOS checks run this with the runner's system Clang. Windows tests use
the actual Vyx APIs and entropy failure executable; this sanitizer command is
POSIX-only. Corpus sampling does not claim full TOML/PCRE2 upstream conformance.

Native OpenSSL checks are opt-in because the default SDK gate does not install
OpenSSL. With the native link libraries installed, run:

```powershell
python probes/gates/stdlib-contracts/run.py --compiler /path/to/vyxc --projects std_priority_queue_contracts --native-crypto --native-bin E:/vcpkg/installed/x64-windows/bin --output .cache/stdlib-native-crypto
```

This adds `std_security_package`, which calls real OpenSSL against independent
hash/HMAC known answers (including NUL and UTF-8), and `std_crypto_faults`,
which injects allocation, digest, HMAC and RNG failures to verify error results
and exactly-once context cleanup. The injection fixture does not establish
cryptographic correctness; the real-library fixture provides that separate
evidence. On Windows the runner deploys the explicitly selected OpenSSL DLLs
beside a private executable and records their source paths and SHA-256 hashes.
Each Windows invocation gets a separate deployment directory. On POSIX configure
the native loader search path for the chosen OpenSSL installation.

SQLite is a required native input to the default gate. Install `libsqlite3-dev`
on Linux; macOS uses system SQLite. On Windows install `sqlite3:x64-windows`, set
`VCPKG_ROOT`, and pass `--native-bin <vcpkg-root>/installed/x64-windows/bin`.
The runner copies exactly `sqlite3.dll` beside a private executable and records
its identity; a missing explicit DLL input fails instead of searching PATH.
The package remains opt-in for applications and does not enter the std umbrella.

For example, with vcpkg at `E:/vcpkg`:

```powershell
$env:VCPKG_ROOT = 'E:/vcpkg'
python probes/gates/stdlib-contracts/run.py --compiler /path/to/vyxc.exe --native-bin E:/vcpkg/installed/x64-windows/bin --negative --model
```

Time failure injection runs separately from the real-clock consumer. POSIX
injects native clock/sleep functions; Windows replaces only the test process's
QPC/Sleep import slots and restores their page protection. It does not alter an
OS DLL, SDK artifact or compiler. This tests failure handling, while the separate
steady-clock consumer tests elapsed time against the platform implementation.

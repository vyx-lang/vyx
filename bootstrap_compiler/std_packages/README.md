# Bootstrap standard packages

[Documentation](../../docs/README.md) · [Standard-library reference](../../docs/STD_LIBRARY.md) · [标准库参考](../../docs/STD_LIBRARY.zh-CN.md)

This directory is the public package surface for the self-host compiler.
Consumers reference packages with `std:<name>` and select the target declared
by that package manifest.

Pure/generic modules use `type = "source"`: the package exposes explicit module
sources to the consuming compiler process and emits no archive. Native bridges
such as `json`, `miniz`, `regex` and `serde_toml` use normal static targets.

Each package owns its canonical sources below its own `src/` directory. The
`std` umbrella target composes the segmented packages through normal manifest
dependencies instead of flattening their files into one target. Bindings that
require independently supplied native SDKs (LLVM, Cacao, curl, OpenSSL,
SQLite, stb and platform-only Win32 modules) are intentionally not smuggled
into the umbrella link closure; they remain explicit opt-in integrations.
The current opt-in native packages are `std:cacao`, `std:dll`, `std:llvm`,
`std:logger`, `std:network`, `std:security`, `std:sqlite`, `std:stb_image`
and `std:win32`. `std:network` aggregates the curl-based web stack
(`std.network.curl` / `httpclient` / `websockets` / `grpc`); `std:security`
is the OpenSSL-based stack plus pure-Vyx hashes (`std.security.openssl` /
`std.security.crypto`).
`std:logger` is the home of `std.log`: a `[build] prebuild = ["vyx: …"]`
script clones spdlog, cmake-builds `libspdlog.a`, and runs `vyxc dci adapter`
to write the `.dcib`. It is not listed in `registry` (implicit `--src=file`
must not pull a C++ link closure).

`--src=file` does not scan `../std/`. The `registry` file in this directory
forwards implicit `std.*` lookup to the `.vyx` files declared by each package
manifest. `vyxc build` still consumes packages through `std:` dependencies and
`--module-sources-file`; it does not double-load this registry.

The flat standard-library snapshots have been removed. Source changes,
package discovery and SDK distribution all use this directory.

Regular expressions use a [vendored PCRE2 package](regex/README.md). TOML
has a [typed, immutable document API](serde/TOML.md). [`std.uuid.Uuid`](std/UUID.md) uses OS
entropy (`rand_s` on Windows, `/dev/urandom` on POSIX), reports failures through
`try_new_v4`, and supports canonical parsing, network-order bytes and collection
value semantics. `uuid4()` remains the panicking string convenience API;
there is no clock/PRNG fallback or shared mutable UUID generator state.

## Contracts and errors

Run the [standard-library project gate](../../probes/gates/stdlib-contracts/README.md)
with the compiler and runtime being released. It exercises ownership, generic
element widths, errors and concurrency at O0/O2. Package discovery is not API
coverage; the gate lists its actual scope and excludes device-dependent tests.

`Channel<T>` is a bounded nonblocking queue. `try_send(value)` returns the
rejected owning value in `ChannelSendError<T>`; use `get_ref()` to inspect it
and `into_inner()` to recover it once. `send(value)` consumes the value on
either outcome. `try_recv()` distinguishes `Empty` from `Closed`, while
`recv()` returns `Option<T>`. Closing permits draining queued values. Join all
users before destruction; queued values are then destroyed once. The old
i64 function surface retains its ambiguous empty-as-zero receive behavior.

`File.try_open`, `try_read_all_bytes` and `try_read_all_text` return `Result`.
An empty file is a successful empty value. Legacy read convenience methods
retain empty-on-error behavior for compatibility; use the Result methods when
the distinction matters. File owns its handle, `close()` is idempotent, and
scope cleanup closes it. Writes report short writes and close failures. Native
directory functions treat path characters literally, without shell evaluation.

`try_base64_decode` and `try_hex_decode` report malformed input. Their
non-Result counterparts panic on malformed input. Base64 uses the standard
alphabet, required padding and canonical pad bits; whitespace and URL-safe
variants require a separate API. Binary results preserve embedded zero bytes.
`hex_int`/`hex_i64` format negative values as the full-width two's-complement
bit pattern. `Rng` is a deterministic PCG generator, not cryptographic entropy;
floating samples are in [0, 1), and bounded integer sampling uses rejection.

`PriorityQueue<T>` compares borrowed elements through Ord and transfers the
root on pop. Peek and collection clone require Clone; clearing destroys every
live element. Min/max setters return a mutable borrow and rebuild nonempty
heaps. `BTreeMap` / `BTreeSet` queries borrow keys, including move-only keys;
their current implementation uses sorted flat arrays, not node-based trees.

OpenSSL digest/HMAC/RNG wrappers have Result-returning `try_*` variants carrying
`CryptoError`. The existing text helpers panic on backend failure. Hash and
HMAC output is hex text; random byte output is also hex, with a 0..256-byte
input range. The [contract gate](../../probes/gates/stdlib-contracts/README.md)
documents opt-in real OpenSSL known-answer and native failure-injection checks.

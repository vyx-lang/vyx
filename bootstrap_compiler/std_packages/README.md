# Bootstrap standard packages

[Documentation](../../docs/README.md) · [Standard-library reference](../../docs/STD_LIBRARY.md) · [标准库参考](../../docs/STD_LIBRARY.zh-CN.md)

This directory is the public package surface for the self-host compiler.
Consumers reference packages with `std:<name>` and select the target declared
by that package manifest.

Pure/generic modules use `type = "source"`: the package exposes explicit module
sources to the consuming compiler process and emits no archive. Native bridges
such as `json` and `miniz` use normal static targets.

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

`../std/` is a leftover seed snapshot, not a consumer import contract. The root
repository `std/` is the frozen host mirror and is intentionally untouched.

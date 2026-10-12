# Network protocol regression

This gate builds `tests/projects/std_network_protocols` through the project
pipeline with `std:network`, then runs the AOT executable against local services.
It uses grpcio for real HTTP/2 gRPC, websockets for RFC 6455, Python's HTTP server,
and h2/raw socket servers for malformed responses. No public service is required.

Install Python dependencies in your chosen environment:

```sh
python -m pip install grpcio h2 websockets cryptography
```

Use a compiler built from the current source tree and its matching runtime:

```powershell
python probes/gates/network-protocols/run.py `
  --compiler bootstrap_compiler/out/vyxc.exe `
  --native-bin E:/vcpkg/installed/x64-windows/bin
```

The native libcurl must include HTTP/2 support. The Windows WSS regression
uses an OpenSSL-enabled libcurl (`curl[core,http2,openssl]` in vcpkg).
Other TLS backends must support raw TLS reads/writes through `CONNECT_ONLY`;
successful HTTPS requests alone do not verify that API.
`--native-bin` is repeatable and searched in the supplied order. On Windows
the gate runs a private executable copy with those DLLs, so old libraries
deployed to the project's `target/` cannot override the selected dependencies.
`--python-packages <directory>` permits an isolated Python package installation.
`--opt 0` or `--opt 2` selects one optimization level; the default runs both.
`--no-build` only reuses the most recently built consumer for investigation.

The executable checks HTTP GET, NUL-containing POST, empty POST and HTTP 404;
gRPC binary round trips, repeated/empty calls, server error trailers,
deadlines, closed/unavailable channels, missing status, invalid lengths,
unsupported compression and wrong content types; WebSocket echo, NUL, 64-bit
frame lengths, ping/pong, fragmented text, peer close, forged accept headers,
masked server frames, oversized payloads, invalid UTF-8 and reserved close
status codes. WSS must reject the
temporary untrusted certificate and accept it with an explicit CA file.
Server-side observations verify actual requests and ping replies.

`primitive_member_collision.vyx` separately reproduces a builtin `toString`
collision with user methods. Compile and execute it at `-O0` and `-O2`.

Logs, hashes and results are written under local `.cache/<run>/`. This gate does
not certify streaming gRPC, WebSocket binary/extensions, concurrency, or a
platform that was not executed.

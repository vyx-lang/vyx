# Network package

Add the source package to `Vyx.toml`:

```toml
[dependencies]
network = { path = "std:network", target = "std_network" }
```

The package exports `std.network.curl`, `std.network.httpclient`,
`std.network.websockets`, and `std.network.grpc`. Its manifest propagates
libcurl and OpenSSL link dependencies to consuming targets. Install the native
development libraries for the target platform; deploy their shared libraries
beside the executable or in the platform loader's search path.

## HTTP

```vyx
use std.network.httpclient;

var client = HttpClient.create();
client.setTimeout(5);
let response = client.postJson("https://example.org/api", "{}");
if (!response.ok) { print("HTTP transport failed"); }
print(response.statusCode);
```

`ok` indicates transport success; inspect `statusCode` for HTTP errors.
Request and response lengths are explicit, so embedded NUL bytes are preserved.
Empty `postJson` requests still use POST.

## WebSocket

```vyx
use std.network.websockets;

var socket = WebSocket.connect("wss://example.org/events");
if (socket.isOpen()) {
    socket.send("hello");
    print(socket.recv());
    socket.close();
} else {
    print(socket.lastError());
}
```

TCP, DNS, proxies and TLS use libcurl's connection layer. WSS retains
certificate and hostname verification. A private CA can be supplied as the
second `connect` argument; there is no option to disable verification.
The TLS backend must support libcurl's `CONNECT_ONLY` send/receive API.
The Windows regression uses a libcurl built with HTTP/2 and OpenSSL
(`curl[core,http2,openssl]`). Its raw TLS transport is tested separately from
certificate rejection and acceptance; a different TLS backend requires the
same checks.
The Vyx layer performs the RFC 6455 handshake, masks client frames with
OpenSSL randomness, assembles fragmented text messages and handles ping/pong
and close. Text must contain valid UTF-8. Binary messages, subprotocols and
extensions are rejected. The maximum message size is 16 MiB.

The API is synchronous. Reads and writes wait at most ten seconds for each
raw I/O operation. `recv()` returns an empty string for an empty message or
connection closure; inspect `isOpen()` and `lastError()` to distinguish them.
`close()` is idempotent; scope cleanup releases the transport even after
handshake or frame errors.

## gRPC

```vyx
use std.network.grpc;

var channel = GrpcChannel.connect("https://example.org:443");
channel.setTimeout(5);
if (channel.isConnected()) {
    // serialized_request contains bytes encoded by the application's schema.
    let response = channel.unaryCall("/acme.Service/Method", serialized_request);
    if (response.ok) { consume_response_bytes(response.data); }
    else { print(response.status); print(response.message); }
}
channel.close();
```

libcurl **must be built with HTTP/2**, for example
`vcpkg install curl[http2]:x64-windows`. A libcurl without this capability
returns a transport error; calls never fall back to HTTP/1.1. Bare `host:port`
selects plaintext HTTP/2. HTTPS uses certificate and hostname verification;
the optional second `connect` argument selects a private CA file.

`connect` establishes a transport; `isConnected()` records successful setup,
not a live probe of the remote service. `unaryCall` sends a length-prefixed
binary message, requires HTTP/2 and `application/grpc`, validates the response
length and reads `grpc-status` / percent-decoded `grpc-message` from trailers.
The response has `status`, `data`, `ok`, and `message`. A non-OK status,
deadline, absent status or malformed frame produces `ok=false`.

Only uncompressed unary RPC is implemented. Requests and responses are
limited to 16 MiB. Protocol Buffers code generation, streaming RPC, compression
and custom metadata submission are not provided by this module. `GrpcMetadata`
is a data record, not an implemented transport option. `close()` is idempotent
and scope cleanup closes the channel.

Wire formats follow the [gRPC HTTP/2 protocol](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md)
and [WebSocket protocol](https://www.rfc-editor.org/rfc/rfc6455).
The transport uses libcurl's [CONNECT_ONLY](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html),
[easy_send](https://curl.se/libcurl/c/curl_easy_send.html) and
[easy_recv](https://curl.se/libcurl/c/curl_easy_recv.html) APIs.

## Regression

[Network protocol gate](../../../probes/gates/network-protocols/README.md)
builds the real package consumer and runs it against grpcio and websockets
servers, with separate malformed-wire fixtures. It records compiler/native
library hashes and requires successful AOT execution.

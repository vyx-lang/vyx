"""AOT network package against grpcio, websockets and hostile wire responses."""
from __future__ import annotations

import argparse
import base64
import concurrent.futures
import contextlib
import datetime
import hashlib
import http.server
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / "tests/projects/std_network_protocols"


class TCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def background(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


class HTTP(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.observed.append(("GET", self.path))
        self.send_response(200 if self.path == "/hello" else 404)
        self.end_headers()
        self.wfile.write(b"hello from HTTP" if self.path == "/hello" else b"missing")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.server.observed.append(("POST", self.path, body.hex()))
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"POST" if self.path == "/empty" else body)


class RawWS(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(5)
        try:
            data = b""
            while b"\r\n\r\n" not in data and len(data) < 16384:
                chunk = self.request.recv(4096)
                if not chunk:
                    return
                data += chunk
            lines = data.decode("ascii").split("\r\n")
            path = lines[0].split()[1]
            headers = dict(line.split(": ", 1) for line in lines[1:] if ": " in line)
            accept = base64.b64encode(hashlib.sha1(
                (headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
            ).digest()).decode()
            self.server.observed.append(path)
            if path == "/wrong-accept":
                reply = f"Sec-WebSocket-Accept: wrong\r\nX-Forged: {accept}\r\n"
            else:
                reply = f"Sec-WebSocket-Accept: {accept}\r\n"
            self.request.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\n"
                                  "Connection: keep-alive, Upgrade\r\n" + reply + "\r\n").encode())
            if path == "/masked":
                self.request.sendall(b"\x81\x81abcd\x00")
            elif path == "/huge":
                self.request.sendall(b"\x81\x7f" + struct.pack("!Q", 16777217))
            elif path == "/bad-utf8":
                self.request.sendall(b"\x81\x02\xc0\xaf")
            elif path == "/bad-close":
                self.request.sendall(b"\x88\x02\x03\xed")  # 1005 is not a wire status
        except (OSError, ValueError, KeyError):
            pass


class MalformedH2(socketserver.BaseRequestHandler):
    def handle(self):
        import h2.config
        import h2.connection
        import h2.events

        self.request.settimeout(5)
        conn = h2.connection.H2Connection(h2.config.H2Configuration(client_side=False))
        conn.initiate_connection()
        paths = {}
        try:
            self.request.sendall(conn.data_to_send())
            while True:
                data = self.request.recv(65536)
                if not data:
                    return
                for event in conn.receive_data(data):
                    if isinstance(event, h2.events.RequestReceived):
                        paths[event.stream_id] = dict(event.headers)[b":path"].decode()
                    elif isinstance(event, h2.events.DataReceived):
                        conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                    elif isinstance(event, h2.events.StreamEnded):
                        path = paths[event.stream_id]
                        self.server.observed.append(path)
                        content = "application/grpc-bogus" if path.endswith("BadType") else "application/grpc"
                        conn.send_headers(event.stream_id, [(":status", "200"), ("content-type", content)])
                        body = b"\x00\x00\x00\x00\x01x"
                        if path.endswith("BadFrame"):
                            body = b"\x00\x00\x00\x00\x05x"
                        if path.endswith("Compressed"):
                            body = b"\x01\x00\x00\x00\x01x"
                        conn.send_data(event.stream_id, body, end_stream=path.endswith("NoStatus"))
                        if not path.endswith("NoStatus"):
                            conn.send_headers(event.stream_id, [("grpc-status", "0")], end_stream=True)
                    elif isinstance(event, h2.events.ConnectionTerminated):
                        return
                self.request.sendall(conn.data_to_send())
        except (OSError, h2.exceptions.H2Error):
            pass


@contextlib.contextmanager
def servers(cache):
    import grpc
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    from websockets.sync.server import serve

    observed = {key: [] for key in ["http", "grpc", "ws", "h2", "raw_ws"]}
    http_server = background(http.server.ThreadingHTTPServer(("127.0.0.1", 0), HTTP))
    http_server.observed = observed["http"]
    raw_ws = TCPServer(("127.0.0.1", 0), RawWS)
    raw_ws.observed = observed["raw_ws"]
    background(raw_ws)
    h2_server = TCPServer(("127.0.0.1", 0), MalformedH2)
    h2_server.observed = observed["h2"]
    background(h2_server)
    grpc_server = grpc.server(concurrent.futures.ThreadPoolExecutor(max_workers=4))

    def echo(data, ctx):
        observed["grpc"].append(("Echo", data.hex()))
        return data

    def denied(data, ctx):
        observed["grpc"].append(("Denied", data.hex()))
        ctx.abort(grpc.StatusCode.PERMISSION_DENIED, "denied by server")

    def slow(data, ctx):
        observed["grpc"].append(("Slow", data.hex()))
        time.sleep(1.5)
        return data

    grpc_server.add_generic_rpc_handlers((grpc.method_handlers_generic_handler("vyx.Echo", {
        name: grpc.unary_unary_rpc_method_handler(fn, request_deserializer=lambda x: x,
                                                   response_serializer=lambda x: x)
        for name, fn in [("Echo", echo), ("Denied", denied), ("Slow", slow)]
    }),))
    grpc_port = grpc_server.add_insecure_port("127.0.0.1:0")
    grpc_server.start()

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=1))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost"),
                           x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), critical=False)
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .sign(key, hashes.SHA256()))
    cert_path, key_path = cache / "localhost.pem", cache / "localhost.key"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                          serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption()))
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert_path, key_path)

    def websocket(peer):
        path = peer.request.path
        observed["ws"].append(path)
        if path == "/fragmented":
            pong = peer.ping(b"probe")
            peer.send(["fragmented ", "text"])
            if not pong.wait(5):
                observed["ws"].append("MISSING_PONG")
        elif path == "/close":
            peer.close(1000, "done")
        else:
            try:
                for message in peer:
                    peer.send(message)
            except Exception:
                pass

    ws = serve(websocket, "127.0.0.1", 0, compression=None, max_size=16777216)
    wss = serve(websocket, "127.0.0.1", 0, ssl=tls, compression=None, max_size=16777216)
    background(ws)
    background(wss)
    absent = socket.socket()
    absent.bind(("127.0.0.1", 0))  # reserved but deliberately not listening
    env = {
        "NETWORK_HTTP": f"http://127.0.0.1:{http_server.server_port}",
        "NETWORK_GRPC": f"127.0.0.1:{grpc_port}",
        "NETWORK_H2": f"127.0.0.1:{h2_server.server_address[1]}",
        "NETWORK_WS": f"ws://127.0.0.1:{ws.socket.getsockname()[1]}",
        "NETWORK_RAW_WS": f"ws://127.0.0.1:{raw_ws.server_address[1]}",
        "NETWORK_TLS_WS": f"wss://127.0.0.1:{wss.socket.getsockname()[1]}",
        "NETWORK_CA": str(cert_path),
        "NETWORK_ABSENT": f"127.0.0.1:{absent.getsockname()[1]}",
    }
    try:
        yield env, observed
    finally:
        absent.close()
        grpc_server.stop(0).wait()
        ws.shutdown()
        wss.shutdown()
        for server in [http_server, raw_ws, h2_server]:
            server.shutdown()
            server.server_close()


def identity(path):
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--native-bin", type=Path, action="append", default=[])
    parser.add_argument("--python-packages", type=Path)
    parser.add_argument("--opt", type=int, choices=[0, 2], action="append")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    if args.python_packages:
        sys.path.insert(0, str(args.python_packages.resolve()))
    import grpc
    import h2
    import websockets

    compiler = args.compiler.resolve()
    run_dir = Path(__file__).resolve().parent / ".cache" / time.strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["LLVM_ROOT"] = str(ROOT / "clang") if os.name == "nt" else env.get("LLVM_ROOT", "/usr/lib/llvm-22")
    env["PATH"] = os.pathsep.join([str(p.resolve()) for p in args.native_bin] + [env["PATH"]])
    records = []
    for opt in args.opt or [0, 2]:
        command = [str(compiler), "--src=project", str(PROJECT), "--emit=exe", f"-O{opt}", "-j2"]
        if not args.no_build:
            build = subprocess.run(command, cwd=PROJECT, env=env, capture_output=True, timeout=600)
            (run_dir / f"build-O{opt}.log").write_bytes(build.stdout + build.stderr)
            if build.returncode:
                print((build.stdout + build.stderr).decode(errors="replace")[-6000:])
                raise RuntimeError(f"project build failed: {build.returncode}")
        with servers(run_dir) as (server_env, observed):
            executable = PROJECT / "target" / ("std_network_protocols.exe" if os.name == "nt" else "std_network_protocols")
            runtime_libraries = []
            # Windows loads executable-local DLLs before PATH. Run in a private
            # directory so a previous project deployment cannot shadow --native-bin.
            if os.name == "nt":
                isolated = run_dir / f"O{opt}"
                isolated.mkdir()
                shutil.copy2(executable, isolated / executable.name)
                for name in ["libcurl.dll", "nghttp2.dll", "zlib1.dll",
                             "libcrypto-3-x64.dll", "libssl-3-x64.dll"]:
                    for directory in args.native_bin:
                        source = directory / name
                        if source.is_file():
                            shutil.copy2(source, isolated / name)
                            runtime_libraries.append(identity(isolated / name))
                            break
                executable = isolated / executable.name
            result = subprocess.run([str(executable)], cwd=PROJECT, env=env | server_env,
                                    capture_output=True, timeout=90)
            output = result.stdout.decode(errors="replace")
            (run_dir / f"run-O{opt}.log").write_bytes(result.stdout + result.stderr)
            expected = ["HTTP GET / binary POST / empty POST / 404 ok",
                        "gRPC HTTP/2 binary / repeat / empty / trailers / deadline / malformed / close ok",
                        "WebSocket echo / NUL / large / ping / fragments / close / malformed / TLS rejection ok",
                        "network protocols ok"]
            passed = result.returncode == 0 and all(s in output for s in expected)
            passed = passed and ("POST", "/echo", "410042") in observed["http"]
            passed = passed and ("Echo", "410042") in observed["grpc"]
            passed = passed and "MISSING_PONG" not in observed["ws"]
            records.append({"opt": opt, "build_command": command, "exit": result.returncode,
                            "executable": identity(executable), "runtime_libraries": runtime_libraries,
                            "stdout": output, "stderr": result.stderr.decode(errors="replace"),
                            "observed": observed, "pass": passed})
            print(f"O{opt}: {'PASS' if passed else 'FAIL'}\n{output}", flush=True)
    report = {"compiler": identity(compiler), "packages": {"grpcio": grpc.__version__,
              "h2": h2.__version__, "websockets": websockets.__version__},
              "native_candidates": [identity(p / name) for p in args.native_bin
                              for name in ["libcurl.dll", "nghttp2.dll"] if (p / name).exists()],
              "records": records}
    (run_dir / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(run_dir / "results.json")
    return 0 if all(x["pass"] for x in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())

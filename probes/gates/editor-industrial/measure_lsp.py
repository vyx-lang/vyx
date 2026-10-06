"""Measure the real SDK compiler workspace through the LSP wire protocol.

Requires psutil. Results are measurements of this executable, not compiler gates.
"""
import argparse
import collections
import json
import pathlib
import queue
import subprocess
import threading
import time

import psutil


class Client:
    def __init__(self, exe, cwd):
        self.p = subprocess.Popen([str(exe), "--stdio"], cwd=cwd,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        self.messages = queue.Queue()
        self.stderr = collections.deque(maxlen=100)
        threading.Thread(target=self.read, daemon=True).start()
        threading.Thread(target=self.read_err, daemon=True).start()

    def read_err(self):
        for line in self.p.stderr:
            self.stderr.append(line.decode("utf-8", errors="replace"))

    def read(self):
        try:
            while True:
                length = None
                headers = []
                while True:
                    line = self.p.stdout.readline()
                    if len(headers) < 4:
                        headers.append(repr(line[:120]))
                    if not line:
                        raise EOFError("server stdout closed")
                    if line in (b"\r\n", b"\n"):
                        break
                    if line.lower().startswith(b"content-length:"):
                        length = int(line.split(b":", 1)[1])
                if length is None or length > 64 * 1024 * 1024:
                    raise RuntimeError(f"invalid protocol frame length={length!r}; headers={headers}")
                payload = self.p.stdout.read(length)
                if len(payload) != length:
                    raise EOFError("short frame")
                self.messages.put(json.loads(payload))
        except BaseException as exc:
            self.messages.put(exc)

    def send(self, method, params=None, ident=None):
        obj = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            obj["params"] = params
        if ident is not None:
            obj["id"] = ident
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.p.stdin.flush()

    def wait(self, predicate, timeout, limit_mb, samples):
        deadline = time.monotonic() + timeout
        proc = psutil.Process(self.p.pid)
        while time.monotonic() < deadline:
            try:
                info = proc.memory_info()
                mb = getattr(info, "private", info.rss) / 1024**2
                samples.append(mb)
                if mb > limit_mb:
                    raise MemoryError(f"private memory exceeded {limit_mb} MiB ({mb:.1f})")
            except psutil.NoSuchProcess:
                raise RuntimeError("server exited")
            try:
                item = self.messages.get(timeout=0.05)
            except queue.Empty:
                continue
            if isinstance(item, BaseException):
                raise item
            if predicate(item):
                if item.get("error"):
                    raise RuntimeError(item["error"])
                return item
        raise TimeoutError("protocol request timed out")

    def close(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait(timeout=10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lsp", type=pathlib.Path, required=True)
    ap.add_argument("--workspace", type=pathlib.Path, required=True)
    ap.add_argument("--document", type=pathlib.Path)
    ap.add_argument("--iterations", type=int, default=100)
    ap.add_argument("--query", default="Sema")
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--limit-mb", type=float, default=1536)
    ap.add_argument("--growth-mb", type=float, default=64)
    ap.add_argument("--semantic-tokens", action="store_true")
    ap.add_argument("--burst", type=int, default=0, help="Send queued edits without waiting for diagnostics.")
    ap.add_argument("--result", type=pathlib.Path, required=True)
    args = ap.parse_args()
    assert args.iterations > 0, "iterations must be positive"
    workspace = args.workspace.resolve()
    client = Client(args.lsp.resolve(), workspace)
    samples = []
    result = {"lsp": str(args.lsp.resolve()), "workspace": str(workspace),
              "iterations": args.iterations, "status": "failed"}
    start = time.monotonic()
    try:
        wait = lambda pred: client.wait(pred, args.timeout, args.limit_mb, samples)
        client.send("initialize", {"rootUri": workspace.as_uri()}, 1)
        wait(lambda m: m.get("id") == 1)
        result["initialize_seconds"] = time.monotonic() - start
        result["initialized_mb"] = samples[-1]
        client.send("initialized", {})
        # Query symbols from the actual project; a source-free initialize is not success.
        client.send("workspace/symbol", {"query": args.query}, 2)
        symbols = wait(lambda m: m.get("id") == 2)["result"]
        result["workspace_symbols"] = len(symbols)
        assert symbols, "workspace source index missing"
        if args.document:
            doc = args.document.resolve()
            text = doc.read_text(encoding="utf-8-sig")
            uri = doc.as_uri()
            client.send("textDocument/didOpen", {"textDocument": {
                "uri": uri, "languageId": "vyx", "version": 1, "text": text}})
            wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                 and m.get("params", {}).get("uri") == uri)
            result["opened_mb"] = samples[-1]
            if args.semantic_tokens:
                tick = time.monotonic()
                client.send("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}}, "tokens")
                data = wait(lambda m: m.get("id") == "tokens")["result"]["data"]
                assert data and len(data) % 5 == 0, "invalid semantic token response"
                result["semantic_tokens_seconds"] = time.monotonic() - tick
                result["semantic_tokens"] = len(data) // 5
            timings = []
            result["edit_memory_mb"] = []
            warm_mb = None
            for i in range(args.iterations):
                tick = time.monotonic()
                client.send("textDocument/didChange", {"textDocument": {
                    "uri": uri, "version": i + 2}, "contentChanges": [
                        {"text": text + f"\n// editor revision {i}\n"}]})
                wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                     and m.get("params", {}).get("uri") == uri)
                client.send("textDocument/documentSymbol", {"textDocument": {"uri": uri}}, i + 10)
                answer = wait(lambda m: m.get("id") == i + 10)
                assert answer["result"], "document index lost after edit"
                timings.append(time.monotonic() - tick)
                result["edit_memory_mb"].append(samples[-1])
                if i == min(9, args.iterations - 1):
                    warm_mb = samples[-1]
            result.update(edit_max_seconds=max(timings), edit_mean_seconds=sum(timings) / len(timings),
                          warm_mb=warm_mb, edited_mb=samples[-1],
                          edit_growth_mb=samples[-1] - warm_mb)
            assert result["edit_growth_mb"] <= args.growth_mb, "memory grows with edits"
            if args.burst:
                tick = time.monotonic()
                for i in range(args.burst):
                    client.send("textDocument/didChange", {"textDocument": {
                        "uri": uri, "version": args.iterations + i + 2}, "contentChanges": [
                            {"text": text + f"\nfn editor_revision_{i}() -> i32 {{ return {i}; }}\n"}]})
                # A request is a synchronization barrier and must see the final edit.
                client.send("textDocument/documentSymbol", {"textDocument": {"uri": uri}}, "burst")
                burst_symbols = wait(lambda m: m.get("id") == "burst")["result"]
                names = {s["name"] for s in burst_symbols}
                assert f"editor_revision_{args.burst - 1}" in names, "request did not see final edit"
                assert args.burst == 1 or "editor_revision_0" not in names, "old index survived edit"
                result["burst_edits"] = args.burst
                result["burst_seconds"] = time.monotonic() - tick
                assert result["burst_seconds"] < min(args.timeout, 20), "queued edits block editor requests"
            client.send("textDocument/didClose", {"textDocument": {"uri": uri}})
        client.send("shutdown", {}, "shutdown")
        wait(lambda m: m.get("id") == "shutdown")
        client.send("exit")
        client.p.wait(timeout=15)
        assert client.p.returncode == 0
        result["status"] = "passed"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        result.update(peak_private_mb=max(samples, default=0), elapsed_seconds=time.monotonic() - start,
                      stderr="".join(client.stderr))
        client.close()
        args.result.parent.mkdir(parents=True, exist_ok=True)
        args.result.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps({k: v for k, v in result.items() if k not in ("stderr", "edit_memory_mb")}, ensure_ascii=False))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())

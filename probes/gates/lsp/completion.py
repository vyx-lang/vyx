#!/usr/bin/env python3
"""Probe vyxc-lsp use-path package completion (prefix match + isIncomplete)."""
import json
import os
import subprocess
import sys


def frame(body: str) -> bytes:
    data = body.encode()
    return f"Content-Length: {len(data)}\r\n\r\n".encode() + data


def read_msg(proc):
    headers = {}
    while True:
        line = proc.stdout.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        k, _, v = line.decode(errors="replace").partition(":")
        headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    if n <= 0:
        return None
    data = proc.stdout.read(n)
    return json.loads(data.decode())


def labels_of(msg):
    result = msg.get("result") or {}
    items = result.get("items") or []
    return result.get("isIncomplete"), [it.get("label", "") for it in items]


def main() -> int:
    lsp = sys.argv[1]
    env = os.environ.copy()
    proc = subprocess.Popen(
        [lsp, "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    assert proc.stdin and proc.stdout
    uri = "file:///tmp/pkg_probe.vyx"
    src_dot = "use std.\n"
    src_c = "use std.c\n"
    init = json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {"capabilities": {}, "rootUri": None, "processId": None},
    })
    did = json.dumps({
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": uri,
                "languageId": "vyx",
                "version": 1,
                "text": src_dot,
            }
        },
    })
    comp_dot = json.dumps({
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/completion",
        "params": {
            "textDocument": {"uri": uri},
            "position": {"line": 0, "character": len("use std.")},
        },
    })
    change = json.dumps({
        "jsonrpc": "2.0",
        "method": "textDocument/didChange",
        "params": {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{"text": src_c}],
        },
    })
    comp_c = json.dumps({
        "jsonrpc": "2.0",
        "id": 3,
        "method": "textDocument/completion",
        "params": {
            "textDocument": {"uri": uri},
            "position": {"line": 0, "character": len("use std.c")},
        },
    })
    proc.stdin.write(
        frame(init) + frame(did) + frame(comp_dot) + frame(change) + frame(comp_c)
    )
    proc.stdin.flush()

    init_caps = None
    dot_msg = None
    c_msg = None
    for _ in range(16):
        msg = read_msg(proc)
        if msg is None:
            break
        if msg.get("id") == 1:
            init_caps = (msg.get("result") or {}).get("capabilities", {})
        elif msg.get("id") == 2:
            dot_msg = msg
        elif msg.get("id") == 3:
            c_msg = msg
        if init_caps is not None and dot_msg is not None and c_msg is not None:
            break
    proc.kill()

    triggers = ((init_caps or {}).get("completionProvider") or {}).get("triggerCharacters") or []
    if "." not in triggers:
        print("FAIL: initialize missing '.' triggerCharacter")
        print(init_caps)
        return 1

    incomplete, labels = labels_of(dot_msg or {})
    if not incomplete:
        print("FAIL: use std. completion isIncomplete should be true")
        print(dot_msg)
        return 1
    need = ("std.collections", "std.core", "std.fs")
    missing = [n for n in need if not any(l == n or l.startswith(n + ".") for l in labels)]
    if missing:
        print("FAIL: use std. missing packages", missing)
        print("labels=", labels[:40], "count=", len(labels))
        return 1
    if any(l.startswith("bootstrap.") for l in labels):
        print("FAIL: use std. leaked non-std prefix", [l for l in labels if l.startswith("bootstrap.")][:8])
        return 1

    incomplete_c, labels_c = labels_of(c_msg or {})
    if not incomplete_c:
        print("FAIL: use std.c completion isIncomplete should be true")
        print(c_msg)
        return 1
    if "std.collections" not in labels_c or "std.core" not in labels_c:
        print("FAIL: use std.c should keep collections/core")
        print(labels_c[:40])
        return 1
    if "std.fs" in labels_c:
        print("FAIL: use std.c should drop std.fs (prefix mismatch)")
        print(labels_c[:40])
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: completion.py <vyxc-lsp>")
        sys.exit(2)
    sys.exit(main())

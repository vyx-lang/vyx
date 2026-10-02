#!/usr/bin/env python3
"""Probe vyxc-lsp documentFormatting + initialize capabilities."""
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
    init = json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {"capabilities": {}, "rootUri": None, "processId": None},
    })
    messy = "fn main() -> i32 {\nlet x = 1;\nif (x > 0) {\nreturn x;\n}\nreturn 0;\n}\n"
    did = json.dumps({
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": "file:///tmp/fmt_probe.vyx",
                "languageId": "vyx",
                "version": 1,
                "text": messy,
            }
        },
    })
    fmt = json.dumps({
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/formatting",
        "params": {
            "textDocument": {"uri": "file:///tmp/fmt_probe.vyx"},
            "options": {"tabSize": 4, "insertSpaces": True},
        },
    })
    proc.stdin.write(frame(init) + frame(did) + frame(fmt))
    proc.stdin.flush()

    caps = None
    edits = None
    for _ in range(12):
        msg = read_msg(proc)
        if msg is None:
            break
        if msg.get("id") == 1:
            caps = msg.get("result", {}).get("capabilities", {})
        if msg.get("id") == 2:
            edits = msg.get("result")
        if caps is not None and edits is not None:
            break
    proc.kill()

    if not caps or not caps.get("documentFormattingProvider"):
        print("FAIL: initialize missing documentFormattingProvider")
        print(caps)
        return 1
    if not caps.get("codeActionProvider"):
        print("FAIL: initialize missing codeActionProvider")
        return 1
    if not isinstance(edits, list) or not edits:
        print("FAIL: formatting returned no edits")
        print(edits)
        return 1
    text = edits[0].get("newText", "")
    if "    let x = 1;" not in text:
        print("FAIL: formatted text missing indented let")
        print(text)
        return 1
    print("OK")
    return 0

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: format.py <vyxc-lsp>")
        sys.exit(2)
    sys.exit(main())

#!/usr/bin/env bash
# Semantic LSP: publishDiagnostics carries a Sema code for a type error.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
if [ -f /etc/profile.d/vyx-llvm22.sh ]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/vyx-llvm22.sh
fi

cd "$ROOT/bootstrap_compiler"
if [ ! -x out/boot ]; then
  echo "building boot for LSP probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x out/vyxc-lsp ]; then
  echo "building vyxc-lsp..."
  LD_LIBRARY_PATH=out ./out/boot build --target vyxc-lsp -j"$(nproc)"
fi

python3 - "$ROOT/bootstrap_compiler/out/vyxc-lsp" <<'PY'
import json, os, subprocess, sys

lsp = sys.argv[1]
env = os.environ.copy()
src = 'fn main() -> i32 {\n let x: i32 = "no";\n return 0;\n}\n'
payload = json.dumps({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {"capabilities": {}, "rootUri": None}
})
did = json.dumps({
    "jsonrpc": "2.0",
    "method": "textDocument/didOpen",
    "params": {
        "textDocument": {
            "uri": "file:///tmp/lsp_probe.vyx",
            "languageId": "vyx",
            "version": 1,
            "text": src
        }
    }
})

def frame(body: str) -> bytes:
    data = body.encode()
    return f"Content-Length: {len(data)}\r\n\r\n".encode() + data

proc = subprocess.Popen(
    [lsp, "--stdio"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    env=env,
)
assert proc.stdin and proc.stdout
proc.stdin.write(frame(payload) + frame(did))
proc.stdin.flush()

def read_msg():
    headers = {}
    while True:
        line = proc.stdout.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        k, _, v = line.decode().partition(":")
        headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    if n <= 0:
        return None
    data = proc.stdout.read(n)
    return json.loads(data.decode())

found = False
for _ in range(8):
    msg = read_msg()
    if msg is None:
        break
    if msg.get("method") == "textDocument/publishDiagnostics":
        diags = msg.get("params", {}).get("diagnostics", [])
        text = json.dumps(diags)
        if "E1000" in text or "type" in text.lower():
            found = True
            break

proc.kill()
if not found:
    print("FAIL: no semantic publishDiagnostics")
    sys.exit(1)
print("OK")
PY

python3 "$ROOT/probes/gates/lsp/format.py" "$ROOT/bootstrap_compiler/out/vyxc-lsp"
python3 "$ROOT/probes/gates/lsp/completion.py" "$ROOT/bootstrap_compiler/out/vyxc-lsp"

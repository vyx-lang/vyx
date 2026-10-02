#!/usr/bin/env bash
set -euo pipefail

# Build the Phase-0 bootstrap project.
# Build the bootstrap entry from the workspace root, where its project-local
# standard library resolves automatically.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="$(cd "$ROOT/.." && pwd)"
ENTRY="$ROOT/src/core/main.vyx"
OUT="$ROOT/boot_a.exe"

if [ ! -f "$ENTRY" ]; then
    echo "entry $ENTRY missing" >&2
    exit 2
fi

VYXC=""
if command -v vyxc >/dev/null 2>&1; then
    VYXC="vyxc"
elif [ -x "$WS/build/vyxc" ]; then
    VYXC="$WS/build/vyxc"
elif [ -x "$WS/build/vyxc.exe" ]; then
    VYXC="$WS/build/vyxc.exe"
elif [ -x "$WS/cmake-build-debug/vyxc" ]; then
    VYXC="$WS/cmake-build-debug/vyxc"
else
    echo "vyxc not found in PATH, build/, or cmake-build-debug/" >&2
    exit 3
fi

cd "$WS"
echo "[bootstrap] (cwd=$WS) $VYXC --src=file bootstrap_compiler/src/core/main.vyx -o $OUT"
exec "$VYXC" --src=file "bootstrap_compiler/src/core/main.vyx" -o "$OUT"

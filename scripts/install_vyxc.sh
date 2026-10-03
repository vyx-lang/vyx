#!/usr/bin/env bash
# Install dist/my-folder.tar.xz (linux-x86_64-release SDK) onto PATH as `vyxc`.
# Usage: sudo bash scripts/install_vyxc.sh [archive]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCHIVE="${1:-$REPO_ROOT/dist/my-folder.tar.xz}"
PREFIX="${VYX_SDK_ROOT:-/opt/vyx/linux-x86_64-release}"
WRAPPER=/usr/local/bin/vyxc

[ -f "$ARCHIVE" ] || { echo "missing archive: $ARCHIVE" >&2; exit 1; }

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
tar -xJf "$ARCHIVE" -C "$tmpdir"

src=""
if [ -x "$tmpdir/linux-x86_64-release/bin/vyxc" ]; then
    src="$tmpdir/linux-x86_64-release"
elif [ -x "$tmpdir/bin/vyxc" ]; then
    src="$tmpdir"
else
    echo "archive has no bin/vyxc" >&2
    find "$tmpdir" -name vyxc | head >&2
    exit 1
fi

mkdir -p "$(dirname "$PREFIX")"
rm -rf "$PREFIX"
cp -a "$src" "$PREFIX"

cat >"$WRAPPER" <<EOF
#!/bin/sh
ROOT="\${VYX_SDK_ROOT:-$PREFIX}"
if [ -z "\${LLVM_ROOT:-}" ]; then
  export LLVM_ROOT="\$ROOT"
fi
export LD_LIBRARY_PATH="\$ROOT/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export PATH="\$ROOT/toolchain/bin:\$PATH"
exec "\$ROOT/bin/vyxc" "\$@"
EOF
chmod 755 "$WRAPPER"
ln -sfn "$WRAPPER" /usr/local/bin/vyx

echo "installed $PREFIX"
echo "try: vyxc --help"

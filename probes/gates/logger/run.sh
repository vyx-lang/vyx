#!/usr/bin/env bash
# std:logger — format/log extracted; DCI + spdlog via package prebuild Vyx script.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
export VYX_STD_PACKAGES="$ROOT/bootstrap_compiler/std_packages"

if [ -n "${LOGGER_VYXC:-}" ]; then
    BOOT="$LOGGER_VYXC"
elif [ -x "$ROOT/bootstrap_compiler/out/boot" ]; then
    BOOT="$ROOT/bootstrap_compiler/out/boot"
elif [ -x /home/ubuntu/wp-deref/bootstrap_compiler/out/boot ]; then
    BOOT=/home/ubuntu/wp-deref/bootstrap_compiler/out/boot
else
    echo "FAIL: no boot; set LOGGER_VYXC" >&2
    exit 1
fi

BOOT_DIR="$(cd "$(dirname "$BOOT")" && pwd)"
export LD_LIBRARY_PATH="$BOOT_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ ! -f "$VYX_STD_PACKAGES/logger/Vyx.toml" ]; then
    echo "FAIL: std_packages/logger missing" >&2
    exit 1
fi
if grep -q 'src/log.vyx' "$VYX_STD_PACKAGES/format/Vyx.toml"; then
    echo "FAIL: format package still lists src/log.vyx" >&2
    exit 1
fi
if grep -q 'format/src/log.vyx' "$VYX_STD_PACKAGES/registry"; then
    echo "FAIL: implicit registry still forwards format/src/log.vyx" >&2
    exit 1
fi
if ! grep -q 'prebuild = \["vyx: scripts/prepare_spdlog.vyx"\]' "$VYX_STD_PACKAGES/logger/Vyx.toml"; then
    echo "FAIL: logger package is not using the vyx: prebuild script" >&2
    exit 1
fi

APP="$(mktemp -d /tmp/std_logger_probe.XXXXXX)"
cleanup() { rm -rf "$APP"; }
trap cleanup EXIT

cat > "$APP/Vyx.toml" << 'EOF'
[package]
name = "std_logger_probe"
version = "0.1.0"
entry = "src/main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
threads = 1

[dependencies]
logger = { path = "std:logger", target = "std_logger" }

[target.std_logger_probe]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
EOF

mkdir -p "$APP/src"
cat > "$APP/src/main.vyx" << 'EOF'
use std.log;

fn main() -> i32 {
    set_log_level(log_level_info());
    log_info("std logger via dci spdlog");
    log_flush();
    return 0;
}
EOF

echo "== using compiler: $BOOT =="
echo "== VYX_STD_PACKAGES=$VYX_STD_PACKAGES =="
( cd "$APP" && "$BOOT" build --target std_logger_probe )
LOG="$APP/run.log"
"$APP/target/std_logger_probe" >"$LOG" 2>&1
cat "$LOG"
if ! grep -q 'std logger via dci spdlog' "$LOG"; then
    echo "FAIL: spdlog did not emit the Vyx log_info line" >&2
    exit 1
fi
echo "OK: std.logger dci+spdlog"

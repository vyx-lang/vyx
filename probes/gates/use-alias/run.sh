#!/usr/bin/env bash
# Short-name / module aliases in a multi-file, multi-module project.
# Pins: `use path as Alias`, `use Alias = path` after |imported, Alias.fn(),
# Alias.Type { }, and Alias.Type annotations. Two sibling modules both
# export class Box; the entry file must keep them apart via aliases.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"

if [ -n "${ALIAS_VYXC:-}" ]; then
    BOOT="$ALIAS_VYXC"
elif [ -x "$ROOT/bootstrap_compiler/out/boot" ]; then
    BOOT="$ROOT/bootstrap_compiler/out/boot"
else
    echo "FAIL: no boot; set ALIAS_VYXC or build bootstrap_compiler/out/boot" >&2
    exit 1
fi

BOOT_DIR="$(cd "$(dirname "$BOOT")" && pwd)"
export LD_LIBRARY_PATH="$BOOT_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

APP="$(mktemp -d /tmp/use_alias_probe.XXXXXX)"
cleanup() { rm -rf "$APP"; }
trap cleanup EXIT

mkdir -p "$APP/src"
cat > "$APP/Vyx.toml" << 'EOF'
[package]
name = "use_alias_probe"
version = "0.1.0"
entry = "src/main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
threads = 1

[target.use_alias_probe]
type = "executable"
entry = "src/main.vyx"
sources = ["src/helper.vyx", "src/other.vyx", "src/util.vyx"]
auto_sources = false
EOF

cat > "$APP/src/helper.vyx" << 'EOF'
module helper;

public fn marker() -> i32 { return 7; }

public class Box {
    public n: i32;
}
EOF

cat > "$APP/src/other.vyx" << 'EOF'
module other;

public fn tag() -> i32 { return 3; }

public class Box {
    public n: i32;
    public tag: i32;
}
EOF

cat > "$APP/src/util.vyx" << 'EOF'
module app.util;

use helper as h;

public fn util_mark() -> i32 {
    return h.marker();
}
EOF

cat > "$APP/src/main.vyx" << 'EOF'
module app;

use helper as h;
use H = helper;
use other as o;
use app.util;

fn take(b: h.Box) -> i32 {
    return b.n;
}

fn main() -> i32 {
    let a = h.Box { n: 7 };
    let b: H.Box = H.Box { n: 7 };
    let c = o.Box { n: 1, tag: 2 };
    return h.marker() + H.marker() + helper.marker()
        + take(a) + b.n + c.n + c.tag + o.tag() + util_mark() - 48;
}
EOF

echo "== using compiler: $BOOT =="
if ! ( cd "$APP" && "$BOOT" build --target use_alias_probe ); then
    echo "FAIL: project build" >&2
    exit 1
fi

EXE="$APP/target/use_alias_probe"
if [ ! -x "$EXE" ]; then
    echo "FAIL: no executable at $EXE" >&2
    ls -la "$APP/target" >&2 || true
    exit 1
fi

set +e
"$EXE"
RC=$?
set -e
if [ "$RC" -ne 0 ]; then
    echo "FAIL: probe exit=$RC" >&2
    exit 1
fi
echo "OK: use-alias multi-module"

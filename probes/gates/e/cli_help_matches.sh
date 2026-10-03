#!/usr/bin/env bash
# WP-E: help flag set == flags parse_cli_args actually accepts.
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
  echo "building boot for WP-E probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"

HELP="$("$BOOT" help 2>&1 || true)"
if echo "$HELP" | grep -q -- '--dump-hir2\|--dump-mir2\|--verify-hir2\|--verify-mir2\|--verify-hir-facts'; then
  if ! echo "$HELP" | grep -q -- 'aliases:'; then
    echo "FAIL: dump/verify flags in help must be labeled as aliases of --emit="
    exit 1
  fi
fi
if echo "$HELP" | grep -q -- '--legacy-codegen'; then
  echo "FAIL: help still documents --legacy-codegen"
  exit 1
fi
if ! echo "$HELP" | grep -q -- '--version'; then
  echo "FAIL: help must document --version"
  exit 1
fi
if grep -n 'a == "--legacy-codegen"' "$ROOT/bootstrap_compiler/src/core/main.vyx"; then
  echo "FAIL: --legacy-codegen is still parsed in main.vyx"
  exit 1
fi

python3 - "$BOOT" "$HELP" "$ROOT/bootstrap_compiler/src/core/main.vyx" <<'PY'
import re, subprocess, sys, tempfile, os
boot, help_text, main_path = sys.argv[1], sys.argv[2], sys.argv[3]
fail = False

# Only the Options: block (not Usage prose like `new ... [--bare]`).
opt_block = help_text.split("Options:", 1)[-1] if "Options:" in help_text else help_text
help_flags = set()
for line in opt_block.splitlines():
    for m in re.finditer(r'(--[A-Za-z0-9-]+(?:=[A-Za-z0-9-]*)?)|(-O[0-3sz])|(-fPIC)|(-fpic)|(-h)|(-g)|(-o)|(-j)|(-L)|(-l)', line):
        raw = m.group(0)
        if raw.startswith('--src'):
            help_flags.add('--src=')
        elif raw.startswith('--emit=') or raw == '--emit':
            help_flags.add('--emit=')
        elif raw.startswith('--run'):
            help_flags.add('--run=')
        elif raw in ('-h', '--help'):
            continue
        else:
            help_flags.add(raw.split('=')[0] if '=' in raw and not raw.startswith('--emit') else raw.split('=')[0])

src = tempfile.NamedTemporaryFile('w', suffix='.vyx', delete=False)
src.write('fn main() -> i32 { return 0; }\n')
src.close()
os.environ['LD_LIBRARY_PATH'] = os.path.dirname(boot)

VALUE_FLAGS = {
    '-o', '-L', '-l', '-j', '--target', '--triplet', '--sysroot', '--dci', '--dci-file',
    '--lib-path', '--link', '--link-obj', '--interface-root', '--module-source',
    '--max-errors', '--mir-opt', '--llvm-opt', '--unit-sources', '--project-unit-sources',
    '--interface-unit-sources', '--module-sources-file', '--export-root-names',
    '--export-root-names-file', '--emit-root-names', '--emit-root-names-file',
}

def run(args):
    p = subprocess.run([boot, *args], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr

unknown = []
for flag in sorted(help_flags):
    args = ['--src=file', src.name, '--emit=ir', '-o', src.name + '.ir']
    if flag in ('--src=', '--emit='):
        continue
    if flag == '--run=':
        args.append('--run=aot')
    elif flag in VALUE_FLAGS:
        dummy = 'x'
        if flag in ('--mir-opt', '--llvm-opt'):
            dummy = '0'
        elif flag == '--max-errors':
            dummy = '1'
        elif flag == '-j':
            dummy = '1'
        args.extend([flag, dummy])
    else:
        args.append(flag)
    rc, out = run(args)
    if 'unknown option' in out:
        unknown.append((flag, out.strip().splitlines()[-1] if out.strip() else 'unknown'))
        fail = True

if unknown:
    print('FAIL: help flags reported unknown option:')
    for f, line in unknown:
        print(f'  {f}: {line}')

src_text = open(main_path).read()
accepted = set()
for fn_name in ('cli_is_mode_arg', 'cli_option_takes_value'):
    m = re.search(r'fn ' + fn_name + r'\(.*?\n\}', src_text, re.S)
    if not m:
        print('FAIL: missing', fn_name)
        fail = True
        continue
    for lit in re.findall(r'"([^"]+)"', m.group(0)):
        if not lit.startswith('-'):
            continue
        if lit.startswith('--run='):
            accepted.add('--run=')
        elif lit.startswith('--emit='):
            accepted.add('--emit=')
        else:
            accepted.add(lit.split('=')[0] if '=' in lit else lit)

accepted.update(['--src=', '--emit=', '--run=', '--target', '--triplet', '--sysroot', '-L', '-l', '-o', '-j'])

removed = set()
m = re.search(r'fn cli_is_removed_ir_arg\(.*?\n\}', src_text, re.S)
if m:
    for lit in re.findall(r'"([^"]+)"', m.group(0)):
        if lit.startswith('-'):
            removed.add(lit)

missing_help = []
for flag in sorted(accepted):
    if flag in removed:
        continue
    if flag not in help_flags:
        missing_help.append(flag)
        fail = True

if missing_help:
    print('FAIL: accepted flags missing from help:')
    for f in missing_help:
        print(' ', f)

if fail:
    sys.exit(1)
print('OK: help set matches accepted CLI flags; --legacy-codegen is gone')
os.unlink(src.name)
PY

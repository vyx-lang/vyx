#!/usr/bin/env bash
set -euo pipefail
component_root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
repo_root=$(cd -- "$component_root/../.." && pwd -P)
compiler=${1:-$repo_root/dist/vyx-sdk-linux-x86_64-llvm22/bin/vyxc}
clang=$(command -v clang++)
real_builtins=$("$clang" --rtlib=compiler-rt -print-libgcc-file-name)
[[ -f $real_builtins && -x $compiler ]] || { echo 'A native Clang compiler-rt and Linux SDK are required.' >&2; exit 1; }
root=$(mktemp -d /tmp/vyx-termux-toolchain-XXXXXXXX)
trap 'rm -rf -- "$root"' EXIT
export VYX_TEST_CLANG=$clang VYX_TEST_COMPILER=$compiler VYX_TEST_RESOURCE=$root/resource
mkdir -p "$root/prefix/bin" "$root/work" "$VYX_TEST_RESOURCE/lib/linux"
ln -s "$("$clang" -print-resource-dir)/include" "$VYX_TEST_RESOURCE/include"
cat > "$root/prefix/bin/clang++" <<'SH'
#!/usr/bin/env bash
if [[ ${VYX_TEST_REJECT_LINK:-0} == 1 && $1 != -print-libgcc-file-name ]]; then
  exec "$VYX_TEST_CLANG" --rtlib=compiler-rt -resource-dir "$VYX_TEST_RESOURCE" -Wl,--vyx-setup-test-reject "$@"
fi
exec "$VYX_TEST_CLANG" --rtlib=compiler-rt -resource-dir "$VYX_TEST_RESOURCE" "$@"
SH
cat > "$root/prefix/bin/vyxc" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$VYX_TEST_SDK_LOG"
if [[ ${VYX_TEST_REJECT_SDK:-0} == 1 && $1 == build ]]; then exit 43; fi
exec "$VYX_TEST_COMPILER" "$@"
SH
chmod +x "$root/prefix/bin/clang++" "$root/prefix/bin/vyxc"
export VYX_TEST_SDK_LOG=$root/sdk.log
source "$component_root/install/termux.sh"
repair_mode=success
pkg() {
  [[ $* == 'reinstall -y libcompiler-rt' ]] || return 1
  printf '%s\n' "$*" >> "$root/repair.log"
  case "$repair_mode" in reject) return 41 ;; noop) return 0 ;; esac
  cp -- "$real_builtins" "$VYX_TEST_RESOURCE/lib/linux/$(basename -- "$real_builtins")"
}
# The real driver must fail to link before its matching builtins are restored.
printf 'int main() { return 0; }\n' > "$root/before.cpp"
if "$root/prefix/bin/clang++" "$root/before.cpp" -o "$root/before" > "$root/before.log" 2>&1; then
  echo 'Missing builtins did not reproduce a native link failure.' >&2; exit 1
fi
grep -q 'libclang_rt.builtins' "$root/before.log"
termux_verify_linker "$root/prefix" "$root/work"
[[ $(wc -l < "$root/repair.log") == 1 ]]
termux_verify_sdk "$root/prefix" "$root/work"
grep -Fxq 'build --run=aot -j1' "$VYX_TEST_SDK_LOG"
# An already complete compiler does not reinstall packages.
termux_verify_linker "$root/prefix" "$root/work"
[[ $(wc -l < "$root/repair.log") == 1 ]]
export VYX_TEST_REJECT_LINK=1
if termux_verify_linker "$root/prefix" "$root/work" > "$root/link-rejection.log" 2>&1; then
  echo 'Link failure was accepted.' >&2; exit 1
fi
grep -q 'Setup is incomplete' "$root/link-rejection.log"
unset VYX_TEST_REJECT_LINK
export VYX_TEST_REJECT_SDK=1
if termux_verify_sdk "$root/prefix" "$root/work" > "$root/sdk-rejection.log" 2>&1; then
  echo 'Vyx project failure was accepted.' >&2; exit 1
fi
grep -q 'Setup is incomplete' "$root/sdk-rejection.log"
unset VYX_TEST_REJECT_SDK
rm -- "$VYX_TEST_RESOURCE/lib/linux/$(basename -- "$real_builtins")"
repair_mode=noop
if termux_verify_linker "$root/prefix" "$root/work" > "$root/noop-repair.log" 2>&1; then
  echo 'An incomplete builtins repair was accepted.' >&2; exit 1
fi
grep -q 'builtins are still missing' "$root/noop-repair.log"
repair_mode=reject
if termux_verify_linker "$root/prefix" "$root/work" > "$root/repair-rejection.log" 2>&1; then
  echo 'Failed builtins repair was accepted.' >&2; exit 1
fi
echo 'PASS: real missing-builtins link failure, matching compiler-rt repair, native C++ run, real SDK project AOT, no needless reinstall, rejected linker/SDK/repair failures.'
echo 'Platform: Linux host orchestration; Android device execution not tested.'

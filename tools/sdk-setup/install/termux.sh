#!/usr/bin/env bash
set -euo pipefail

# These checks are also sourced by the host regression gate. Production setup
# still requires the fixed Termux prefix below.
termux_verify_linker() {
  local prefix=$1 work=$2 builtins
  local clang=$prefix/bin/clang++
  if ! builtins=$("$clang" -print-libgcc-file-name); then
    echo 'Termux clang++ cannot start; repair its clang/libllvm packages.' >&2
    return 1
  fi
  if [[ ! -f $builtins ]]; then
    printf 'Missing Clang builtins: %s\nReinstalling libcompiler-rt...\n' "$builtins"
    pkg reinstall -y libcompiler-rt || return 1
    builtins=$("$clang" -print-libgcc-file-name) || return 1
    if [[ ! -f $builtins ]]; then
      printf 'Clang builtins are still missing: %s\nUpdate clang and libcompiler-rt from the same Termux repository.\n' "$builtins" >&2
      return 1
    fi
  fi
  cat > "$work/link.cpp" <<'CPP'
#include <iostream>
#include <string>
int main() { std::cout << std::string("Termux C++ link check passed.") << '\n'; return 0; }
CPP
  if ! "$clang" "$work/link.cpp" -o "$work/link-check"; then
    echo 'Termux C++ linking failed; check clang, libcompiler-rt, lld and ndk-sysroot. Setup is incomplete.' >&2
    return 1
  fi
  "$work/link-check" || return 1
}

termux_verify_sdk() {
  local prefix=$1 work=$2
  "$prefix/bin/vyxc" --version || return 1
  mkdir -p "$work/project/src" || return 1
  cat > "$work/project/src/main.vyx" <<'VYX'
fn main() -> i32 { print("Termux SDK AOT check passed."); return 0; }
VYX
  cat > "$work/project/Vyx.toml" <<'TOML'
[package]
name = "vyx_setup_check"
version = "0.1.0"
entry = "src/main.vyx"

[build]
output_dir = "target"

[target.vyx_setup_check]
type = "executable"
entry = "src/main.vyx"
TOML
  if ! (cd "$work/project" && "$prefix/bin/vyxc" build --run=aot -j1); then
    echo 'Vyx project AOT build/run failed. Setup is incomplete; the linker diagnostics are printed above.' >&2
    return 1
  fi
}

# Sourcing exposes the validation functions without installing anything. A script
# read from stdin has no BASH_SOURCE entry and must run setup normally.
if [[ -n ${BASH_SOURCE[0]:-} && ${BASH_SOURCE[0]} != "$0" ]]; then return 0; fi

action=setup
yes=0
base_url=https://github.com/vyx-lang/vyx/releases/latest/download
while (($#)); do
  case "$1" in
    setup|update|uninstall) action=$1; shift ;;
    --yes|-y) yes=1; shift ;;
    --base-url) base_url=${2:?Missing download URL}; shift 2 ;;
    *) printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
  esac
done
base_url=${base_url%/}
case "$base_url" in
  https://*|http://localhost:*|http://127.0.0.1:*|http://\[::1\]:*) ;;
  *) echo 'Use an HTTPS URL (HTTP is supported for localhost previews).' >&2; exit 1 ;;
esac
[[ ${PREFIX:-} == /data/data/com.termux/files/usr ]] || { echo 'Run this installer inside Termux.' >&2; exit 1; }
command -v apt >/dev/null || { echo 'This SDK installer requires apt-based Termux.' >&2; exit 1; }
case "$(uname -m)" in
  aarch64|arm64) ;;
  *) echo 'The Termux SDK requires an ARM64 device.' >&2; exit 1 ;;
esac
# The .deb uses Termux's fixed prefix; pkg owns the installed SDK files.
if [[ $action == uninstall ]]; then
  if (( ! yes )); then
    if ! { exec 3<>/dev/tty; } 2>/dev/null; then echo 'Use --yes for unattended uninstall.' >&2; exit 1; fi
    printf 'Remove the Vyx SDK? [y/N]: ' >&3
    IFS= read -r answer <&3; exec 3>&-
    [[ $answer == y || $answer == yes ]] || { echo 'Cancelled.'; exit 0; }
  fi
  pkg uninstall -y vyx-sdk-termux
  echo 'Vyx uninstalled. clang, lld, Python and project files were kept.'
  exit 0
fi
for tool in pkg curl sha256sum dpkg-deb mktemp; do
  command -v "$tool" >/dev/null || { printf 'Required tool missing: %s\n' "$tool" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-$PREFIX/tmp}/vyx-install-XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
archive_name=vyx-sdk-termux-aarch64.deb
echo 'Downloading the Termux ARM64 SDK...'
curl --fail --location --silent --show-error --retry 3 "$base_url/$archive_name.sha256" -o "$work/checksum"
checksum_line=$(tr -d '\r' < "$work/checksum")
[[ $checksum_line != *$'\n'* ]] || { echo 'Invalid SDK checksum file.' >&2; exit 1; }
read -r expected checksum_name extra <<< "$checksum_line"
[[ $expected =~ ^[a-fA-F0-9]{64}$ && ${checksum_name#\*} == "$archive_name" && -z ${extra:-} ]] || { echo 'Invalid SDK checksum file.' >&2; exit 1; }
curl --fail --location --show-error --retry 3 "$base_url/$archive_name" -o "$work/$archive_name"
actual=$(sha256sum "$work/$archive_name")
[[ ${actual%% *} == "${expected,,}" ]] || { echo 'SDK checksum mismatch; installation stopped.' >&2; exit 1; }
[[ $(dpkg-deb -f "$work/$archive_name" Package) == vyx-sdk-termux && $(dpkg-deb -f "$work/$archive_name" Architecture) == aarch64 ]] || { echo 'The download is not a Termux ARM64 SDK package.' >&2; exit 1; }
# pkg forwards local .deb files to apt and installs their dependencies. No sudo.
pkg install -y clang libcompiler-rt lld ndk-sysroot python "$work/$archive_name"
export PATH="$PREFIX/bin:$PATH"
termux_verify_linker "$PREFIX" "$work"
termux_verify_sdk "$PREFIX" "$work"
echo 'Vyx is installed. Run vyxc build in your project directory.'

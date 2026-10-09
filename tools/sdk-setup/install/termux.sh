#!/usr/bin/env bash
set -euo pipefail

action=setup
yes=0
base_url=https://www.vyxlang.com
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
curl --fail --location --silent --show-error --retry 3 "$base_url/sdk/$archive_name.sha256" -o "$work/checksum"
checksum_line=$(tr -d '\r' < "$work/checksum")
[[ $checksum_line != *$'\n'* ]] || { echo 'Invalid SDK checksum file.' >&2; exit 1; }
read -r expected checksum_name extra <<< "$checksum_line"
[[ $expected =~ ^[a-fA-F0-9]{64}$ && ${checksum_name#\*} == "$archive_name" && -z ${extra:-} ]] || { echo 'Invalid SDK checksum file.' >&2; exit 1; }
curl --fail --location --show-error --retry 3 "$base_url/sdk/$archive_name" -o "$work/$archive_name"
actual=$(sha256sum "$work/$archive_name")
[[ ${actual%% *} == "${expected,,}" ]] || { echo 'SDK checksum mismatch; installation stopped.' >&2; exit 1; }
[[ $(dpkg-deb -f "$work/$archive_name" Package) == vyx-sdk-termux && $(dpkg-deb -f "$work/$archive_name" Architecture) == aarch64 ]] || { echo 'The download is not a Termux ARM64 SDK package.' >&2; exit 1; }
# pkg forwards local .deb files to apt and installs their dependencies. No sudo.
pkg install -y clang lld python "$work/$archive_name"
export PATH="$PREFIX/bin:$PATH"
"$PREFIX/bin/vyxc" --version
echo 'Vyx is installed. Run vyxc build in your project directory.'

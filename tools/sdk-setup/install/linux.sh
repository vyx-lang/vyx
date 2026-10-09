#!/usr/bin/env bash
set -euo pipefail

action=setup
base_url=https://github.com/vyx-lang/vyx/releases/latest/download
installer_url=https://www.vyxlang.com/install/linux.sh
install_dir=${VYX_INSTALL_DIR:-}
config_dir=${XDG_CONFIG_HOME:-$HOME/.config}/vyx
yes=0
while (($#)); do
  case "$1" in
    setup|update|rollback|uninstall) action=$1; shift ;;
    --base-url) base_url=${2:?Missing download URL}; shift 2 ;;
    --installer-url) installer_url=${2:?Missing installer URL}; shift 2 ;;
    --install-dir) install_dir=${2:?Missing installation directory}; shift 2 ;;
    --yes|-y) yes=1; shift ;;
    *) printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
  esac
done
fail() { echo "$*" >&2; exit 1; }
quote() { printf "'%s'" "${1//\'/\'\"\'\"\'}"; }
ask() {
  if ! { exec 3<>/dev/tty; } 2>/dev/null; then fail 'An interactive terminal is required. Use --install-dir or --yes for unattended setup.'; fi
  printf '%s ' "$1" >&3
  IFS= read -r answer <&3
  exec 3>&-
}
[[ $(uname -s) == Linux ]] || fail 'This installer requires Linux.'
[[ ${PREFIX:-} != /data/data/com.termux/files/usr ]] || fail 'Use the Termux installer on Android.'
case "$(uname -m)" in
  x86_64) architecture=x86_64 ;;
  aarch64|arm64) architecture=aarch64 ;;
  *) fail 'The Linux SDK supports x64 and ARM64.' ;;
esac
explicit_dir=$install_dir
if [[ -z $install_dir && -f $config_dir/install-root ]]; then IFS= read -r install_dir < "$config_dir/install-root"; fi
# Adopt the first installer's current/bin location when it is already on PATH.
if [[ -z $install_dir ]] && command -v vyxc >/dev/null; then
  legacy_bin=$(dirname -- "$(command -v vyxc)")
  if [[ $legacy_bin == */current/bin && -L ${legacy_bin%/bin} ]]; then install_dir=${legacy_bin%/current/bin}; fi
fi
if [[ -z $install_dir ]]; then
  [[ $action == setup ]] || { echo 'No managed Vyx SDK is installed.'; exit 0; }
  install_dir=$HOME/.local/share/vyx
fi
if [[ $action == setup && -z $explicit_dir && $yes == 0 ]]; then
  printf 'Vyx SDK setup\nDirectory: %s\n  1) Install / update here (default)\n  2) Choose a directory\n  3) Cancel\n' "$install_dir"
  ask 'Choose [1]:'
  case "$answer" in
    ''|1) ;;
    2) ask 'Installation directory:'; [[ -n $answer ]] || fail 'A directory is required.'; install_dir=$answer ;;
    3) echo 'Cancelled.'; exit 0 ;;
    *) fail 'Choose 1, 2 or 3.' ;;
  esac
fi
case "$install_dir" in *:*|*$'\n'*|*$'\r'*) fail 'The installation path cannot contain a colon or newline (PATH separator).' ;; esac
[[ $install_dir == /* ]] || install_dir=$PWD/$install_dir
install_dir=$(realpath -m -- "$install_dir")
[[ $install_dir != / && $install_dir != "$HOME" ]] || fail 'Choose a dedicated SDK directory.'
root=$install_dir
releases=$root/releases
current=$root/current
state=$root/.vyx-state
marker=$root/.vyx-setup
for dir in "$releases" "$root/bin"; do [[ ! -L $dir ]] || fail "Managed directory must not be a link: $dir"; done
if [[ -e $marker ]]; then [[ $(cat "$marker") == 'vyx-sdk-setup-v2' ]] || fail 'The directory is unmanaged.'; fi
require_release() {
  [[ $1 =~ ^[a-f0-9]{64}$ ]] || fail 'Invalid SDK revision.'
  local release=$releases/$1
  [[ ! -L $release && -f $release/.vyx-archive.sha256 && $(cat "$release/.vyx-archive.sha256") == "$1" && -f $release/SDK-MANIFEST.json && -x $release/bin/vyxc ]] || fail "Incomplete or unmanaged SDK: $release"
}
old=''; previous=''
if [[ -f $state ]]; then
  mapfile -t revisions < "$state"
  old=${revisions[0]:-}; previous=${revisions[1]:-}
  require_release "$old"
  [[ -z $previous ]] || require_release "$previous"
fi
if [[ -e $current || -L $current ]]; then
  [[ -L $current ]] || fail 'The current SDK directory is unmanaged.'
  target=$(readlink -- "$current")
  [[ $target == releases/* && ${target#releases/} =~ ^[a-f0-9]{64}$ ]] || fail 'The current SDK link is unmanaged.'
  require_release "${target#releases/}"
  if [[ -z $old ]]; then old=${target#releases/}; else [[ $target == releases/$old ]] || fail 'SDK state and current link disagree.'; fi
elif [[ -n $old ]]; then fail 'The current SDK link is missing.'
fi
if [[ $action == uninstall ]]; then
  [[ -f $marker && -n $old ]] || fail 'Uninstall requires a managed SDK. Run setup once to adopt an older installation.'
  if (( ! yes )); then ask "Remove managed SDKs from $root ? [y/N]:"; [[ $answer == y || $answer == yes ]] || { echo 'Cancelled.'; exit 0; }; fi
  shopt -s nullglob
  owned=()
  for release in "$releases"/*; do
    hash=${release##*/}
    if [[ $hash =~ ^[a-f0-9]{64}$ ]]; then require_release "$hash"; owned+=("$release"); fi
  done
  # A shell loader is removed only for the installation currently registered in this user's config.
  if [[ -f $config_dir/install-root && $(cat "$config_dir/install-root") == "$root" ]]; then
    setup_line='[ ! -f "${XDG_CONFIG_HOME:-$HOME/.config}/vyx/env" ] || . "${XDG_CONFIG_HOME:-$HOME/.config}/vyx/env" # Vyx SDK'
    if [[ -f $root/.vyx-profiles ]]; then
      while IFS= read -r profile; do
        if [[ -f $profile ]]; then
          temp=$(mktemp "${profile}.vyx-XXXXXXXX")
          { grep -Fvx -- "$setup_line" "$profile" || [[ $? == 1 ]]; } > "$temp"
          cat "$temp" > "$profile"; rm -f -- "$temp"
        fi
      done < "$root/.vyx-profiles"
    fi
    rm -f -- "$config_dir/env" "$config_dir/install-root"
    fish_file=${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/vyx.fish
    if [[ -f $fish_file ]] && grep -Fqx '# Vyx SDK; managed by vyxup' "$fish_file"; then rm -f -- "$fish_file"; fi
    rmdir -- "$config_dir" 2>/dev/null || true
  fi
  rm -- "$current"
  # Each recursive target is a verified direct child of releases; rm does not follow links within it.
  for release in "${owned[@]}"; do [[ $release == "$releases/"* && ! -L $release ]] || fail 'Unsafe SDK removal path.'; rm -rf -- "$release"; done
  rm -f -- "$state" "$marker" "$root/.vyx-profiles" "$root/vyxup.sh" "$root/bin/vyxup"
  for dir in "$releases" "$root/bin" "$root"; do rmdir -- "$dir" 2>/dev/null || true; done
  echo 'Vyx uninstalled. Project files, unrelated files and other tools were kept. Open a new terminal to refresh its inherited PATH.'
  exit 0
fi
[[ $action != update || -n $old ]] || fail 'No managed SDK found. Run setup first.'
[[ $action != rollback || -n $previous ]] || fail 'No previous SDK is available for rollback.'
base_url=${base_url%/}
case "$base_url" in https://*|http://localhost:*|http://127.0.0.1:*|http://\[::1\]:*) ;; *) fail 'Use an HTTPS URL (HTTP is supported for localhost fixtures).' ;; esac
for tool in curl tar sha256sum mktemp; do command -v "$tool" >/dev/null || fail "Required tool missing: $tool"; done
umask 022
mkdir -p "$releases" "$root/bin" "$config_dir"
work=$(mktemp -d "$root/.staging-XXXXXXXX")
committing=0; committed=0
cleanup() {
  status=$?
  if (( committing && ! committed )); then
    for index in "${!destinations[@]}"; do
      dest=${destinations[index]}
      if [[ -f $work/snapshot-$index ]]; then cp -p -- "$work/snapshot-$index" "$dest"; else rm -f -- "$dest"; fi
    done
    if [[ -n $old ]]; then ln -s "releases/$old" "$work/restore"; mv -Tf -- "$work/restore" "$current"; else rm -f -- "$current"; fi
  fi
  [[ $work == "$root/.staging-"* && ! -L $work ]] || exit 1
  rm -rf -- "$work"
  exit "$status"
}
trap cleanup EXIT
if [[ $action == rollback ]]; then expected=$previous; require_release "$expected"; else
  archive_name="vyx-sdk-linux-$architecture-llvm22.tar.gz"
  echo "Checking the Linux $architecture SDK..."
  curl -fsSL --retry 3 "$base_url/$archive_name.sha256" -o "$work/checksum"
  checksum_line=$(tr -d '\r' < "$work/checksum")
  [[ $checksum_line != *$'\n'* ]] || fail 'Invalid SDK checksum file.'
  read -r expected checksum_name extra <<< "$checksum_line"
  [[ $expected =~ ^[a-fA-F0-9]{64}$ && ${checksum_name#\*} == "$archive_name" && -z ${extra:-} ]] || fail 'Invalid SDK checksum file.'
  expected=${expected,,}
  if [[ -e $releases/$expected ]]; then require_release "$expected"; echo 'This SDK is already installed.'; else
    echo 'Downloading the SDK...'
    curl -fSL --retry 3 "$base_url/$archive_name" -o "$work/$archive_name"
    actual=$(sha256sum "$work/$archive_name")
    [[ ${actual%% *} == "$expected" ]] || fail 'SDK checksum mismatch; installation stopped.'
    mkdir "$work/unpacked"
    tar -xzf "$work/$archive_name" -C "$work/unpacked"
    payload="$work/unpacked/${archive_name%.tar.gz}"
    [[ -x $payload/bin/vyxc && -f $payload/SDK-MANIFEST.json ]] || fail 'The archive does not contain a complete Linux SDK.'
    "$payload/bin/vyxc" --version || fail 'SDK startup failed; current SDK was kept.'
    printf '%s\n' "$expected" > "$payload/.vyx-archive.sha256"
    mv -- "$payload" "$releases/$expected"
  fi
fi
"$releases/$expected/bin/vyxc" --version || fail 'SDK startup failed; current SDK was kept.'
[[ $old == "$expected" ]] || previous=$old
printf '%s\n%s\n' "$expected" "$previous" > "$work/state"
printf '%s\n' 'vyx-sdk-setup-v2' > "$work/marker"
printf '%s\n' "$root" > "$work/install-root"
if [[ -f ${BASH_SOURCE[0]:-} ]]; then cp -- "${BASH_SOURCE[0]}" "$work/manager"; else
  case "$installer_url" in https://*|http://localhost:*|http://127.0.0.1:*|http://\[::1\]:*) ;; *) fail 'Use an HTTPS installer URL (HTTP is supported for localhost fixtures).' ;; esac
  curl -fsSL --retry 3 "$installer_url" -o "$work/manager"
fi
bash -n "$work/manager"
{
  printf '#!/usr/bin/env bash\nexec bash %s --install-dir %s "$@"\n' "$(quote "$root/vyxup.sh")" "$(quote "$root")"
} > "$work/launcher"
{
  printf '# Vyx SDK PATH; managed by vyxup.\n'
  for bin in "$root/bin" "$root/current/bin"; do printf 'case ":$PATH:" in *:%s:*) ;; *) export PATH=%s:"$PATH" ;; esac\n' "$(quote "$bin")" "$(quote "$bin")"; done
} > "$work/env"
setup_line='[ ! -f "${XDG_CONFIG_HOME:-$HOME/.config}/vyx/env" ] || . "${XDG_CONFIG_HOME:-$HOME/.config}/vyx/env" # Vyx SDK'
profiles=("$HOME/.profile" "$HOME/.bashrc")
[[ ! -f $HOME/.bash_profile ]] || profiles+=("$HOME/.bash_profile")
if [[ ${SHELL:-} == */zsh || -f $HOME/.zshrc ]]; then profiles+=("${ZDOTDIR:-$HOME}/.zshrc" "${ZDOTDIR:-$HOME}/.zprofile"); fi
if [[ -f $root/.vyx-profiles ]]; then
  while IFS= read -r profile; do
    present=0
    for known in "${profiles[@]}"; do [[ $known != "$profile" ]] || present=1; done
    (( present )) || profiles+=("$profile")
  done < "$root/.vyx-profiles"
fi
printf '%s\n' "${profiles[@]}" > "$work/profiles"
destinations=("$state" "$marker" "$config_dir/install-root" "$root/vyxup.sh" "$root/bin/vyxup" "$config_dir/env" "$root/.vyx-profiles" "${profiles[@]}")
fish_file=${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/vyx.fish
if [[ ${SHELL:-} == */fish || -d ${XDG_CONFIG_HOME:-$HOME/.config}/fish ]]; then
  if [[ -f $fish_file ]] && ! grep -Fqx '# Vyx SDK; managed by vyxup' "$fish_file"; then fail "Refusing to overwrite unmanaged fish config: $fish_file"; fi
  destinations+=("$fish_file"); mkdir -p -- "$(dirname -- "$fish_file")"
fi
for index in "${!destinations[@]}"; do dest=${destinations[index]}; [[ ! -f $dest ]] || cp -p -- "$dest" "$work/snapshot-$index"; done
committing=1
for pair in 'state:.vyx-state' 'marker:.vyx-setup' 'manager:vyxup.sh' 'launcher:bin/vyxup' 'profiles:.vyx-profiles'; do cp -- "$work/${pair%%:*}" "$root/${pair#*:}"; done
chmod +x -- "$root/bin/vyxup"
cp -- "$work/env" "$config_dir/env"
cp -- "$work/install-root" "$config_dir/install-root"
for profile in "${profiles[@]}"; do if [[ ! -f $profile ]] || ! grep -Fqx -- "$setup_line" "$profile"; then printf '\n%s\n' "$setup_line" >> "$profile"; fi; done
if [[ ${destinations[-1]} == "$fish_file" ]]; then printf '# Vyx SDK; managed by vyxup\nfish_add_path %s %s\n' "$(quote "$root/current/bin")" "$(quote "$root/bin")" > "$fish_file"; fi
ln -s "releases/$expected" "$work/current"
mv -Tf -- "$work/current" "$current"
committed=1
printf 'SDK directory: %s\nManage: vyxup update / vyxup rollback / vyxup uninstall\nOpen a new terminal or source %s/env to activate PATH.\n' "$root" "$config_dir"

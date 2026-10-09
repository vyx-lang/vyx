#!/usr/bin/env bash
set -euo pipefail
base_url=${1:-http://127.0.0.1:4201}
component_root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
test_root=$(mktemp -d /tmp/vyx-installer-test-XXXXXXXX)
trap 'rm -rf -- "$test_root"' EXIT
export HOME="$test_root/home with spaces"
export XDG_CONFIG_HOME="$HOME/config with spaces"
export SHELL=/bin/zsh
mkdir -p "$HOME"
install_root="$test_root/SDK with spaces and 'quote"
# A real controlling terminal exercises the menu while the installer itself arrives on stdin via curl.
python3 - "$base_url" "$install_root" "$test_root" <<'PY'
import errno, os, pty, select, shlex, sys, time
base, root, artifacts = sys.argv[1:]
command = 'curl -fsSL ' + shlex.quote(base + '/install/linux.sh') + ' | bash -s -- --base-url ' + shlex.quote(base)
for mode, replies in [('cancel', [('Choose [1]:', '3')]), ('custom', [('Choose [1]:', '2'), ('Installation directory:', root)])]:
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp('bash', ['bash', '-o', 'pipefail', '-c', command])
    output = b''; next_reply = 0; deadline = time.monotonic() + 180
    while True:
        if time.monotonic() > deadline:
            os.kill(pid, 9); raise RuntimeError('Menu timed out')
        readable, _, _ = select.select([fd], [], [], 1)
        if not readable: continue
        try: chunk = os.read(fd, 65536)
        except OSError as exc:
            if exc.errno == errno.EIO: break
            raise
        if not chunk: break
        output += chunk
        if next_reply < len(replies) and replies[next_reply][0].encode() in output:
            os.write(fd, (replies[next_reply][1] + '\n').encode()); next_reply += 1
    os.close(fd)
    _, status = os.waitpid(pid, 0)
    with open(artifacts + '/' + mode + '.log', 'wb') as log: log.write(output)
    if os.waitstatus_to_exitcode(status) or next_reply != len(replies): raise RuntimeError(output.decode(errors='replace'))
    if mode == 'cancel' and os.path.exists(os.environ['XDG_CONFIG_HOME'] + '/vyx'): raise RuntimeError('Cancellation created config')
PY
. "$XDG_CONFIG_HOME/vyx/env"
[[ $(command -v vyxc) == "$install_root/current/bin/vyxc" ]]
[[ $(command -v vyxup) == "$install_root/bin/vyxup" ]]
[[ $(cat "$XDG_CONFIG_HOME/vyx/install-root") == "$install_root" ]]
first=$(readlink "$install_root/current")
mkdir -p "$test_root/hello-project/src"
cd "$test_root/hello-project"
printf 'fn main() -> i32 { print("installer-ok"); return 0; }\n' > src/main.vyx
printf '[package]\nname = "installer_hello"\nversion = "0.1.0"\nentry = "src/main.vyx"\n\n[build]\noutput_dir = "target"\n\n[target.installer_hello]\ntype = "executable"\nentry = "src/main.vyx"\n' > Vyx.toml
vyxc build --run=aot -j1
bash "$component_root/install/linux.sh" --yes --base-url "$base_url"
for profile in .profile .bashrc .zshrc .zprofile; do [[ $(grep -Fc '# Vyx SDK' "$HOME/$profile") == 1 ]]; done
bash -n "$XDG_CONFIG_HOME/vyx/env"
vyxup update --base-url "$base_url/update"
[[ -f $install_root/current/SETUP-TEST-REVISION ]]
second=$(readlink "$install_root/current")
[[ $first != "$second" ]]
[[ $(tail -n1 "$install_root/.vyx-state") == "${first#releases/}" ]]
vyxup rollback
[[ $(readlink "$install_root/current") == "$first" ]]
vyxup rollback
[[ $(readlink "$install_root/current") == "$second" ]]
for variant in bad-checksum bad-startup; do
  cp "$install_root/.vyx-state" "$test_root/state-before"
  if vyxup update --base-url "$base_url/$variant" > "$test_root/$variant.log" 2>&1; then echo "Invalid SDK accepted: $variant" >&2; exit 1; fi
  cmp "$install_root/.vyx-state" "$test_root/state-before"
  [[ $(readlink "$install_root/current") == "$second" ]]
done
grep -q 'checksum mismatch' "$test_root/bad-checksum.log"
grep -q 'startup failed' "$test_root/bad-startup.log"
[[ -z $(find "$install_root" -maxdepth 1 -name '.staging-*' -print) ]]
echo '# keep my shell configuration' >> "$HOME/.bashrc"
echo keep > "$install_root/user-note.txt"
mkdir "$test_root/external"; echo keep > "$test_root/external/keep.txt"
ln -s "$test_root/external" "$install_root/$first/external-link"
vyxup uninstall --yes
[[ -f $install_root/user-note.txt && -f $test_root/external/keep.txt && -f src/main.vyx ]]
[[ ! -e $install_root/current && ! -e $install_root/releases && ! -e $XDG_CONFIG_HOME/vyx/env && ! -e $XDG_CONFIG_HOME/vyx/install-root ]]
for profile in .profile .bashrc .zshrc .zprofile; do ! grep -Fq '# Vyx SDK' "$HOME/$profile"; done
grep -Fq '# keep my shell configuration' "$HOME/.bashrc"
mkdir "$test_root/mock-bin" "$test_root/termux-temp"
printf '#!/bin/sh\ncase "$1" in -m) echo armv7l ;; -s) echo Linux ;; *) exit 1 ;; esac\n' > "$test_root/mock-bin/uname"
chmod +x "$test_root/mock-bin/uname"
if PATH="$test_root/mock-bin:$PATH" bash "$component_root/install/linux.sh" --install-dir "$test_root/unsupported" > "$test_root/architecture.log" 2>&1; then echo 'Unsupported architecture accepted.' >&2; exit 1; fi
[[ ! -e $test_root/unsupported ]]
echo 'PASS: Linux real piped menu/cancel, custom directory with spaces/quotes, root memory, real SDK AOT, repeat setup, installed manager update, offline rollback, rejected checksum/startup, uninstall/profile cleanup and unrelated-file/link-target preservation.'

# Real Termux package validation; mock pkg to avoid installing Android files on Linux.
printf '#!/bin/sh\necho aarch64\n' > "$test_root/mock-bin/uname"
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "$VYX_TEST_PKG_LOG"\nexit 41\n' > "$test_root/mock-bin/pkg"
chmod +x "$test_root/mock-bin/pkg"
export VYX_TEST_PKG_LOG="$test_root/pkg.log"
for action in setup update; do
  set +e
  PREFIX=/data/data/com.termux/files/usr TMPDIR="$test_root/termux-temp" PATH="$test_root/mock-bin:$PATH" bash "$component_root/install/termux.sh" "$action" --base-url "$base_url" > "$test_root/termux.log" 2>&1
  termux_status=$?
  set -e
  [[ $termux_status == 41 ]]
  for arg in install -y clang lld python; do grep -Fxq -- "$arg" "$VYX_TEST_PKG_LOG"; done
  grep -q '/vyx-sdk-termux-aarch64.deb$' "$VYX_TEST_PKG_LOG"
done
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "$VYX_TEST_PKG_LOG"\n' > "$test_root/mock-bin/pkg"
PREFIX=/data/data/com.termux/files/usr PATH="$test_root/mock-bin:$PATH" bash "$component_root/install/termux.sh" uninstall --yes
[[ $(cat "$VYX_TEST_PKG_LOG") == $'uninstall\n-y\nvyx-sdk-termux' ]]
echo 'PASS: Termux real .deb download/validation for setup and update; uninstall removes only SDK. pkg mocked; Android device install not executed.'

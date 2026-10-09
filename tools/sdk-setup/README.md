# SDK setup

[中文](README.zh-CN.md)

Installers for Windows, Linux, and Termux. The local website copies `install/` to
its `/install/` assets during builds. Scripts download SDK packages and SHA256
files from `https://www.vyxlang.com`. SDK archives and website assets remain local.

## Install and manage

From the repository root, run one of:

```powershell
& .\tools\sdk-setup\install\windows.ps1
# Or choose a directory without an interactive menu:
& .\tools\sdk-setup\install\windows.ps1 -InstallDir 'D:\Vyx' -Yes
```

```bash
bash tools/sdk-setup/install/linux.sh
# Or choose a directory without an interactive menu:
bash tools/sdk-setup/install/linux.sh --install-dir "$HOME/SDKs/Vyx" --yes
. "${XDG_CONFIG_HOME:-$HOME/.config}/vyx/env"
```

Interactive desktop setup offers the default directory, a custom directory, or
cancel. It remembers the chosen directory and configures PATH without administrator
rights or sudo. After installation, run these commands from any project:

```sh
vyxup update
vyxup rollback
vyxup uninstall
```

An update verifies SHA256 and starts the new compiler before selecting it. A
failed download, checksum, or startup leaves the current SDK selected. Reinstalling
the same package skips its download; older versions remain available for offline
rollback. Uninstall asks for confirmation and removes managed SDKs, the manager,
and its environment setup while retaining projects and unrelated files. Reopen
your terminal afterwards to refresh its inherited PATH.

Windows defaults to `%LOCALAPPDATA%\Vyx`, with `sdk/<SHA256>` versions and a
`current` junction. `%APPDATA%\Vyx/install.json` records the root. Linux defaults
to `~/.local/share/vyx`, with `releases/<SHA256>` versions and a `current` symlink.
`${XDG_CONFIG_HOME:-$HOME/.config}/vyx/install-root` records the root; `env` sets
PATH and shell loaders are marked `# Vyx SDK`.

Windows uninstall removes junctions without following them into external folders.
When invoked through the CMD launcher, a hidden helper waits for that CMD process
to exit before removing the launcher, preserving the command's exit status.

Termux requires Android ARM64, apt-based Termux, and the fixed prefix
`/data/data/com.termux/files/usr`. It does not support a custom directory or
desktop rollback commands:

```sh
bash tools/sdk-setup/install/termux.sh
bash tools/sdk-setup/install/termux.sh update
pkg uninstall vyx-sdk-termux
```

Install and update pass the verified SDK package to `pkg`, with clang, lld, and
Python. Uninstall removes only `vyx-sdk-termux`, keeping those tools and projects.
The scripts do not set global `LD_LIBRARY_PATH`. macOS has no installer yet.

All scripts accept `setup`, `update`, and `uninstall`; desktop scripts also accept
`rollback`. For unattended confirmation, use Windows `-Yes` or Linux/Termux
`--yes`. `-BaseUrl` / `--base-url` supports HTTPS mirrors and localhost HTTP test
servers. Windows `-NoPersistPath` avoids writing user PATH; `-ConfigDir` isolates
the installation registry during tests.

## Verify

Place current SDK packages and `.sha256` files in local `dist/`, using the names
in the [packaging guide](../../bootstrap_compiler/scripts/README.md). From the
repository root, with Python 3.11 or newer:

```sh
python tools/sdk-setup/tests/installer-test-server.py --prepare
python tools/sdk-setup/tests/installer-test-server.py --port 4197
```

In another PowerShell terminal:

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/sdk-setup/tests/check-installers.ps1
```

For Linux, use the same server on port 4201, then:

```bash
bash tools/sdk-setup/tests/check-installers-linux.sh http://127.0.0.1:4201
```

The loopback server creates a synthetic archive revision by adding a marker to
the actual SDK, keeping the compiler unchanged. Other fixtures exercise checksum
and startup rejection. Generated fixtures and Windows test outputs belong in the
ignored `out/sdk-setup-tests/`; Linux uses a temporary directory.

Windows PowerShell 5.1 and WSL Ubuntu checks cover menus, cancellation, paths with
spaces, directory memory, actual SDK project AOT, update, offline rollback, repeat
setup, rejected downloads/startup, uninstall, configuration cleanup, and unrelated
file retention. Windows also verifies external junctions and launcher cleanup.
Termux checks a real `.deb` checksum, identity, architecture, and package-manager
arguments, with `pkg` mocked. These checks do not establish Android device support.

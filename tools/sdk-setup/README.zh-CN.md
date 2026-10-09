# SDK 安装管理

Windows、Linux 和 Termux 的安装入口。源码在本目录维护；本地网站构建将
`install/` 中的脚本复制到站点的 `/install/`。安装器使用 `https://www.vyxlang.com`
的 SDK 包和 SHA256 文件，网站与 SDK 二进制不加入本目录的源码提交。

## 安装、更新和卸载

Windows PowerShell：

```powershell
& .\tools\sdk-setup\install\windows.ps1
# 也可指定目录；无需管理员权限。
& .\tools\sdk-setup\install\windows.ps1 -InstallDir 'D:\Vyx' -Yes
```

Linux bash：

```bash
bash tools/sdk-setup/install/linux.sh
# 也可指定目录；无需 sudo。
bash tools/sdk-setup/install/linux.sh --install-dir "$HOME/SDKs/Vyx" --yes
. "${XDG_CONFIG_HOME:-$HOME/.config}/vyx/env"
```

首次交互安装可选择默认目录、自定义目录或取消。再次安装读取已保存的位置。
桌面安装会生成 `vyxup`，可在任意项目目录中执行：

```sh
vyxup update
vyxup rollback
vyxup uninstall
```

更新先校验 SHA256 并运行新 SDK 的 `vyxc --version`，再切换稳定入口。
下载、校验或启动失败时继续使用当前 SDK；相同 SDK 不重复下载。保留旧版本，
`rollback` 可离线切回上一版。卸载需确认，移除管理的 SDK、管理入口和环境配置，
保留项目及安装根目录中的无关文件。卸载后重新打开终端以刷新继承的 PATH。

Windows 默认根目录是 `%LOCALAPPDATA%\Vyx`，版本放在 `sdk/<SHA256>`，
使用 `current` 目录联接；`%APPDATA%\Vyx/install.json` 记录所选根目录。
Linux 默认根目录是 `~/.local/share/vyx`，版本放在 `releases/<SHA256>`，
使用 `current` 符号链接；`${XDG_CONFIG_HOME:-$HOME/.config}/vyx/install-root`
记录根目录，`env` 加载 PATH。shell 配置行使用 `# Vyx SDK` 标记。

Windows 卸载不会跟随 SDK 内的目录联接删除外部文件。通过 CMD 启动器卸载时，
隐藏的清理进程等待该 CMD 退出后移除启动器，保留卸载命令的退出码。

Termux 仅支持 Android ARM64、apt 包管理版和固定安装前缀
`/data/data/com.termux/files/usr`，不支持自选目录或桌面版回退命令：

```sh
bash tools/sdk-setup/install/termux.sh
bash tools/sdk-setup/install/termux.sh update
pkg uninstall vyx-sdk-termux
```

安装和更新由 `pkg` 安装已校验的 SDK 包及 clang、lld、Python；卸载只移除
`vyx-sdk-termux`，保留这些工具和项目文件。不设置全局 `LD_LIBRARY_PATH`。

三个脚本均接受 `setup`、`update`、`uninstall` 动作；桌面脚本另有 `rollback`。
无人值守确认参数为 Windows `-Yes`、Linux / Termux `--yes`。
下载地址参数 `-BaseUrl` / `--base-url` 用于镜像或隔离验收，只接受 HTTPS 或 localhost HTTP。
Windows `-NoPersistPath` 禁止写用户 PATH，`-ConfigDir` 用于隔离安装位置登记。
macOS 暂无安装入口。

## 验证

从仓库根目录执行。先把当前 SDK 包及 `.sha256` 放在本地 `dist/`，文件名见
[打包脚本说明](../../bootstrap_compiler/scripts/README.md)。需要 Python 3.11 或更新版本。

```sh
python tools/sdk-setup/tests/installer-test-server.py --prepare
python tools/sdk-setup/tests/installer-test-server.py --port 4197
```

另一 PowerShell 终端执行 Windows 验收：

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/sdk-setup/tests/check-installers.ps1
```

Linux 使用端口 4201 的同一测试服务，再执行：

```bash
bash tools/sdk-setup/tests/check-installers-linux.sh http://127.0.0.1:4201
```

服务只监听 localhost。测试版本夹具给当前真实 SDK 增加一个归档标记，改变包哈希，
不修改编译器；另构造损坏校验和与无法启动的 SDK，检查失败后当前版本保持不变。
夹具及 Windows 验收结果在被忽略的 `out/sdk-setup-tests/`，Linux 使用独立临时目录。

Windows PowerShell 5.1 和 WSL Ubuntu 的验证覆盖目录菜单、取消、含空格路径、
位置记忆、实际 SDK 项目 AOT、更新、离线回退、重复安装、拒绝损坏下载及启动失败、
卸载取消、配置清理和无关文件保留。Windows 还检查外部目录联接与启动器清理。
Termux 检查真实 .deb 的校验和、包身份、架构及安装 / 更新 / 卸载参数，但模拟 `pkg`；
这些检查不等于 Android 实机验证。

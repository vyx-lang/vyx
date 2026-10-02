#!/usr/bin/env python3
"""Vyx 官网部署脚本：构建 -> 上传 -> 校验。

只做这三件事。服务器上的 nginx vhost 与 Let's Encrypt 证书**已经配好**，
站点根固定为 /data/www/www.vyxlang.com，本脚本不碰任何服务器配置。

用法（在 website/ 目录下）：

    python deploy.py                 # 构建 + 上传 + 校验（日常就这一条）
    python deploy.py --skip-build    # 只把当前 dist/ 传上去
    python deploy.py --no-verify     # 跳过线上校验
    python deploy.py --prune         # 上传后删掉远端多余文件（清理旧的 hash 资源）
    python deploy.py --dry-run       # 只打印将要做什么，不连服务器

凭据优先级：命令行参数 > 环境变量 > 同目录下的 .deploy.env（被 Git 忽略，勿入库）

    VYX_SSH_HOST      默认 162.211.183.87
    VYX_SSH_PORT      默认 22
    VYX_SSH_USER      默认 root
    VYX_SSH_PASS      必填（密码）
    VYX_DEPLOY_ROOT   默认 /data/www/www.vyxlang.com
    VYX_DEPLOY_DOMAIN 默认 www.vyxlang.com
    VYX_PYTHON        可选，指定带 paramiko 的 python.exe
    VYX_NODE          可选，指定 node.exe

本机注意：PATH 上的 `python` 是托管版、**没有 paramiko**；脚本会自动改用
D:/miniconda3/python.exe 重跑一次（纯 Python 实现，无需 rsync/sshpass）。
"""

import argparse
import glob
import os
import posixpath
import shlex
import shutil
import stat as statmod
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DIST = os.path.join(HERE, "dist")
ENV_FILE = os.path.join(HERE, ".deploy.env")

# 本机已知的、装了 paramiko 的解释器（按顺序尝试）
PYTHON_CANDIDATES = (
    "D:/miniconda3/python.exe",
    "C:/Users/92703/miniconda3/python.exe",
)

DEFAULTS = {
    "VYX_SSH_HOST": "162.211.183.87",
    "VYX_SSH_PORT": "22",
    "VYX_SSH_USER": "root",
    "VYX_SSH_PASS": "",
    "VYX_DEPLOY_ROOT": "/data/www/www.vyxlang.com",
    "VYX_DEPLOY_DOMAIN": "www.vyxlang.com",
}

CURL_FMT = "%{http_code} %{size_download}"


# --------------------------------------------------------------------------- #
# 输出
# --------------------------------------------------------------------------- #
def step(msg):
    print("==> %s" % msg, flush=True)


def info(msg):
    print("    %s" % msg, flush=True)


def warn(msg):
    print("  ! %s" % msg, flush=True)


def die(msg, code=1):
    print("ERROR: %s" % msg, file=sys.stderr, flush=True)
    raise SystemExit(code)


def human(n):
    return "%.1f KB" % (n / 1024.0) if n < 1024 * 1024 else "%.1f MB" % (n / 1048576.0)


# --------------------------------------------------------------------------- #
# 解释器 / 运行时探测
# --------------------------------------------------------------------------- #
def _has_paramiko(exe):
    try:
        proc = subprocess.run(
            [exe, "-c", "import paramiko"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    return proc.returncode == 0


def load_paramiko():
    """返回 paramiko 模块；当前解释器没有就换一个重跑整支脚本。"""
    try:
        import paramiko  # noqa: F401

        return paramiko
    except ImportError:
        pass

    if os.environ.get("VYX_DEPLOY_REEXEC") == "1":
        die("当前解释器没有 paramiko：%s\n"
            "       请用带 paramiko 的 python 运行，或设 VYX_PYTHON=<python.exe 路径>"
            % sys.executable)

    cands = [os.environ.get("VYX_PYTHON", "")] + list(PYTHON_CANDIDATES)
    for exe in cands:
        if not exe or not os.path.isfile(exe):
            continue
        if os.path.abspath(exe).lower() == os.path.abspath(sys.executable).lower():
            continue
        if _has_paramiko(exe):
            print("==> %s 没有 paramiko，改用 %s 重跑"
                  % (os.path.basename(sys.executable), exe), flush=True)
            env = dict(os.environ, VYX_DEPLOY_REEXEC="1")
            raise SystemExit(subprocess.call([exe, os.path.abspath(__file__)] + sys.argv[1:],
                                             env=env))

    die("找不到带 paramiko 的 python.exe（试过：%s）\n"
        "       可用 VYX_PYTHON=<路径> 指定，或 pip install paramiko"
        % ", ".join(x for x in cands if x))


def find_node():
    cands = [os.environ.get("VYX_NODE", ""), shutil.which("node") or ""]
    cands += sorted(glob.glob("C:/Users/92703/.workbuddy/binaries/node/versions/*/node.exe"),
                    reverse=True)
    cands.append("D:/nodejs/node.exe")
    for exe in cands:
        if exe and os.path.isfile(exe):
            return exe
    die("找不到 node.exe，请设 VYX_NODE=<node.exe 路径>")


# --------------------------------------------------------------------------- #
# 配置
# --------------------------------------------------------------------------- #
def load_env_file(path):
    """把 .deploy.env 里的 KEY=VALUE 灌进 os.environ（不覆盖已有环境变量）。"""
    if not os.path.isfile(path):
        return
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            key = key.strip()
            val = val.strip().strip('"').strip("'")
            if key and key not in os.environ:
                os.environ[key] = val


def resolve_config(args):
    cfg = {key: (os.environ.get(key) or default) for key, default in DEFAULTS.items()}
    overrides = {
        "VYX_SSH_HOST": args.host,
        "VYX_SSH_PORT": str(args.port) if args.port else None,
        "VYX_SSH_USER": args.user,
        "VYX_SSH_PASS": args.password,
        "VYX_DEPLOY_ROOT": args.remote_root,
        "VYX_DEPLOY_DOMAIN": args.domain,
    }
    for key, val in overrides.items():
        if val:
            cfg[key] = val
    return cfg


# --------------------------------------------------------------------------- #
# 构建 / 本地清单
# --------------------------------------------------------------------------- #
def build_site():
    vite_bin = os.path.join(HERE, "node_modules", "vite", "bin", "vite.js")
    if not os.path.isfile(vite_bin):
        die("没装依赖，先在 website/ 跑 `npm ci`")
    node = find_node()
    step("构建站点（%s）" % os.path.basename(node))
    if subprocess.call([node, vite_bin, "build"], cwd=HERE) != 0:
        die("vite build 失败")


def local_inventory(dist):
    if not os.path.isdir(dist):
        die("本地产物目录不存在：%s（先构建，或去掉 --skip-build）" % dist)
    inv = {}
    for root, _dirs, files in os.walk(dist):
        for name in files:
            full = os.path.join(root, name)
            inv[os.path.relpath(full, dist).replace("\\", "/")] = os.path.getsize(full)
    if not inv:
        die("本地产物目录是空的：%s" % dist)
    return inv


# --------------------------------------------------------------------------- #
# SSH / SFTP（断线自动重连）
# --------------------------------------------------------------------------- #
class Session:
    """一条 SSH 连接 + SFTP 通道；传输层断开时自动重连并重试当前动作。"""

    def __init__(self, paramiko, cfg, retries=4):
        self._paramiko = paramiko
        self._cfg = cfg
        self._retries = retries
        self.client = None
        self.sftp = None

    # -- 连接管理 ------------------------------------------------------------ #
    def connect(self):
        self.close()
        client = self._paramiko.SSHClient()
        client.set_missing_host_key_policy(self._paramiko.AutoAddPolicy())
        client.connect(
            self._cfg["VYX_SSH_HOST"],
            port=int(self._cfg["VYX_SSH_PORT"]),
            username=self._cfg["VYX_SSH_USER"],
            password=self._cfg["VYX_SSH_PASS"],
            timeout=30,
            banner_timeout=30,
            auth_timeout=30,
            look_for_keys=False,
            allow_agent=False,
        )
        transport = client.get_transport()
        if transport is not None:
            transport.set_keepalive(15)
        self.client = client
        self.sftp = client.open_sftp()
        return self

    def close(self):
        for obj in (self.sftp, self.client):
            try:
                if obj is not None:
                    obj.close()
            except Exception:
                pass
        self.sftp = None
        self.client = None

    # -- 重试包装 ------------------------------------------------------------ #
    def _retry(self, what, fn):
        last = None
        for attempt in range(1, self._retries + 1):
            try:
                if self.sftp is None:
                    self.connect()
                return fn()
            except Exception as exc:  # 传输层任何异常都当断线处理
                if not isinstance(exc, (self._paramiko.SSHException, EOFError, OSError)):
                    raise
                last = exc
                self.close()
                if attempt == self._retries:
                    break
                warn("%s 失败（%s），2s 后重连重试（%d/%d）"
                     % (what, type(exc).__name__, attempt, self._retries - 1))
                time.sleep(2)
        raise last

    # -- 操作 ---------------------------------------------------------------- #
    def mkdirs(self, remote_dir):
        def go():
            parts, head = [], remote_dir
            while head and head != "/":
                parts.append(head)
                head = posixpath.dirname(head)
            for path in reversed(parts):
                try:
                    self.sftp.stat(path)
                except IOError:
                    self.sftp.mkdir(path)

        return self._retry("建目录 %s" % remote_dir, go)

    def put(self, local, remote):
        def go():
            self.mkdirs(posixpath.dirname(remote))
            self.sftp.put(local, remote)

        return self._retry("上传 %s" % posixpath.basename(remote), go)

    def run(self, command, timeout=600):
        def go():
            _stdin, stdout, stderr = self.client.exec_command(command, timeout=timeout,
                                                              get_pty=False)
            out = stdout.read().decode("utf-8", "replace")
            err = stderr.read().decode("utf-8", "replace")
            return stdout.channel.recv_exit_status(), out, err

        return self._retry("远端命令", go)

    def remote_inventory(self, remote_root):
        def go():
            inv, dirs = {}, []

            def walk(path, prefix):
                for entry in self.sftp.listdir_attr(path):
                    child = posixpath.join(path, entry.filename)
                    rel = prefix + entry.filename
                    if statmod.S_ISDIR(entry.st_mode):
                        dirs.append(child)
                        walk(child, rel + "/")
                    else:
                        inv[rel] = entry.st_size

            walk(remote_root, "")
            return inv, dirs

        return self._retry("列远端目录", go)

    def remove(self, remote_path):
        return self._retry("删除 %s" % remote_path, lambda: self.sftp.remove(remote_path))

    def rmdir(self, remote_path):
        try:
            self.sftp.rmdir(remote_path)
        except IOError:
            pass


# --------------------------------------------------------------------------- #
# 部署
# --------------------------------------------------------------------------- #
def upload(session, dist, remote_root, inv):
    session._retry("检查远端目录", lambda: session.sftp.stat(remote_root))
    sent = 0
    for rel in sorted(inv):
        session.put(os.path.join(dist, rel.replace("/", os.sep)),
                    posixpath.join(remote_root, rel))
        sent += inv[rel]
    return sent


def prune(session, remote_root, dirs, extra):
    for rel in sorted(extra):
        session.remove(posixpath.join(remote_root, rel))
    for path in sorted(dirs, key=len, reverse=True):
        session.rmdir(path)


# --------------------------------------------------------------------------- #
# 校验
# --------------------------------------------------------------------------- #
def curl_cmd(url, resolve=None):
    parts = ["curl", "-sS", "--max-time", "20", "-o", "/dev/null", "-w", CURL_FMT]
    if resolve:
        parts += ["--resolve", resolve]
    parts.append(url)
    return " ".join(shlex.quote(p) for p in parts)


def verify(session, cfg, inv):
    domain = cfg["VYX_DEPLOY_DOMAIN"]
    remote_root = cfg["VYX_DEPLOY_ROOT"]
    ok = True

    step("校验远端文件清单")
    rc, out, err = session.run("find " + shlex.quote(remote_root)
                               + " -type f -printf '%P %s\\n' | sort")
    if rc != 0:
        warn("远端 find 失败：%s" % err.strip())
        return False
    remote = {}
    for line in out.splitlines():
        bits = line.split(None, 1)
        if len(bits) == 2 and bits[1].strip().isdigit():
            remote[bits[0]] = int(bits[1])

    missing = [k for k in inv if k not in remote]
    size_bad = [(k, inv[k], remote[k]) for k in sorted(inv)
                if k in remote and remote[k] != inv[k]]
    extra = [k for k in sorted(remote) if k not in inv]

    info("本地 %d 个文件 / %s，远端 %d 个文件"
         % (len(inv), human(sum(inv.values())), len(remote)))
    if missing:
        ok = False
        warn("远端缺少 %d 个文件：%s" % (len(missing), ", ".join(missing[:5])))
    if size_bad:
        ok = False
        for rel, lsize, rsize in size_bad[:5]:
            warn("大小不一致 %s：本地 %d / 远端 %d" % (rel, lsize, rsize))
    if extra:
        warn("远端多出 %d 个文件（旧 hash 资源，可用 --prune 清理）：%s"
             % (len(extra), ", ".join(extra[:5])))
    if not missing and not size_bad:
        info("远端内容与本地 dist 一致")

    step("校验线上响应")
    main_js = sorted((k for k in inv if k.startswith("assets/") and k.endswith(".js")),
                     key=lambda k: -inv[k])[:1]
    # 三个入口都要能被直接访问：首页、独立 MOSP 页、教程站。
    # 教程站是哈希路由，正文 JSON 是懒加载的分包，所以额外点名一个「章节包」（正文
    # chunk），否则「入口能开、点进某一章 404」这类问题会漏过去。
    chapter_prefixes = ("basics.", "intermediate.", "advanced.", "migration.")
    chapter_chunks = sorted((k for k in inv
                             if k.startswith("assets/") and k.endswith(".js")
                             and os.path.basename(k).startswith(chapter_prefixes)),
                            key=lambda k: -inv[k])[:1]
    for path in ["/", "/mosp.html", "/tutorial/", "/vyx.png"] \
            + ["/" + p for p in main_js + chapter_chunks]:
        url = "https://%s%s" % (domain, path)
        _rc, origin, _ = session.run(curl_cmd(url, resolve="%s:443:127.0.0.1" % domain))
        _rc, public, _ = session.run(curl_cmd(url))
        origin, public = origin.strip() or "?", public.strip() or "?"
        good = origin.split()[0:1] == ["200"] and public.split()[0:1] == ["200"]
        ok = ok and good
        print("  %s %-26s 源站 %-14s 公网 %-14s"
              % ("v" if good else "x", path, origin, public), flush=True)

    return ok


# --------------------------------------------------------------------------- #
def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="deploy.py",
        description="Vyx 官网部署：构建 -> 上传 -> 校验（不含 nginx/证书配置）",
    )
    parser.add_argument("--skip-build", action="store_true", help="跳过 vite build，直接上传现有 dist/")
    parser.add_argument("--no-verify", action="store_true", help="跳过上传后的线上校验")
    parser.add_argument("--prune", action="store_true", help="上传后删除远端多余文件（清理旧 hash 资源）")
    parser.add_argument("--dry-run", action="store_true", help="只打印计划，不连服务器")
    parser.add_argument("--dist", default=DEFAULT_DIST, help="本地产物目录（默认 website/dist）")
    parser.add_argument("--host", help="覆盖 VYX_SSH_HOST")
    parser.add_argument("--port", help="覆盖 VYX_SSH_PORT")
    parser.add_argument("--user", help="覆盖 VYX_SSH_USER")
    parser.add_argument("--password", help="覆盖 VYX_SSH_PASS（默认走环境变量或 .deploy.env）")
    parser.add_argument("--remote-root", help="覆盖 VYX_DEPLOY_ROOT")
    parser.add_argument("--domain", help="覆盖 VYX_DEPLOY_DOMAIN")
    args = parser.parse_args(argv)

    if not args.skip_build:
        build_site()

    load_env_file(ENV_FILE)
    cfg = resolve_config(args)
    dist = os.path.abspath(args.dist)
    inv = local_inventory(dist)

    if args.dry_run:
        step("部署目标（dry-run，不连服务器）")
        info("%s@%s:%s" % (cfg["VYX_SSH_USER"], cfg["VYX_SSH_HOST"], cfg["VYX_SSH_PORT"]))
        info("远端 %s  <-  本地 %s" % (cfg["VYX_DEPLOY_ROOT"], dist))
        info("%d 个文件 / %s" % (len(inv), human(sum(inv.values()))))
        for rel in sorted(inv):
            info("  %s" % rel)
        return 0

    if not cfg["VYX_SSH_PASS"]:
        die("没有密码：设环境变量 VYX_SSH_PASS，或写进 %s" % ENV_FILE)

    paramiko = load_paramiko()

    step("部署目标")
    info("%s@%s:%s" % (cfg["VYX_SSH_USER"], cfg["VYX_SSH_HOST"], cfg["VYX_SSH_PORT"]))
    info("远端 %s  <-  本地 %s" % (cfg["VYX_DEPLOY_ROOT"], dist))
    info("%d 个文件 / %s" % (len(inv), human(sum(inv.values()))))

    session = Session(paramiko, cfg)
    try:
        step("连接")
        try:
            session.connect()
        except Exception as exc:
            die("连接 %s 失败：%s: %s" % (cfg["VYX_SSH_HOST"], type(exc).__name__, exc))
        info("已连接")

        step("上传")
        sent = upload(session, dist, cfg["VYX_DEPLOY_ROOT"], inv)
        info("已上传 %d 个文件 / %s" % (len(inv), human(sent)))

        if args.prune:
            step("清理远端多余文件")
            rinv, rdirs = session.remote_inventory(cfg["VYX_DEPLOY_ROOT"])
            extra = [k for k in rinv if k not in inv]
            if extra:
                prune(session, cfg["VYX_DEPLOY_ROOT"], rdirs, extra)
                info("删除 %d 个文件" % len(extra))
            else:
                info("远端没有多余文件")

        if args.no_verify:
            print("==> 完成（已跳过校验）", flush=True)
            return 0

        ok = verify(session, cfg, inv)
    finally:
        session.close()

    if ok:
        print("==> 部署完成：https://%s/" % cfg["VYX_DEPLOY_DOMAIN"], flush=True)
        return 0
    print("==> 上传成功，但校验未全过，请看上面的 x 行", file=sys.stderr, flush=True)
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)

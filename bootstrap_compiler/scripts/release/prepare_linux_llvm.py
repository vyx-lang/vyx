#!/usr/bin/env python3
"""Install LLVM 22 development packages on the Ubuntu 24.04 release runners."""
from pathlib import Path
import subprocess
import sys
import tempfile


KEY_URL = "https://apt.llvm.org/llvm-snapshot.gpg.key"
KEYRING = "/usr/share/keyrings/llvm.gpg"
SOURCE = "/etc/apt/sources.list.d/llvm22.list"
PACKAGES = (
    "clang-22", "llvm-22-dev", "lld-22", "libclang-cpp22-dev", "cmake",
    "ninja-build", "git", "patchelf", "zlib1g-dev", "libzstd-dev", "libxml2-dev",
)


def run(command):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True)


def prepare():
    with tempfile.TemporaryDirectory(prefix="vyx-llvm-apt-") as temporary:
        work = Path(temporary)
        key = work / "llvm.asc"
        keyring = work / "llvm.gpg"
        # --retry alone does not cover every transport error, including DNS.
        # Use curl's output file so partial downloads are reset on retry.
        # Bound each attempt and the retry window; never pipe failed data to GPG.
        run(["curl", "--fail", "--silent", "--show-error", "--location",
             "--retry", "5", "--retry-all-errors", "--retry-delay", "5",
             "--retry-max-time", "180", "--connect-timeout", "15", "--max-time", "45",
             "--output", key, KEY_URL])
        run(["gpg", "--batch", "--yes", "--dearmor", "--output", keyring, key])
        source = work / "llvm22.list"
        source.write_text(
            f"deb [signed-by={KEYRING}] https://apt.llvm.org/noble/ llvm-toolchain-noble-22 main\n",
            encoding="utf-8")
        run(["sudo", "install", "-m", "0644", keyring, KEYRING])
        run(["sudo", "install", "-m", "0644", source, SOURCE])
        apt = ["sudo", "apt-get", "-o", "Acquire::Retries=3",
               "-o", "Acquire::http::Timeout=30", "-o", "Acquire::https::Timeout=30"]
        # APT can otherwise exit zero while a repository index failed to load.
        run([*apt, "--error-on=any", "update"])
        run([*apt, "install", "-y", *PACKAGES])


def main():
    if sys.platform != "linux":
        raise SystemExit("LLVM APT preparation requires an Ubuntu Linux release runner")
    try:
        prepare()
    except subprocess.CalledProcessError as error:
        print(f"error: Linux LLVM dependency preparation failed at {error.cmd[0]} "
              f"(exit {error.returncode})", file=sys.stderr)
        raise SystemExit(error.returncode)


if __name__ == "__main__":
    main()

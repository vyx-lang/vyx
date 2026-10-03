#!/usr/bin/env bash
# Idempotent setup of a separate LLVM 22.x compiler toolchain for building Vyx
# on Debian/Ubuntu. Development builds should use the repository-root `clang/`
# SDK first; this provisioning script is only for a host where that SDK is not
# available. It mirrors the layout pinned in the repository Dockerfile
# (LLVM_VERSION=22, LLVM_ROOT=/usr/lib/llvm-22) and the LLVM 22.1 requirement
# in CMakeLists.txt (find_package(LLVM 22.1 REQUIRED CONFIG)).
#
# Usage:  sudo bash scripts/setup_llvm22.sh
# Re-running is safe: existing packages/symlinks are left in place.

set -euo pipefail

LLVM_VERSION=22
LLVM_ROOT=/usr/lib/llvm-${LLVM_VERSION}
LLVM_DIR=${LLVM_ROOT}/lib/cmake/llvm

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

export DEBIAN_FRONTEND=noninteractive

echo "==> Installing prerequisites"
$SUDO apt-get update -y
# build-essential + libstdc++-14-dev provide the libstdc++ link library that
# clang-22 selects by default (it prefers the newest GCC dir, i.e. 14).
$SUDO apt-get install -y --no-install-recommends \
    wget lsb-release software-properties-common gnupg ca-certificates \
    zlib1g-dev libzstd-dev build-essential libstdc++-14-dev

echo "==> Installing LLVM ${LLVM_VERSION} via apt.llvm.org (all components)"
if ! command -v clang-${LLVM_VERSION} >/dev/null 2>&1; then
    wget -qO- https://apt.llvm.org/llvm.sh | $SUDO bash -s -- ${LLVM_VERSION} all
fi

echo "==> Installing SDK packages required by Vyx"
$SUDO apt-get update -y
$SUDO apt-get install -y --no-install-recommends \
    llvm-${LLVM_VERSION} \
    llvm-${LLVM_VERSION}-dev \
    llvm-${LLVM_VERSION}-tools \
    llvm-${LLVM_VERSION}-runtime \
    clang-${LLVM_VERSION} \
    libclang-${LLVM_VERSION}-dev \
    libclang-common-${LLVM_VERSION}-dev \
    lld-${LLVM_VERSION} \
    libc++-${LLVM_VERSION}-dev \
    libc++abi-${LLVM_VERSION}-dev \
    cmake ninja-build

echo "==> Creating convenience symlinks in /usr/local/bin"
$SUDO ln -sf /usr/bin/clang-${LLVM_VERSION}       /usr/local/bin/clang
$SUDO ln -sf /usr/bin/clang++-${LLVM_VERSION}     /usr/local/bin/clang++
$SUDO ln -sf /usr/bin/llvm-ar-${LLVM_VERSION}     /usr/local/bin/llvm-ar
$SUDO ln -sf /usr/bin/llvm-config-${LLVM_VERSION} /usr/local/bin/llvm-config

echo "==> Persisting environment to /etc/profile.d/vyx-llvm22.sh"
$SUDO tee /etc/profile.d/vyx-llvm22.sh >/dev/null <<EOF
# Vyx LLVM ${LLVM_VERSION} toolchain environment (managed by scripts/setup_llvm22.sh)
export CC=clang-${LLVM_VERSION}
export CXX=clang++-${LLVM_VERSION}
export LLVM_VERSION=${LLVM_VERSION}
export LLVM_ROOT=${LLVM_ROOT}
export LLVM_DIR=${LLVM_DIR}
case ":\$PATH:" in
  *:${LLVM_ROOT}/bin:*) ;;
  *) export PATH="${LLVM_ROOT}/bin:\$PATH" ;;
esac
case ":\$PATH:" in
  *:/usr/local/bin:*) ;;
  *) export PATH="/usr/local/bin:\$PATH" ;;
esac
EOF
$SUDO chmod 0644 /etc/profile.d/vyx-llvm22.sh

echo "==> Verification"
clang --version
llvm-config --version
llvm-config --prefix
ls "${LLVM_DIR}"

echo "==> LLVM ${LLVM_VERSION} toolchain ready. Open a new shell or 'source /etc/profile.d/vyx-llvm22.sh'."

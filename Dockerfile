FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV LLVM_VERSION=22

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates \
    wget lsb-release software-properties-common gnupg \
    python3 zlib1g-dev libzstd-dev && \
    rm -rf /var/lib/apt/lists/*

RUN wget -qO- https://apt.llvm.org/llvm.sh | bash -s -- ${LLVM_VERSION} all && \
    apt-get update && apt-get install -y --no-install-recommends \
    llvm-${LLVM_VERSION}-dev \
    libclang-${LLVM_VERSION}-dev \
    clang-${LLVM_VERSION} \
    lld-${LLVM_VERSION} && \
    ln -sf /usr/bin/clang-${LLVM_VERSION} /usr/local/bin/clang && \
    ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/local/bin/clang++ && \
    ln -sf /usr/bin/llvm-ar-${LLVM_VERSION} /usr/local/bin/llvm-ar && \
    rm -rf /var/lib/apt/lists/*

ENV CC=clang-${LLVM_VERSION}
ENV CXX=clang++-${LLVM_VERSION}
ENV LLVM_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/llvm
ENV LLVM_ROOT=/usr/lib/llvm-${LLVM_VERSION}
ENV LLVM_BIN=/usr/local/bin

WORKDIR /vyx
COPY . .

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR=${LLVM_DIR} \
    -DCMAKE_CXX_STANDARD=26 && \
    cmake --build build --target vyxc vyx

RUN build/vyxc --help 2>&1 | head -5 || true

# --- Runtime image ---
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LLVM_VERSION=22

RUN apt-get update && apt-get install -y --no-install-recommends \
    wget lsb-release software-properties-common gnupg ca-certificates && \
    wget -qO- https://apt.llvm.org/llvm.sh | bash -s -- ${LLVM_VERSION} all && \
    apt-get update && apt-get install -y --no-install-recommends \
        llvm-${LLVM_VERSION}-dev libclang-${LLVM_VERSION}-dev \
        lld-${LLVM_VERSION} clang-${LLVM_VERSION} \
        zlib1g-dev libzstd-dev zlib1g libzstd1 && \
        ln -sf /usr/bin/clang-${LLVM_VERSION} /usr/local/bin/clang && \
        ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/local/bin/clang++ && \
        ln -sf /usr/bin/llvm-ar-${LLVM_VERSION} /usr/local/bin/llvm-ar && \
        rm -rf /var/lib/apt/lists/*

WORKDIR /vyx

COPY --from=builder /vyx/build/vyxc /usr/local/bin/vyxc
COPY --from=builder /vyx/build/vyx /usr/local/bin/vyx
COPY --from=builder /vyx/std/ /usr/local/share/vyx/std/

ENV PATH="/usr/local/bin:${PATH}"
ENV LLVM_ROOT=/usr/lib/llvm-${LLVM_VERSION}
ENV LLVM_BIN=/usr/local/bin

ENTRYPOINT ["vyxc"]

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV EDITOR=nano VISUAL=nano

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    bash-completion \
    cmake \
    clang-format-18 \
    pre-commit \
    ninja-build \
    gdb \
    strace \
    lsof \
    procps \
    iproute2 \
    net-tools \
    curl \
    ca-certificates \
    git \
    nano \
    openssh-client \
    && rm -rf /var/lib/apt/lists/*

RUN git config --global core.editor nano

WORKDIR /workspace

COPY docker/bashrc /root/.bashrc

CMD ["bash"]

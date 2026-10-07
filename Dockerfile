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

RUN groupadd --system telemetry \
    && useradd \
        --system \
        --gid telemetry \
        --home-dir /var/lib/telemetry-server \
        --shell /usr/sbin/nologin \
        telemetry \
    && mkdir -p /var/lib/telemetry-server \
    && chown telemetry:telemetry /var/lib/telemetry-server \
    && chmod 0750 /var/lib/telemetry-server

RUN git config --global core.editor nano

WORKDIR /workspace

COPY docker/bashrc /root/.bashrc

CMD ["bash"]

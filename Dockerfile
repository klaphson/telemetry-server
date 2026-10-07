FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV EDITOR=nano VISUAL=nano GIT_EDITOR=nano

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

# The client uses the host's Docker engine through the mounted socket.
RUN install -m 0755 -d /etc/apt/keyrings \
    && curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc \
    && chmod a+r /etc/apt/keyrings/docker.asc \
    && printf '%s\n' \
        'Types: deb' \
        'URIs: https://download.docker.com/linux/ubuntu' \
        'Suites: noble' \
        'Components: stable' \
        "Architectures: $(dpkg --print-architecture)" \
        'Signed-By: /etc/apt/keyrings/docker.asc' \
        > /etc/apt/sources.list.d/docker.sources \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        docker-ce-cli docker-buildx-plugin docker-compose-plugin \
    && rm -rf /var/lib/apt/lists/*

RUN git config --global core.editor nano

WORKDIR /workspace

COPY docker/bashrc /root/.bashrc

CMD ["bash"]

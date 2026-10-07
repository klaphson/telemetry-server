#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

missing_packages=()
command -v pre-commit >/dev/null 2>&1 || missing_packages+=(pre-commit)
command -v clang-format-18 >/dev/null 2>&1 || missing_packages+=(clang-format-18)
command -v nano >/dev/null 2>&1 || missing_packages+=(nano)
[[ -r /usr/share/bash-completion/bash_completion ]] || missing_packages+=(bash-completion)

docker_packages=()
command -v docker >/dev/null 2>&1 || docker_packages+=(docker-ce-cli)
docker compose version >/dev/null 2>&1 || docker_packages+=(docker-compose-plugin)
docker buildx version >/dev/null 2>&1 || docker_packages+=(docker-buildx-plugin)

if ((${#docker_packages[@]})); then
    # Existing containers may predate the Docker CLI installation in the image.
    source /etc/os-release
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
    chmod a+r /etc/apt/keyrings/docker.asc
    printf '%s\n' \
        'Types: deb' \
        'URIs: https://download.docker.com/linux/ubuntu' \
        "Suites: ${UBUNTU_CODENAME:-$VERSION_CODENAME}" \
        'Components: stable' \
        "Architectures: $(dpkg --print-architecture)" \
        'Signed-By: /etc/apt/keyrings/docker.asc' \
        > /etc/apt/sources.list.d/docker.sources
    missing_packages+=("${docker_packages[@]}")
fi

if ((${#missing_packages[@]})); then
    printf 'Installing missing development tools: %s\n' "${missing_packages[*]}"
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing_packages[@]}"
fi

git config --global core.editor nano

# Keep existing containers in sync with the Bash configuration used by new images.
if ! cmp -s docker/bashrc "$HOME/.bashrc"; then
    install -m 0644 docker/bashrc "$HOME/.bashrc"
fi

pre-commit install

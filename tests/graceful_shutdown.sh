#!/usr/bin/env bash

set -euo pipefail

log=$(mktemp)
server_pid=

cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill -KILL "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi

    rm -f "$log"
}

trap cleanup EXIT

"$1" >"$log" 2>&1 &
server_pid=$!

for ((attempt = 0; attempt < 100; ++attempt)); do
    if grep -q 'listening on' "$log"; then
        break
    fi

    if ! kill -0 "$server_pid" 2>/dev/null; then
        cat "$log"
        exit 1
    fi

    sleep 0.05
done

grep -q 'listening on' "$log" || {
    cat "$log"
    exit 1
}

exec 3<>/dev/tcp/127.0.0.1/9000

printf 'temperature=23\n' >&3

exec 3>&-

for ((attempt = 0; attempt < 100; ++attempt)); do
    if grep -qF \
        '[reader] telemetry: temperature=23' \
        "$log"; then
        break
    fi

    sleep 0.05
done

grep -qF \
    '[reader] telemetry: temperature=23' \
    "$log"

kill -TERM "$server_pid"

if ! wait "$server_pid"; then
    cat "$log"
    echo 'Expected graceful shutdown to succeed'
    exit 1
fi

server_pid=

grep -qF \
    '[server] shutdown requested signal=' \
    "$log"

grep -qF \
    '[server] entering draining state' \
    "$log"

grep -qF \
    '[reader] EOF' \
    "$log"
#!/usr/bin/env bash

set -euo pipefail

log=$(mktemp)
server_pid=

cleanup() {
    local status=$?

    if [[ -n "$server_pid" ]]; then
        kill -KILL "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi

    if ((status != 0)); then
        cat "$log" >&2
    fi
    rm -f "$log"
    exit "$status"
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

printf '\x54\x4c\x52\x59\x00\x01\x00\x20\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x3f\xf0\x00\x00\x00\x00\x00\x00' >&3

exec 3>&-

for ((attempt = 0; attempt < 100; ++attempt)); do
    if grep -qF \
        '[reader] telemetry: sensor=1 metric=2 timestamp_ns=3 value=1' \
        "$log"; then
        break
    fi

    sleep 0.05
done

grep -qF \
    '[reader] telemetry: sensor=1 metric=2 timestamp_ns=3 value=1' \
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

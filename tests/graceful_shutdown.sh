#!/usr/bin/env bash

set -euo pipefail

log=$(mktemp)
server_pid=
reader_pid=

data_file=$(mktemp)
rm -f "$data_file"

cleanup() {
    local status=$?

    if [[ -n "$reader_pid" ]]; then
        kill -KILL "$reader_pid" 2>/dev/null || true
    fi
    if [[ -n "$server_pid" ]]; then
        kill -KILL "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi

    if ((status != 0)); then
        cat "$log" >&2
    fi
    rm -f "$log"
    rm -f "$data_file"
    exit "$status"
}

trap cleanup EXIT

wait_for_log() {
    local message=$1
    local attempt

    for ((attempt = 0; attempt < 100; ++attempt)); do
        if grep -qF "$message" "$log"; then
            return 0
        fi
        if ! kill -0 "$server_pid" 2>/dev/null; then
            break
        fi
        sleep 0.05
    done

    echo "Expected log message: $message" >&2
    return 1
}

start_server() {
    "$1" --data-file "$data_file" >"$log" 2>&1 &
    server_pid=$!
    wait_for_log 'listening on'
    reader_pid=$(cat "/proc/$server_pid/task/$server_pid/children")
    reader_pid=${reader_pid// /}
    [[ "$reader_pid" =~ ^[0-9]+$ ]]
    wait_for_log '[reader] reading binary telemetry from pipe'
}

# Bound waits even when this script is run outside CTest.
wait_for_exit() {
    local expected_status=$1
    local actual_status=0
    local attempt

    for ((attempt = 0; attempt < 100; ++attempt)); do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            wait "$server_pid" || actual_status=$?
            server_pid=
            reader_pid=
            if ((actual_status != expected_status)); then
                echo "Expected exit status $expected_status, got $actual_status" >&2
                return 1
            fi
            return 0
        fi
        sleep 0.05
    done

    echo 'Timed out waiting for the server to exit' >&2
    return 1
}

start_server "$1"

# Nonterminal SIGCHLD events must not be treated as reader failure or shutdown.
kill -STOP "$reader_pid"
wait_for_log "[server] reader stopped signal=$(kill -l STOP)"
kill -0 "$server_pid"
kill -CONT "$reader_pid"
wait_for_log '[server] reader continued'

exec 3<>/dev/tcp/127.0.0.1/9000

printf '\x54\x4c\x52\x59\x00\x01\x00\x20\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x3f\xf0\x00\x00\x00\x00\x00\x00' >&3

exec 3>&-

wait_for_log '[reader] telemetry: sensor=1 metric=2 timestamp_ns=3 value=1'

kill -TERM "$server_pid"

wait_for_exit 0

[[ -f "$data_file" ]]
size=$(stat -c '%s' "$data_file")
[[ "$size" -eq 32 ]]

grep -qF \
    '[server] shutdown requested signal=' \
    "$log"

grep -qF \
    '[server] entering draining state' \
    "$log"

grep -qF \
    '[reader] EOF' \
    "$log"

# Losing an idle reader must fail without another client write or shutdown signal.
start_server "$1"
kill -KILL "$reader_pid"
wait_for_exit 1
grep -qF "[main] reader terminated by signal=$(kill -l KILL)" "$log"

# A storage-open failure is a normal child exit, not a signal termination.
# The persisted data file is a regular file, so it cannot be a parent directory.
"$1" --data-file "$data_file/invalid" >"$log" 2>&1 &
server_pid=$!
wait_for_exit 1
grep -qF '[main] reader exited code=1' "$log"

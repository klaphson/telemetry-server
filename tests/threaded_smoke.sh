#!/usr/bin/env bash
set -euo pipefail

folder=$(mktemp -d)
log="$folder/server.log"
data="$folder/telemetry.bin"
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
    rm -rf "$folder"
    exit "$status"
}
trap cleanup EXIT

wait_for_log() {
    local message=$1
    for ((attempt = 0; attempt < 100; ++attempt)); do
        if grep -qF "$message" "$log"; then
            return 0
        fi
        kill -0 "$server_pid" 2>/dev/null || break
        sleep 0.05
    done
    echo "Expected log message: $message" >&2
    return 1
}

wait_for_exit() {
    local expected=$1
    local actual=0
    for ((attempt = 0; attempt < 100; ++attempt)); do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            wait "$server_pid" || actual=$?
            server_pid=
            if ((actual != expected)); then
                echo "Expected exit status $expected, got $actual" >&2
                return 1
            fi
            return 0
        fi
        sleep 0.05
    done
    echo "Timed out waiting for reader-mode thread to exit" >&2
    return 1
}

frame='\x54\x4c\x52\x59\x00\x01\x00\x20\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x3f\xf0\x00\x00\x00\x00\x00\x00'

"$1" --reader-mode thread --data-file "$data" >"$log" 2>&1 &
server_pid=$!
wait_for_log 'listening on'
wait_for_log '[reader] reading telemetry from shared memory'
# When procfs exposes the child list, verify that Reader is not a process.
children="/proc/$server_pid/task/$server_pid/children"
if [[ -r "$children" ]]; then
    [[ -z "$(cat "$children")" ]]
fi

exec 3<>/dev/tcp/127.0.0.1/9000
printf '%b' "$frame" >&3
exec 3>&-
wait_for_log '[reader] telemetry: sensor=1 metric=2 timestamp_ns=3 value=1'

kill -TERM "$server_pid"
wait_for_exit 0
grep -qF '[reader] EOF' "$log"
printf '%b' "$frame" | cmp - "$data"

# Reader can fail before Server starts waiting for events.
"$1" --reader-mode thread --data-file "$folder/missing/telemetry.bin" >"$log" 2>&1 &
server_pid=$!
wait_for_exit 1
grep -qF '[main] reader exited code=1' "$log"

# Trigger a write failure after the threads are running. Ignore SIGXFSZ so
# that the write returns EFBIG and Reader reports its own failure.
(
    trap '' XFSZ
    ulimit -f 1
    exec "$1" --reader-mode thread --data-file "$folder/limited.bin" --quiet
) >"$log" 2>&1 &
server_pid=$!
wait_for_log 'listening on'
wait_for_log '[reader] reading telemetry from shared memory'

payload=
for ((i = 0; i < 128; ++i)); do
    payload+="$frame"
done
exec 3<>/dev/tcp/127.0.0.1/9000
# Server may close the connection as soon as Reader reports the write failure.
printf '%b' "$payload" >&3 || true
exec 3>&-

wait_for_exit 1
grep -qF 'write telemetry file:' "$log"
grep -qF '[server] reader thread stopped code=1' "$log"
grep -qF '[main] reader exited code=1' "$log"

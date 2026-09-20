#!/usr/bin/env bash
set -euo pipefail

log=$(mktemp)
server_pid=
reader_pid=
cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    if [[ -n "$reader_pid" ]]; then
        kill -KILL "$reader_pid" 2>/dev/null || true
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
grep -q 'listening on' "$log" || { cat "$log"; exit 1; }
reader_pid=$(cat "/proc/$server_pid/task/$server_pid/children")
reader_pid=${reader_pid// /}
[[ -n "$reader_pid" ]]

exec 3<>/dev/tcp/127.0.0.1/9000
printf 'temperature=' >&3
sleep 0.05
printf '23\r\nhumidity=50\n' >&3
exec 3>&-
for ((attempt = 0; attempt < 100; ++attempt)); do
    if grep -qF '[reader] telemetry: temperature=23' "$log" &&
       grep -qF '[reader] telemetry: humidity=50' "$log"; then
        break
    fi
    sleep 0.05
done
grep -qF '[reader] telemetry: temperature=23' "$log"
grep -qF '[reader] telemetry: humidity=50' "$log"

# A failed reader must wake the server and produce a failed process exit.
kill -KILL "$reader_pid"
reader_pid=
if wait "$server_pid"; then
    echo 'Expected failure after terminating the reader'
    exit 1
fi
server_pid=
grep -qF '[main] reader failed' "$log"

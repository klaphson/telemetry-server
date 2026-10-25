#!/usr/bin/env bash
set -euo pipefail

frames=${1:-100000}
batch=${2:-128}
port=${3:-19000}

for value in "$frames" "$batch" "$port"; do
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "Expected positive integers" >&2; exit 2; }
done
if ((frames > 1000000 || batch > 4096 || port > 65535)); then
    echo "Limits: frames <= 1000000, batch <= 4096, port <= 65535" >&2
    exit 2
fi

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
server="${SERVER_BIN:-$root/build-bench/telemetry-server}"
sender="${SENDER_BIN:-$root/build-bench/telemetry-load}"
[[ -x "$server" && -x "$sender" ]] || { echo "Run make benchmark first" >&2; exit 2; }
work=$(mktemp -d)
server_pid=
reader_pid=

cleanup() {
    local result=$?
    if [[ -n "$reader_pid" ]]; then
        kill -KILL "$reader_pid" 2>/dev/null || true
    fi
    if [[ -n "$server_pid" ]]; then
        kill -KILL "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    if ((result != 0)); then
        echo "Server log:" >&2
        tail -n 50 "$work/server.log" >&2 || true
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT

data="$work/telemetry.bin"
expected=$((frames * 32))

"$server" --port "$port" --data-file "$data" --quiet >"$work/server.log" 2>&1 &
server_pid=$!

ready=false
for ((i=0; i<200; ++i)); do
    if grep -qF '[server] listening on' "$work/server.log"; then ready=true; break; fi
    if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
    sleep 0.05
done
[[ "$ready" == true ]] || { echo "Server did not start" >&2; exit 1; }
reader_pid=$(cat "/proc/$server_pid/task/$server_pid/children" 2>/dev/null || true)
reader_pid=${reader_pid// /}

start=$(date +%s%N)
"$sender" --frames "$frames" --batch-frames "$batch" --port "$port"

# TCP send completion alone does not prove storage completion.
persisted=false
for ((i=0; i<2400; ++i)); do
    if [[ -f "$data" ]]; then
        bytes=$(stat -c '%s' "$data")
        if ((bytes == expected)); then persisted=true; break; fi
        if ((bytes > expected)); then echo "File too large" >&2; exit 1; fi
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
    sleep 0.05
done
[[ "$persisted" == true ]] || { echo "Frames not fully written" >&2; exit 1; }

kill -TERM "$server_pid"
stopped=false
for ((i=0; i<400; ++i)); do
    state=$(ps -o stat= -p "$server_pid" 2>/dev/null || true)
    if [[ -z "$state" || "$state" == Z* ]]; then stopped=true; break; fi
    sleep 0.05
done
[[ "$stopped" == true ]] || { echo "Shutdown timeout" >&2; exit 1; }
if ! wait "$server_pid"; then
    server_pid=
    echo "Server exited with an error" >&2
    exit 1
fi
server_pid=
reader_pid=
end=$(date +%s%N)

[[ $(stat -c '%s' "$data") -eq "$expected" ]] || exit 1
grep -qF '[reader] EOF' "$work/server.log" || { echo "Reader EOF missing" >&2; exit 1; }
awk -v n="$frames" -v ns="$((end-start))" -v bytes="$expected" 'BEGIN {
    seconds=ns/1000000000;
    printf "persisted_frames=%d\npersisted_bytes=%d\n", n, bytes;
    printf "end_to_end_seconds=%.3f\nend_to_end_frames_per_second=%.0f\n", seconds, n/seconds;
}'
echo "validated: file size and graceful Reader EOF"

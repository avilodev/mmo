#!/usr/bin/env bash

set -u

RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}/mmo-server-${UID}"
SUPERVISOR_PID_FILE="$RUNTIME_DIR/supervisor.pid"

if [[ ! -f "$SUPERVISOR_PID_FILE" ]]; then
    echo "No supervised MMO server stack is running."
    exit 0
fi

supervisor_pid=$(<"$SUPERVISOR_PID_FILE")
if [[ ! "$supervisor_pid" =~ ^[0-9]+$ ]] ||
   [[ ! -r "/proc/$supervisor_pid/cmdline" ]] ||
   ! tr '\0' ' ' < "/proc/$supervisor_pid/cmdline" | grep -Fq "scripts/start_servers.sh"; then
    echo "Removing stale MMO supervisor state; no matching process is running."
    rm -f -- "$SUPERVISOR_PID_FILE"
    rmdir -- "$RUNTIME_DIR" 2>/dev/null || true
    exit 0
fi

echo "Requesting graceful shutdown from MMO supervisor PID $supervisor_pid..."
kill -TERM "$supervisor_pid"

for _ in {1..20}; do
    kill -0 "$supervisor_pid" 2>/dev/null || {
        echo "MMO server stack stopped."
        exit 0
    }
    sleep 1
done

echo "Supervisor did not exit within 20 seconds." >&2
exit 1

#!/usr/bin/env bash

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}/mmo-server-${UID}"
SUPERVISOR_PID_FILE="$RUNTIME_DIR/supervisor.pid"

declare -a SERVER_PIDS=()
declare -a SERVER_NAMES=()
CLEANING_UP=0

is_our_supervisor() {
    local pid=$1
    [[ -r "/proc/$pid/cmdline" ]] &&
        tr '\0' ' ' < "/proc/$pid/cmdline" | grep -Fq "scripts/start_servers.sh"
}

cleanup() {
    local exit_code=${1:-0}
    (( CLEANING_UP )) && return
    CLEANING_UP=1
    trap - INT TERM EXIT

    echo
    echo "Stopping MMO server stack..."

    local pid
    for pid in "${SERVER_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done

    # Give each server time to save players and join worker threads.
    local deadline=$((SECONDS + 15))
    while (( SECONDS < deadline )); do
        local alive=0
        for pid in "${SERVER_PIDS[@]}"; do
            kill -0 "$pid" 2>/dev/null && alive=1
        done
        (( alive == 0 )) && break
        sleep 1
    done

    for pid in "${SERVER_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "Force-stopping unresponsive server PID $pid"
            kill -KILL "$pid" 2>/dev/null || true
        fi
        wait "$pid" 2>/dev/null || true
    done

    rm -f -- "$SUPERVISOR_PID_FILE"
    rmdir -- "$RUNTIME_DIR" 2>/dev/null || true
    echo "All MMO servers stopped."
    exit "$exit_code"
}

fail() {
    echo "ERROR: $*" >&2
    cleanup 1
}

# Where each server writes its rolling log.
#
# Logging used to go to stdout and nowhere else, and this script did not
# redirect it, so a server's history lived only in whichever terminal happened
# to start it -- and a player's disconnect left nothing to diagnose. Each
# process now writes its own file; common/src/log.c rotates them.
LOG_DIR=${MMO_LOG_DIR:-"$PROJECT_ROOT/logs"}
mkdir -p -- "$LOG_DIR"

start_server() {
    local name=$1
    local log_slug=$2
    shift 2

    echo "Starting $name... (log: $LOG_DIR/$log_slug.log)"
    # The console stays on so this script's own supervision output still shows
    # startup failures; set MMO_LOG_CONSOLE=0 to send everything to the files.
    MMO_LOG_FILE="$LOG_DIR/$log_slug.log" "$@" &
    local pid=$!
    SERVER_NAMES+=("$name")
    SERVER_PIDS+=("$pid")

    # Catch immediate configuration, port, database, and asset failures.
    sleep 0.25
    kill -0 "$pid" 2>/dev/null || fail "$name failed during startup"
}

trap 'cleanup 130' INT
trap 'cleanup 143' TERM
trap 'cleanup $?' EXIT

mkdir -p -- "$RUNTIME_DIR"
if [[ -f "$SUPERVISOR_PID_FILE" ]]; then
    old_pid=$(<"$SUPERVISOR_PID_FILE")
    if [[ "$old_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_pid" 2>/dev/null && is_our_supervisor "$old_pid"; then
        echo "MMO server stack is already supervised by PID $old_pid" >&2
        exit 1
    fi
    rm -f -- "$SUPERVISOR_PID_FILE"
fi
printf '%s\n' "$$" > "$SUPERVISOR_PID_FILE"

cd -- "$PROJECT_ROOT"

required_files=(
    login_server/bin/login_server
    login_server/certs/server.crt
    login_server/certs/server.key
    realm_server/bin/realm_server
    realm_server/realm_config/realm_1.conf
    realm_server/bin/data/worlds.conf
    world_server/bin/world_server
    world_server/bin/data/world.dat
    world_server/bin/data/zones.json
)
for required in "${required_files[@]}"; do
    [[ -f "$required" ]] || fail "Missing $required
       Run 'make setup' to create what setup owns, then 'make'.
       'make setup-check' lists everything that is missing in one pass."
done

# Redis credentials, if the deployment has any.
#
# REDISCLI_AUTH is what redis-cli reads on its own; passing -a puts the
# password on a command line where /proc and the shell history can see it. The
# servers read MMO_REDIS_PASSWORD themselves and are launched with it inherited
# from this shell, the same as every other setting here.
if [[ -n "${MMO_REDIS_PASSWORD:-}" ]]; then
    export REDISCLI_AUTH="$MMO_REDIS_PASSWORD"
fi

redis-cli PING 2>/dev/null | grep -qx PONG || fail "Redis is not reachable
       If Redis has a password, set MMO_REDIS_PASSWORD before running this."
pg_isready -q || fail "PostgreSQL is not ready"

# Try to connect with what libpq can already find before asking for anything.
#
# setup/setup.sh writes a ~/.pgpass entry, and libpq reads it without being
# told, so on a machine that ran 'make setup' this whole block is silent. The
# prompt is the fallback for a machine that did not, and it is deliberately not
# the first thing tried: exporting PGPASSWORD puts the password in the
# environment of all twelve child processes, where /proc exposes it to anything
# else running as this user.
if [[ -z "${PGPASSWORD:-}" ]] &&
   ! psql -w -h localhost -U postgres -d postgres -Atqc "SELECT 1" 2>/dev/null | grep -qx 1; then
    echo "PostgreSQL needs a password and none was found in ~/.pgpass."
    echo "Run 'make setup' to configure one, or enter it now."
    read -r -s -p "PostgreSQL password for user postgres: " PGPASSWORD
    echo
    export PGPASSWORD
fi

psql -w -h localhost -U postgres -d postgres -Atqc "SELECT 1" 2>/dev/null | grep -qx 1 ||
    fail "PostgreSQL authentication failed for user postgres.
       Run 'make setup' to configure ~/.pgpass, or 'make setup-check' to see what is wrong."

if [[ $(redis-cli EXISTS server_auth_key:global 2>/dev/null) != 1 ]]; then
    echo "Redis server keys are missing; generating them now..."
    bash common/server_keys/generate_daily_server_keys.sh || fail "Could not generate Redis server keys"
fi

start_server "login server" login ./login_server/bin/login_server

worlds=(Armeia Bosteuis Cardinal Derive Exodus Jatrus Karmel Longevity Nervow Prototype)
for world in "${worlds[@]}"; do
    config="world_server/world_config/${world}.conf"
    [[ -f "$config" ]] || fail "Missing $config"
    start_server "$world world server" "world-${world,,}" ./world_server/bin/world_server "$config"
done

# Start realm last so its first monitoring pass can reach every world.
start_server "realm server" realm ./realm_server/bin/realm_server realm_server/realm_config/realm_1.conf

echo
echo "MMO stack running (${#SERVER_PIDS[@]} processes). Press Ctrl+C to stop everything."
echo "From another terminal, 'make stop' requests the same graceful shutdown."

while :; do
    for i in "${!SERVER_PIDS[@]}"; do
        pid=${SERVER_PIDS[$i]}
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null
            status=$?
            echo "${SERVER_NAMES[$i]} exited unexpectedly (status $status)" >&2
            cleanup 1
        fi
    done
    sleep 1
done

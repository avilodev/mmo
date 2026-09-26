#!/usr/bin/env bash

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}/mmo-server-${UID}"
SUPERVISOR_PID_FILE="$RUNTIME_DIR/supervisor.pid"

declare -a SERVER_PIDS=()
declare -a SERVER_NAMES=()
declare -a SERVER_CMDS=()       # shell-quoted argv, so a dead service can be restarted
declare -a SERVER_SLUGS=()      # log basename, needed again on every restart
declare -a SERVER_FAILS=()      # restarts inside the current window
declare -a SERVER_WINDOW=()     # SECONDS at which that window opened
CLEANING_UP=0

# Restart policy.
#
# A world server crashing used to take the whole stack with it: this script
# noticed the dead child and ran cleanup, which TERMs and then KILLs all twelve
# processes. That threw away the partial-failure tolerance everything above it
# was built for -- the realm marks an unreachable world offline and steers
# players to another one, world sessions in Redis carry a TTL precisely so a
# world that crashed does not block anything, and capacity refusals are already
# graceful. One process dying should cost that process's players, not everyone's.
#
# So a dead child is restarted where it stood. A service that will not stay up
# is abandoned rather than restarted forever -- a crash loop is usually a bad
# config or a corrupt data file, and hammering it buries the first, most useful
# error under thousands of identical ones. The rest of the stack keeps running,
# and the supervisor exits only when nothing is left alive.
MAX_RESTARTS=${MMO_SUPERVISOR_MAX_RESTARTS:-3}
RESTART_WINDOW=${MMO_SUPERVISOR_RESTART_WINDOW:-300}
# Set to 0 for the old fail-fast behaviour: any exit stops the stack. Useful in
# CI and when bisecting a startup crash, where the first failure is the answer.
RESTART_ENABLED=${MMO_SUPERVISOR_RESTART:-1}

is_our_supervisor() {
    local pid=$1
    [[ -r "/proc/$pid/cmdline" ]] &&
        tr '\0' ' ' < "/proc/$pid/cmdline" | grep -Fq "scripts/start_servers.sh"
}

cleanup() {
    local exit_code=${1:-0}
    (( CLEANING_UP )) && return
    CLEANING_UP=1

    # Ignore further interrupts rather than restoring the default disposition.
    #
    # This was `trap - INT TERM EXIT`, which reset INT and TERM to "terminate".
    # Cleanup then spent up to 15 seconds waiting for servers to save and exit,
    # and a second Ctrl+C anywhere in that window -- the natural reaction to a
    # shell that looks hung -- killed the supervisor outright, before the
    # force-kill loop below ever ran. Anything still shutting down was orphaned
    # with its listening socket still bound, so the next `make run` died on
    # "Failed to create server socket" and the ports could only be freed by
    # hand. A server that hangs in its own shutdown made that the normal case
    # rather than the rare one.
    #
    # Ignoring the signals means the force-kill always runs: cleanup is bounded
    # at ~15 seconds and cannot be interrupted into leaving a process behind.
    # EXIT is still cleared, because cleanup ends in `exit` and would otherwise
    # re-enter itself.
    trap '' INT TERM
    trap - EXIT

    echo
    echo "Stopping MMO server stack..."

    # A PID of 0 marks a service that was abandoned after a crash loop. It must
    # be skipped rather than signalled: kill(0) means "every process in my own
    # group", which here is this supervisor and every server still running.
    local pid
    for pid in "${SERVER_PIDS[@]}"; do
        (( pid == 0 )) && continue
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done

    # Give each server time to save players and join worker threads.
    local deadline=$((SECONDS + 15))
    while (( SECONDS < deadline )); do
        local alive=0
        for pid in "${SERVER_PIDS[@]}"; do
            (( pid == 0 )) && continue
            kill -0 "$pid" 2>/dev/null && alive=1
        done
        (( alive == 0 )) && break
        sleep 1
    done

    for pid in "${SERVER_PIDS[@]}"; do
        (( pid == 0 )) && continue
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

# Launch one service in the background, reporting its PID in REPLY_PID.
#
# Split out of start_server() because a restart needs the launch without the
# bookkeeping and without the fail-fast check.
REPLY_PID=0
spawn() {
    local log_slug=$1
    shift
    # The console stays on so this script's own supervision output still shows
    # startup failures; set MMO_LOG_CONSOLE=0 to send everything to the files.
    MMO_LOG_FILE="$LOG_DIR/$log_slug.log" "$@" &
    REPLY_PID=$!
}

start_server() {
    local name=$1
    local log_slug=$2
    shift 2

    echo "Starting $name... (log: $LOG_DIR/$log_slug.log)"
    spawn "$log_slug" "$@"
    local pid=$REPLY_PID

    # Remembered so this service can be restarted where it stood. %q survives
    # the round trip through eval for any argument, including the ones this
    # script does not happen to pass today.
    local quoted
    printf -v quoted '%q ' "$@"

    SERVER_NAMES+=("$name")
    SERVER_PIDS+=("$pid")
    SERVER_SLUGS+=("$log_slug")
    SERVER_CMDS+=("$quoted")
    SERVER_FAILS+=(0)
    SERVER_WINDOW+=("$SECONDS")

    # Catch immediate configuration, port, database, and asset failures.
    #
    # Still fail-fast, and deliberately: a service that cannot survive its first
    # quarter second has a bad config, a taken port or a missing file, and none
    # of those get better by being retried. The restart policy below is for a
    # process that ran and then died.
    sleep 0.25
    kill -0 "$pid" 2>/dev/null || fail "$name failed during startup"
}

# Bring one service back after an unexpected exit.
#
# @return 0 when it was restarted, 1 when it has been abandoned.
restart_server() {
    local i=$1
    local name=${SERVER_NAMES[$i]}

    # Reopen the counting window when the last failure is far enough behind.
    # Without this a service that crashes once a week is eventually abandoned
    # for its third crash in a month, which is not a crash loop.
    if (( SECONDS - SERVER_WINDOW[i] > RESTART_WINDOW )); then
        SERVER_FAILS[$i]=0
        SERVER_WINDOW[$i]=$SECONDS
    fi

    SERVER_FAILS[$i]=$(( SERVER_FAILS[i] + 1 ))

    if (( SERVER_FAILS[i] > MAX_RESTARTS )); then
        SERVER_PIDS[$i]=0
        echo "$name has died ${SERVER_FAILS[i]} times within ${RESTART_WINDOW}s — giving up on it." >&2
        echo "   The rest of the stack keeps running. The first failure is the one to read:" >&2
        echo "   $LOG_DIR/${SERVER_SLUGS[$i]}.log" >&2
        return 1
    fi

    echo "Restarting $name (${SERVER_FAILS[i]} of $MAX_RESTARTS within ${RESTART_WINDOW}s)..." >&2
    eval "spawn \"\${SERVER_SLUGS[$i]}\" ${SERVER_CMDS[$i]}"
    SERVER_PIDS[$i]=$REPLY_PID
    return 0
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
    any_alive=0

    for i in "${!SERVER_PIDS[@]}"; do
        pid=${SERVER_PIDS[$i]}
        (( pid == 0 )) && continue        # abandoned after a crash loop

        if kill -0 "$pid" 2>/dev/null; then
            any_alive=1
            continue
        fi

        wait "$pid" 2>/dev/null
        status=$?
        echo "${SERVER_NAMES[$i]} exited unexpectedly (status $status)" >&2

        (( RESTART_ENABLED )) || cleanup 1

        restart_server "$i" && any_alive=1
    done

    # Only when there is nothing left to supervise. One world giving up costs
    # that world; all twelve giving up means the machine has a problem this
    # script cannot fix by waiting.
    if (( ! any_alive )); then
        echo "Every service has been abandoned — nothing left to supervise." >&2
        cleanup 1
    fi

    sleep 1
done

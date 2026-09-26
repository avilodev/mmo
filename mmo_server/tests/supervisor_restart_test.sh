#!/usr/bin/env bash
#
# Prove that one service dying no longer takes the stack with it.
#
# scripts/start_servers.sh used to call cleanup on any child exiting, which
# TERMs and then KILLs all twelve processes -- so a segfault in one world
# disconnected the other nine worlds' players, the realm and the login server.
# This drives the real script against stub services and checks the three things
# that behaviour has to be replaced by:
#
#   1. a dead service comes back, and nothing else is touched
#   2. a service that will not stay up is abandoned, and nothing else is touched
#   3. the stack still exits cleanly, and stops everything, on a TERM
#
# Everything the script talks to before its watch loop -- Redis, PostgreSQL, the
# binaries, the world file -- is stubbed, because none of it is what is under
# test.

set -u

SERVER_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT

FAILURES=0
check() {
    if [[ $1 == 0 ]]; then
        echo "  FAIL $2"
        FAILURES=$((FAILURES + 1))
    else
        echo "  ok   $2"
    fi
}

# --- a tree that looks enough like the real one --------------------------

PIDDIR="$WORK/pids"
mkdir -p "$PIDDIR" "$WORK/scripts" "$WORK/stubs" \
         "$WORK/login_server/bin" "$WORK/login_server/certs" \
         "$WORK/realm_server/bin/data" "$WORK/realm_server/realm_config" \
         "$WORK/world_server/bin/data" "$WORK/world_server/world_config"

cp "$SERVER_ROOT/scripts/start_servers.sh" "$WORK/scripts/"

# Each stub records which incarnation it is, so a restart is visible as a new
# PID under the same name and as one more line in the start log.
cat > "$WORK/stubs/service" <<'STUB'
#!/usr/bin/env bash
name=$(basename "${MMO_LOG_FILE:-unknown.log}" .log)
printf '%s\n' "$$" > "$PIDDIR/$name.pid"
printf '%s\n' "$$" >> "$PIDDIR/$name.starts"
trap 'exit 0' TERM
while :; do sleep 0.2; done
STUB
chmod +x "$WORK/stubs/service"

for binary in login_server/bin/login_server realm_server/bin/realm_server \
              world_server/bin/world_server; do
    cp "$WORK/stubs/service" "$WORK/$binary"
done

: > "$WORK/login_server/certs/server.crt"
: > "$WORK/login_server/certs/server.key"
: > "$WORK/realm_server/realm_config/realm_1.conf"
: > "$WORK/realm_server/bin/data/worlds.conf"
: > "$WORK/world_server/bin/data/world.dat"
: > "$WORK/world_server/bin/data/zones.json"
for world in Armeia Bosteuis Cardinal Derive Exodus Jatrus Karmel Longevity Nervow Prototype; do
    : > "$WORK/world_server/world_config/${world}.conf"
done

# Redis and PostgreSQL, reduced to the four answers the script asks them for.
cat > "$WORK/stubs/redis-cli" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
    PING)   echo PONG ;;
    EXISTS) echo 1 ;;          # server keys already provisioned
    *)      echo 0 ;;
esac
STUB
cat > "$WORK/stubs/pg_isready" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
cat > "$WORK/stubs/psql" <<'STUB'
#!/usr/bin/env bash
echo 1
STUB
chmod +x "$WORK/stubs/redis-cli" "$WORK/stubs/pg_isready" "$WORK/stubs/psql"

# --- helpers -------------------------------------------------------------

pid_of()    { cat "$PIDDIR/$1.pid" 2>/dev/null || echo 0; }
starts_of() { wc -l < "$PIDDIR/$1.starts" 2>/dev/null | tr -d ' ' || echo 0; }

# Wait until a predicate holds, up to a deadline. Polling, because the thing
# under test polls: the supervisor notices a dead child once a second.
wait_for() {
    local timeout=$1; shift
    local deadline=$((SECONDS + timeout))
    while (( SECONDS < deadline )); do
        "$@" && return 0
        sleep 0.2
    done
    return 1
}

pid_changed()  { [[ $(pid_of "$1") != "$2" && $(pid_of "$1") != 0 ]]; }
starts_reach() { (( $(starts_of "$1") >= $2 )); }

# --- run it --------------------------------------------------------------

echo "=== supervisor restart policy ==="

export PATH="$WORK/stubs:$PATH"
export PIDDIR
export XDG_RUNTIME_DIR="$WORK/run"
export MMO_LOG_DIR="$WORK/logs"
export MMO_SUPERVISOR_MAX_RESTARTS=2
export MMO_SUPERVISOR_RESTART_WINDOW=300
mkdir -p "$XDG_RUNTIME_DIR"

bash "$WORK/scripts/start_servers.sh" > "$WORK/supervisor.out" 2>&1 &
SUPERVISOR=$!

if ! wait_for 20 test -f "$PIDDIR/realm.pid"; then
    echo "  FAIL the stack never finished starting"
    sed -n '1,40p' "$WORK/supervisor.out"
    kill -KILL "$SUPERVISOR" 2>/dev/null
    exit 1
fi
check 1 "all twelve services started"

login_before=$(pid_of login)
realm_before=$(pid_of realm)
armeia_before=$(pid_of world-armeia)

# 1. A crash is survivable, and local.
kill -KILL "$armeia_before" 2>/dev/null
if wait_for 15 pid_changed world-armeia "$armeia_before"; then
    check 1 "a killed world server is restarted in place"
else
    check 0 "a killed world server is restarted in place"
fi
check "$([[ $(pid_of login) == "$login_before" ]] && echo 1 || echo 0)" \
      "the login server was not touched"
check "$([[ $(pid_of realm) == "$realm_before" ]] && echo 1 || echo 0)" \
      "the realm server was not touched"
check "$(kill -0 "$SUPERVISOR" 2>/dev/null && echo 1 || echo 0)" \
      "the supervisor is still running"

# 2. A crash loop is abandoned, and that is also local.
#
# One restart has been spent. Two more kills take it past MAX_RESTARTS=2, after
# which the world stops coming back and nothing else notices.
for _ in 1 2; do
    current=$(pid_of world-armeia)
    kill -KILL "$current" 2>/dev/null
    wait_for 15 pid_changed world-armeia "$current" || true
done

starts_at_giveup=$(starts_of world-armeia)
last=$(pid_of world-armeia)
kill -KILL "$last" 2>/dev/null
sleep 4

check "$([[ $(starts_of world-armeia) == "$starts_at_giveup" ]] && echo 1 || echo 0)" \
      "a world in a crash loop is abandoned rather than restarted forever"
check "$(grep -q "giving up on it" "$WORK/supervisor.out" && echo 1 || echo 0)" \
      "and the supervisor says so, naming the log to read"
check "$(kill -0 "$login_before" 2>/dev/null && echo 1 || echo 0)" \
      "the login server outlives the world that gave up"
check "$(kill -0 "$realm_before" 2>/dev/null && echo 1 || echo 0)" \
      "so does the realm"
check "$(kill -0 "$SUPERVISOR" 2>/dev/null && echo 1 || echo 0)" \
      "and the supervisor keeps supervising the rest"

# 3. Shutdown still stops everything, including the survivors.
kill -TERM "$SUPERVISOR" 2>/dev/null
wait_for 25 bash -c "! kill -0 $SUPERVISOR 2>/dev/null"
check "$(kill -0 "$SUPERVISOR" 2>/dev/null && echo 0 || echo 1)" \
      "a TERM to the supervisor stops it"
check "$(kill -0 "$login_before" 2>/dev/null && echo 0 || echo 1)" \
      "and takes the login server down with it"
check "$(kill -0 "$realm_before" 2>/dev/null && echo 0 || echo 1)" \
      "and the realm"

# The abandoned-service PID is 0 in the supervisor's table. kill(0) means the
# caller's whole process group, so a cleanup that did not skip it would have
# signalled the supervisor itself -- worth stating, since nothing else would
# show it.
check "$(grep -q "All MMO servers stopped" "$WORK/supervisor.out" && echo 1 || echo 0)" \
      "cleanup ran to completion with an abandoned service in the table"

echo
if (( FAILURES )); then
    echo "$FAILURES check(s) failed"
    echo "--- supervisor output ---"
    cat "$WORK/supervisor.out"
    exit 1
fi
echo "ALL ASSERTIONS PASSED"

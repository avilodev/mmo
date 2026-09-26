#!/bin/bash
#
# Verify this machine can run the stack. Changes nothing.
#
# Run it with:  make setup-check
#
# Every check names the failure it prevents, because the point of a preflight is
# to move a problem from "the server exited and I don't know why" to "this one
# thing is missing".

set -uo pipefail

SETUP_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SETUP_DIR/.." && pwd)"
export PROJECT_ROOT

source "$SETUP_DIR/lib/common.sh"
source "$SETUP_DIR/lib/worlds.sh"
source "$SETUP_DIR/lib/packages.sh"
source "$SETUP_DIR/lib/certs.sh"
source "$SETUP_DIR/lib/layout.sh"

FAILURES=0
fail_check() { FAILURES=$((FAILURES + 1)); printf '  %s✗%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }

check_tooling() {
    step "Tooling"
    local tool
    for tool in gcc make psql redis-cli openssl; do
        if have "$tool"; then same "$tool"; else fail_check "$tool is not installed — run 'make setup'"; fi
    done
    if dependencies_present; then ok "all build headers present"
    else fail_check "some development headers are missing — run 'make setup'"; fi
}

check_services() {
    step "Services"
    if pg_isready -q 2>/dev/null; then same "PostgreSQL is accepting connections"
    else fail_check "PostgreSQL is not running — 'sudo service postgresql start'"; fi

    if [ -n "${MMO_REDIS_PASSWORD:-}" ]; then export REDISCLI_AUTH="$MMO_REDIS_PASSWORD"; fi

    if redis-cli PING 2>/dev/null | grep -qx PONG; then same "Redis is answering"
    else fail_check "Redis is not running — 'sudo service redis-server start'"; fi
}

PG_AUTH_OK=0

check_postgres_auth() {
    step "PostgreSQL authentication"
    if PGCONNECT_TIMEOUT=5 psql -w -h localhost -U postgres -d postgres -Atqc 'SELECT 1' 2>/dev/null | grep -qx 1; then
        ok "the postgres role authenticates over TCP without a prompt"
        PG_AUTH_OK=1
    else
        fail_check "cannot authenticate as postgres over TCP — 'make setup' writes ~/.pgpass"
        note "    without this, start_servers.sh prompts and the servers cannot reach their databases"
    fi
}

check_databases() {
    step "World databases"
    pg_isready -q 2>/dev/null || { note "skipped, PostgreSQL is down"; return; }

    # Without working authentication every lookup fails, and reporting ten
    # missing databases would bury the one problem that actually caused it.
    if [ "$PG_AUTH_OK" -ne 1 ]; then
        note "skipped, cannot authenticate — fix the item above first"
        return
    fi

    local name region database host port max_players hardcore realm_port missing=0
    while IFS=$'\t' read -r name region database host port max_players hardcore realm_port; do
        # </dev/null matters: this runs inside a while-read loop, and psql
        # inherits and drains the loop's stdin otherwise, so only the first
        # world is ever checked.
        if PGCONNECT_TIMEOUT=5 psql -w -h localhost -U postgres -d postgres -Atqc \
             "SELECT 1 FROM pg_database WHERE datname='$database'" </dev/null 2>/dev/null | grep -qx 1; then
            same "$database"
        else
            fail_check "$database is missing — run 'make setup'"
            missing=$((missing + 1))
        fi
    done < <(world_rows)
    [ "$missing" -eq 0 ] && ok "every world has a database"
}

check_generated_files() {
    step "Generated configuration"

    local name region database host port max_players hardcore realm_port
    while IFS=$'\t' read -r name region database host port max_players hardcore realm_port; do
        local path="$PROJECT_ROOT/world_server/world_config/$name.conf"
        if [ ! -f "$path" ]; then
            fail_check "$name.conf is missing — run 'make setup'"
            continue
        fi
        local expected
        expected=$(printf '%s\n%s\n%s:%s\n%s\n%s' "$name" "$region" "$host" "$port" "$max_players" "$hardcore")
        if [ "$(read_world_config_block "$path")" = "$expected" ]; then
            same "$name.conf"
        else
            fail_check "$name.conf disagrees with setup/worlds.conf"
        fi
    done < <(world_rows)

    # The realm no longer keeps a world list of its own. It reads worlds.conf,
    # the same file this loop just checked every world .conf against, from
    # beside its binary.
    local deployed="$PROJECT_ROOT/realm_server/bin/data/worlds.conf"
    if [ ! -f "$deployed" ]; then
        note "    realm_server/bin/data/worlds.conf not built yet — 'make' packages it"
    elif cmp -s "$PROJECT_ROOT/setup/worlds.conf" "$deployed"; then
        same "realm_server/bin/data/worlds.conf matches setup/worlds.conf"
    else
        fail_check "realm_server/bin/data/worlds.conf is stale — run 'make'"
    fi
}

check_runtime_files() {
    step "Runtime files"

    # Three services terminate TLS now, not one.
    local entry service cn crt key
    for entry in $CERT_SERVICES; do
        service="${entry%%:*}"
        crt="$PROJECT_ROOT/$service/certs/server.crt"
        key="$PROJECT_ROOT/$service/certs/server.key"

        if [ ! -f "$crt" ] || [ ! -f "$key" ]; then
            fail_check "$service/certs/server.crt is missing — run 'make setup'"
        elif openssl x509 -in "$crt" -noout -checkend 0 >/dev/null 2>&1; then
            same "$service certificate valid until $(openssl x509 -in "$crt" -noout -enddate | cut -d= -f2)"
        else
            fail_check "the $service certificate has expired — 'make setup' regenerates it"
        fi
    done

    if [ -f "$WORLD_DAT" ]; then
        same "world.dat present ($(du -h "$WORLD_DAT" | cut -f1))"
    else
        fail_check "world_server/data/world.dat is missing — worlds refuse to start without it"
    fi

    [ -d "$PROJECT_ROOT/database/databases/users_data" ] \
        && same "SQLite account directory present" \
        || fail_check "database/databases/users_data is missing"
}

# One pin file, and whether it names the certificate it is supposed to.
#
# $1 description, $2 certificate, $3 pin file, $4 what refuses when it is wrong
check_one_pin() {
    local what="$1" crt="$2" pin_file="$3" consequence="$4"
    local pin

    if [ ! -f "$crt" ]; then
        note "$what: skipped, no certificate at $crt"
        return
    fi

    pin=$(compute_pin_line "$crt") || { fail_check "cannot fingerprint $crt"; return; }

    if [ -f "$pin_file" ] && grep -qxF "$pin" "$pin_file"; then
        ok "$what"
    else
        fail_check "$what — $consequence"
        note "    run 'make setup', or add this line to $pin_file:"
        note "      $pin"
    fi
}

check_pins() {
    step "Public-key pins"

    check_one_pin "the realm pins the world's key" \
        "$PROJECT_ROOT/world_server/certs/server.crt" \
        "$PROJECT_ROOT/realm_server/certs/world_pins.txt" \
        "the realm will connect to no world"

    check_one_pin "the world pins the realm's key" \
        "$PROJECT_ROOT/realm_server/certs/server.crt" \
        "$PROJECT_ROOT/world_server/certs/realm_pins.txt" \
        "every world will refuse the realm"

    if [ ! -d "$CLIENT_ROOT" ]; then
        note "client tree not at $CLIENT_ROOT; it must pin:"
        note "  login  $(compute_pin_line "$PROJECT_ROOT/login_server/certs/server.crt" 2>/dev/null || echo '?')"
        note "  realm  $(compute_pin_line "$PROJECT_ROOT/realm_server/certs/server.crt" 2>/dev/null || echo '?')"
        return
    fi

    check_one_pin "the launcher pins the login server's key" \
        "$PROJECT_ROOT/login_server/certs/server.crt" \
        "$CLIENT_ROOT/Launcher/certs/login_pins.txt" \
        "the launcher will refuse to log in"

    check_one_pin "the game pins the realm's key" \
        "$PROJECT_ROOT/realm_server/certs/server.crt" \
        "$CLIENT_ROOT/Game/certs/realm_pins.txt" \
        "the game will refuse to reach the realm"
}

main() {
    printf '%sPreflight for the Multiverse MMO server%s\n' "$C_BOLD" "$C_RESET"
    printf '  root: %s\n' "$PROJECT_ROOT"

    validate_world_table
    check_tooling
    check_services
    check_postgres_auth
    check_databases
    check_generated_files
    check_worlds_conf_deployed || FAILURES=$((FAILURES + 1))
    check_runtime_files
    check_pins
    check_world_dat_matches_client

    printf '\n%s%s%s\n' "$C_BOLD" "────────────────────────────────────────────────────────" "$C_RESET"
    if [ "$FAILURES" -eq 0 ] && [ "$SETUP_WARNINGS" -eq 0 ]; then
        printf '%sReady.%s  make run\n\n' "$C_GREEN$C_BOLD" "$C_RESET"
        return 0
    fi
    printf '%s%s problem(s), %s warning(s).%s Run '"'"'make setup'"'"' to fix what it can.\n\n' \
        "$C_YELLOW$C_BOLD" "$FAILURES" "$SETUP_WARNINGS" "$C_RESET"
    return 1
}

main "$@"

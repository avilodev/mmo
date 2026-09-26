#!/bin/bash
#
# One-command setup for a Linux host: install dependencies, bring PostgreSQL and
# Redis up, create the world databases, generate the world configuration, every
# TLS certificate and the pin files that name them, and produce world.dat.
#
# Run it with:  make setup
#
# Every step is idempotent. Re-running reports what was already correct rather
# than redoing it, so it is also the way to repair a half-configured machine.
#
# Environment:
#   MMO_CLIENT_ROOT   path to the client tree (default: ../mmo_client)
#   FORCE_CONFIGS=1   overwrite world .conf files that disagree with worlds.conf
#   SKIP_PACKAGES=1   assume dependencies are installed; do not touch the package manager
#   SKIP_WORLD_DAT=1  do not generate world.dat (it takes minutes and ~115 MB)

set -uo pipefail

SETUP_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SETUP_DIR/.." && pwd)"
export PROJECT_ROOT

# shellcheck source=lib/common.sh
source "$SETUP_DIR/lib/common.sh"
source "$SETUP_DIR/lib/worlds.sh"
source "$SETUP_DIR/lib/packages.sh"
source "$SETUP_DIR/lib/services.sh"
source "$SETUP_DIR/lib/postgres.sh"
source "$SETUP_DIR/lib/certs.sh"
source "$SETUP_DIR/lib/layout.sh"

SETUP_FORCE_CONFIGS="${FORCE_CONFIGS:-0}"

banner() {
    printf '%s\n' "$C_BOLD"
    printf 'Multiverse MMO server setup\n'
    printf '%s' "$C_RESET"
    printf '  root:   %s\n' "$PROJECT_ROOT"
    printf '  client: %s%s\n' "$CLIENT_ROOT" "$([ -d "$CLIENT_ROOT" ] || printf ' (not found)')"
    printf '  worlds: %s from setup/worlds.conf\n' "$(world_count)"
}

main() {
    [ "$(uname -s)" = Linux ] || die "This setup script targets Linux. On Windows, run it inside WSL."

    banner

    # Ask for the password once, before any step runs, so no later step stops
    # to prompt. Failure here is not fatal: the steps that need root each
    # report their own blocker, and everything that does not need it still runs.
    prime_sudo || true

    # Order matters. Each step depends on the one before it: packages provide
    # psql and redis-cli, the services must answer before databases can be
    # created, and the world table has to be valid before anything is generated
    # from it.
    validate_world_table

    if [ "${SKIP_PACKAGES:-0}" = 1 ]; then
        step "Dependencies"
        same "skipped (SKIP_PACKAGES=1)"
    else
        install_packages
    fi

    # Each of these reports its own blockers and returns non-zero rather than
    # exiting, so one missing prerequisite does not hide the next four. The
    # summary collects them.
    start_services      || true
    configure_postgres_auth || true
    create_world_databases  || true
    create_app_role         || true

    create_runtime_directories
    generate_world_configs
    generate_key_rotation_script
    check_worlds_conf_deployed || true

    generate_certificates
    install_server_pins
    install_client_pins

    if [ "${SKIP_WORLD_DAT:-0}" = 1 ]; then
        step "World map (world.dat)"
        same "skipped (SKIP_WORLD_DAT=1)"
    else
        generate_world_dat || true
    fi
    check_world_dat_matches_client

    summary
}

summary() {
    printf '\n%s%s%s\n' "$C_BOLD" "────────────────────────────────────────────────────────" "$C_RESET"

    if [ "$SETUP_BLOCKERS" -eq 0 ] && [ "$SETUP_WARNINGS" -eq 0 ]; then
        printf '%sSetup complete.%s %s change(s) made.\n\n' "$C_GREEN$C_BOLD" "$C_RESET" "$SETUP_CHANGES"
        printf 'Next:\n'
        printf '  make              build the three services\n'
        printf '  make run          start the whole stack (Ctrl+C stops it)\n'
        printf '  make setup-check  re-verify this machine at any time\n\n'
        return 0
    fi

    if [ "$SETUP_BLOCKERS" -gt 0 ]; then
        printf '%s%s blocker(s)%s and %s warning(s). %s change(s) made.\n\n' \
            "$C_RED$C_BOLD" "$SETUP_BLOCKERS" "$C_RESET" "$SETUP_WARNINGS" "$SETUP_CHANGES"
        printf 'The stack will not start until the ✗ items above are resolved. Everything\n'
        printf 'else was set up, so fixing them and re-running is enough — setup is\n'
        printf 'idempotent and will leave the finished work alone.\n\n'
        printf '  make setup-check  re-check without changing anything\n\n'
        return 1
    fi

    printf '%sSetup finished with %s warning(s).%s %s change(s) made.\n\n' \
        "$C_YELLOW$C_BOLD" "$SETUP_WARNINGS" "$C_RESET" "$SETUP_CHANGES"
    printf 'The warnings above are things "make run" may trip over. Read them before\n'
    printf 'starting the stack; each one names what to do.\n\n'
    return 1
}

main "$@"

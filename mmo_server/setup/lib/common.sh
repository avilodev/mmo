# Shared helpers for the setup steps. Sourced, never executed.
#
# Every step is expected to be idempotent and to say which of the three things
# it did: created something, found it already correct, or skipped it for a
# stated reason. A setup script that prints nothing but "ok" teaches the
# operator nothing about the machine it just changed.

set -uo pipefail

# ANSI only when stdout is a terminal, so piping to a log stays readable.
if [ -t 1 ]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
    C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_BLUE=$'\033[36m'
else
    C_RESET=''; C_BOLD=''; C_DIM=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_BLUE=''
fi

SETUP_WARNINGS=0
SETUP_CHANGES=0
SETUP_BLOCKERS=0

step()    { printf '\n%s==> %s%s\n' "$C_BOLD$C_BLUE" "$*" "$C_RESET"; }
ok()      { printf '  %s✓%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
made()    { SETUP_CHANGES=$((SETUP_CHANGES + 1)); printf '  %s+%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
same()    { printf '  %s·%s %s\n' "$C_DIM" "$C_RESET" "$*"; }
note()    { printf '  %s%s\n' "$C_DIM" "$*$C_RESET"; }
warn()    { SETUP_WARNINGS=$((SETUP_WARNINGS + 1)); printf '  %s!%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }

# A problem that stops "make run" from working, but not one that stops setup
# from finishing everything else. Reported once, and again in the summary --
# one pass that surfaces every blocker beats failing on the first and making
# the operator re-run to discover the second.
blocker() { SETUP_BLOCKERS=$((SETUP_BLOCKERS + 1)); printf '  %s✗%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }

# Reserved for a state setup cannot reason past at all: a malformed world table,
# or the wrong operating system. Anything recoverable should be a blocker.
die()     { printf '\n%sFAILED:%s %s\n' "$C_RED$C_BOLD" "$C_RESET" "$*" >&2; exit 1; }

# Report whether a command exists.
have() { command -v "$1" >/dev/null 2>&1; }

# Run a command with root privileges, explaining why the first time sudo is used.
SUDO_EXPLAINED=0
as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
        return
    fi
    have sudo || return 127
    if [ "$SUDO_EXPLAINED" -eq 0 ]; then
        note "sudo is required to install packages, manage services, and administer PostgreSQL."
        SUDO_EXPLAINED=1
    fi
    sudo "$@"
}

# Ask for the sudo password once, at the start, instead of at whatever moment
# the first privileged step happens to run.
#
# Two reasons. A prompt that appears twelve lines into the output looks like the
# script has hung, especially when output is being piped somewhere. And with the
# credential cached up front, no later step can block: they either have root or
# they report a blocker and move on.
prime_sudo() {
    [ "$(id -u)" -eq 0 ] && return 0
    have sudo || return 1
    sudo -n true 2>/dev/null && return 0

    if [ ! -t 0 ] || [ ! -t 1 ]; then
        return 1   # no terminal to type into; callers report this as a blocker
    fi

    printf '\n%sSetup needs root%s for three things: installing packages, starting\n' "$C_BOLD" "$C_RESET"
    printf 'PostgreSQL and Redis, and creating the world databases. Everything else\n'
    printf 'runs as you. Enter your password once now, or Ctrl-C to stop.\n\n'

    sudo -v || return 1
    SUDO_EXPLAINED=1
    return 0
}

# Report whether this shell can actually obtain root right now.
#
# Checked up front rather than inferred from a failed psql, because "could not
# set the postgres password" and "sudo would not run" send an operator looking
# in completely different places.
# After prime_sudo() has run, this is answered from the cached credential alone,
# so it never blocks and never prompts.
can_be_root() {
    [ "$(id -u)" -eq 0 ] && return 0
    have sudo || return 1
    sudo -n true 2>/dev/null
}

# Explain the absence of root once, in terms of what it blocks.
report_missing_root() {
    blocker "$1 needs root, and this shell cannot obtain it"
    if ! have sudo; then
        note "    sudo is not installed. Re-run as root: sudo -i, then make setup"
    elif [ ! -t 0 ] || [ ! -t 1 ]; then
        note "    no terminal to enter a sudo password on. Run 'make setup' interactively,"
        note "    or pre-authorise first:  sudo -v && make setup"
    else
        note "    the sudo password was not accepted. Re-run, or: sudo -i, then make setup"
    fi
}

# Run psql as the PostgreSQL superuser over the local socket (peer auth).
#
# -w is not optional. Without it, psql prompts for a password on a terminal when
# authentication fails, and a setup script that stops at an invisible prompt
# reads as a hang. Every psql call in setup passes it; a refused connection must
# come back as a failed command, not as a question.
psql_super() { as_root -u postgres psql -w -v ON_ERROR_STOP=1 "$@"; }

# Identify the package manager. Only families this script can actually drive.
detect_package_manager() {
    if   have apt-get; then echo apt
    elif have dnf;     then echo dnf
    elif have pacman;  then echo pacman
    else echo unsupported
    fi
}

# Identify how services are managed. WSL commonly has no running systemd, and
# `systemctl` there fails in a way that reads like a broken install rather than
# an absent init, so this is detected rather than assumed.
detect_service_manager() {
    if have systemctl && systemctl is-system-running >/dev/null 2>&1; then
        echo systemd
    elif have systemctl && [ -d /run/systemd/system ]; then
        echo systemd
    elif have service; then
        echo sysv
    else
        echo none
    fi
}

# Start a service and report what happened. Returns non-zero when it cannot.
service_start() {
    local unit="$1" manager
    manager=$(detect_service_manager)
    case "$manager" in
        systemd) as_root systemctl start "$unit" >/dev/null 2>&1 ;;
        sysv)    as_root service "$unit" start   >/dev/null 2>&1 ;;
        *)       return 1 ;;
    esac
}

# Ask a service manager to start a unit at boot. Best-effort: a machine without
# systemd has no equivalent, and that is not a setup failure.
service_enable() {
    local unit="$1"
    [ "$(detect_service_manager)" = systemd ] || return 0
    as_root systemctl enable "$unit" >/dev/null 2>&1 || true
}

# Wait for a readiness probe to succeed, up to a deadline in seconds.
wait_for() {
    local seconds="$1"; shift
    local waited=0
    while [ "$waited" -lt "$seconds" ]; do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

# Install the build and runtime dependencies. Sourced by setup.sh.

# Package names differ per family; the set is the same.
#
#   build-essential / gcc + make   compiler and make
#   libhiredis-dev                 Redis client, used for sessions and tickets
#   libpq-dev                      PostgreSQL client, used for characters and items
#   libsodium-dev                  Argon2id password hashing in the login server
#   libsqlite3-dev                 the login server's account database
#   libssl-dev                     TLS on the login listener
#   openssl                        generates the login certificate in certs.sh
#   postgresql, postgresql-client  the character store
#   redis-server                   sessions, tickets, server-to-server keys

packages_for_apt='build-essential libhiredis-dev libpq-dev libsodium-dev libsqlite3-dev libssl-dev openssl postgresql postgresql-client redis-server'
packages_for_dnf='gcc make hiredis-devel libpq-devel libsodium-devel sqlite-devel openssl-devel openssl postgresql-server postgresql redis'
packages_for_pacman='base-devel hiredis postgresql-libs libsodium sqlite openssl postgresql redis'

# Report whether every header and library the build links against is present,
# so a machine that already has them is not made to talk to the network.
dependencies_present() {
    have gcc && have make && have openssl && have psql && have redis-cli \
      && [ -e /usr/include/hiredis/hiredis.h ] \
      && [ -e /usr/include/sodium.h ] \
      && [ -e /usr/include/sqlite3.h ] \
      && { [ -e /usr/include/libpq-fe.h ] || [ -e /usr/include/postgresql/libpq-fe.h ]; } \
      && { [ -e /usr/include/openssl/ssl.h ] || [ -e /usr/include/x86_64-linux-gnu/openssl/ssl.h ]; }
}

install_packages() {
    step "Dependencies"

    local manager
    manager=$(detect_package_manager)

    if dependencies_present; then
        same "every build and runtime dependency is already installed"
        return 0
    fi

    if [ "$manager" = unsupported ]; then
        warn "No apt, dnf, or pacman found. Install these yourself, then re-run:"
        note "  $packages_for_apt"
        return 1
    fi

    if ! can_be_root; then
        report_missing_root "installing packages"
        note "    install these yourself, then re-run: $packages_for_apt"
        return 1
    fi

    note "installing with $manager (this is the slow step)"
    case "$manager" in
        apt)
            as_root apt-get update -qq || { blocker "apt-get update failed"; return 1; }
            # shellcheck disable=SC2086
            DEBIAN_FRONTEND=noninteractive as_root apt-get install -y -qq $packages_for_apt \
                || { blocker "apt-get install failed"; return 1; }
            ;;
        dnf)
            # shellcheck disable=SC2086
            as_root dnf install -y -q $packages_for_dnf || { blocker "dnf install failed"; return 1; }
            ;;
        pacman)
            # shellcheck disable=SC2086
            as_root pacman -S --needed --noconfirm $packages_for_pacman || { blocker "pacman install failed"; return 1; }
            ;;
    esac

    made "dependencies installed"

    # Re-check rather than trust the installer's exit status: a package set that
    # installed cleanly but left a header missing produces a build failure three
    # steps later, where the cause is much harder to see.
    dependencies_present || warn "packages installed but some headers are still missing; the build may fail"
}

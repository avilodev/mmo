# Bring PostgreSQL and Redis up and confirm they answer. Sourced by setup.sh.

# On Debian/Ubuntu the postgres unit is "postgresql"; on Fedora and Arch the
# cluster must be initialised once before it will start at all.
postgres_unit() {
    if   [ -d /etc/postgresql ]; then echo postgresql
    elif have systemctl && systemctl list-unit-files 2>/dev/null | grep -q '^postgresql\.service'; then echo postgresql
    else echo postgresql
    fi
}

redis_unit() {
    if have systemctl && systemctl list-unit-files 2>/dev/null | grep -q '^redis-server\.service'; then
        echo redis-server
    elif [ -f /etc/init.d/redis-server ]; then
        echo redis-server
    else
        echo redis
    fi
}

# Fedora and Arch ship an uninitialised data directory. Debian and Ubuntu
# initialise one at install time, so this is a no-op there.
initialise_postgres_cluster_if_needed() {
    local data_dir
    for data_dir in /var/lib/pgsql/data /var/lib/postgres/data; do
        [ -d "$data_dir" ] || continue
        [ -f "$data_dir/PG_VERSION" ] && return 0
        note "initialising the PostgreSQL cluster at $data_dir"
        if have postgresql-setup; then
            as_root postgresql-setup --initdb >/dev/null 2>&1 && { made "cluster initialised"; return 0; }
        fi
        as_root -u postgres initdb -D "$data_dir" >/dev/null 2>&1 \
            && { made "cluster initialised"; return 0; }
        warn "could not initialise the cluster at $data_dir"
        return 1
    done
    return 0
}

start_services() {
    step "Services"

    local manager
    manager=$(detect_service_manager)
    note "service manager: $manager"

    initialise_postgres_cluster_if_needed

    # Only a stopped service needs root. A machine where both are already up
    # gets through this step without asking for anything.
    if ! pg_isready -q 2>/dev/null || ! redis-cli PING 2>/dev/null | grep -qx PONG; then
        if ! can_be_root; then
            report_missing_root "starting PostgreSQL and Redis"
            return 1
        fi
    fi

    if pg_isready -q 2>/dev/null; then
        same "PostgreSQL is already accepting connections"
    else
        service_start "$(postgres_unit)" || true
        if wait_for 20 pg_isready -q; then
            made "PostgreSQL started"
        else
            blocker "PostgreSQL did not come up"
            note "    Start it by hand and re-run:"
            note "      sudo service $(postgres_unit) start   # or: sudo systemctl start $(postgres_unit)"
            return 1
        fi
    fi
    service_enable "$(postgres_unit)"

    if redis-cli PING 2>/dev/null | grep -qx PONG; then
        same "Redis is already answering"
    else
        service_start "$(redis_unit)" || true
        if wait_for 20 bash -c 'redis-cli PING 2>/dev/null | grep -qx PONG'; then
            made "Redis started"
        else
            blocker "Redis did not come up"
            note "    Start it by hand and re-run:"
            note "      sudo service $(redis_unit) start      # or: sudo systemctl start $(redis_unit)"
            return 1
        fi
    fi
    service_enable "$(redis_unit)"

    # Worth saying plainly: on a machine without systemd these do not survive a
    # reboot, and the first symptom is start_servers.sh refusing to run.
    if [ "$manager" != systemd ]; then
        note "no systemd here — after a reboot, start both again:"
        note "  sudo service $(postgres_unit) start && sudo service $(redis_unit) start"
    fi
}

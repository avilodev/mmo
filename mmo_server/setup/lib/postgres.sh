# Give the postgres role a password, record it in ~/.pgpass, and create one
# database per world. Sourced by setup.sh.

PGPASS_FILE="$HOME/.pgpass"

# Generate a password that does not need quoting anywhere it will be used: a
# .pgpass line, a libpq connection string, and a shell environment variable.
generate_password() {
    if have openssl; then
        openssl rand -base64 24 | tr -dc 'A-Za-z0-9' | head -c 24
    else
        tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 24
    fi
}

# Report whether TCP auth as postgres already works with what libpq can find
# (a ~/.pgpass entry, PGPASSWORD, or a trust rule in pg_hba.conf).
#
# -w so a missing credential fails immediately instead of prompting: this runs
# before setup knows whether it will need to write one, and an interactive
# prompt here would stall the script with no indication of what it wants.
postgres_tcp_auth_works() {
    PGCONNECT_TIMEOUT=5 psql -w -h localhost -U postgres -d postgres -Atqc 'SELECT 1' 2>/dev/null \
        | grep -qx 1
}

# Add or replace this host's line in ~/.pgpass.
#
# ~/.pgpass rather than an exported PGPASSWORD or a password in a connection
# string: libpq reads it automatically, so nothing has to carry the secret
# through an environment that /proc exposes to every process of this user.
write_pgpass_line() {
    local user="$1" password="$2"
    local line="localhost:5432:*:$user:$password"

    touch "$PGPASS_FILE"
    chmod 600 "$PGPASS_FILE"

    # Drop any previous entry for this host/user pair before appending, so
    # re-running setup does not leave a stale password shadowing the new one.
    if [ -s "$PGPASS_FILE" ]; then
        grep -v "^localhost:5432:\*:$user:" "$PGPASS_FILE" > "$PGPASS_FILE.tmp" 2>/dev/null || true
        mv "$PGPASS_FILE.tmp" "$PGPASS_FILE"
        chmod 600 "$PGPASS_FILE"
    fi

    printf '%s\n' "$line" >> "$PGPASS_FILE"
    chmod 600 "$PGPASS_FILE"
}

write_pgpass_entry() { write_pgpass_line postgres "$1"; }

configure_postgres_auth() {
    step "PostgreSQL authentication"

    if postgres_tcp_auth_works; then
        same "postgres role already authenticates over TCP (credentials found by libpq)"
        return 0
    fi

    if ! can_be_root; then
        report_missing_root "setting the postgres role's password"
        note "    without it the servers cannot reach their databases"
        return 1
    fi

    local password
    password=$(generate_password)
    [ -n "$password" ] || { blocker "could not generate a password"; return 1; }

    if ! psql_super -c "ALTER USER postgres WITH PASSWORD '$password';" </dev/null >/dev/null 2>&1; then
        blocker "could not set a password for the postgres role"
        note "    'sudo -u postgres psql' failed. Check that PostgreSQL is running and that"
        note "    the postgres system account exists."
        return 1
    fi
    made "set a generated password for the postgres role"

    write_pgpass_entry "$password"
    made "recorded it in $PGPASS_FILE (mode 600)"

    if ! postgres_tcp_auth_works; then
        blocker "the postgres role still will not authenticate over TCP"
        note "    Check that pg_hba.conf allows md5 or scram-sha-256 for host connections"
        note "    from 127.0.0.1, then re-run 'make setup'."
        return 1
    fi

    ok "TCP authentication verified"
    note "the servers and start_servers.sh pick this up automatically; PGPASSWORD is not needed"
}

create_world_databases() {
    step "World databases"

    if ! can_be_root; then
        report_missing_root "creating the world databases"
        note "    every world server exits at startup without its database"
        return 1
    fi

    local name region database host port max_players hardcore realm_port
    local created=0 existing=0 failed=0

    while IFS=$'\t' read -r name region database host port max_players hardcore realm_port; do
        # </dev/null: psql inside a while-read loop drains the loop's stdin
        # otherwise, and only the first world would be created.
        if psql_super -tAc "SELECT 1 FROM pg_database WHERE datname='$database'" </dev/null 2>/dev/null | grep -qx 1; then
            same "$database"
            existing=$((existing + 1))
        else
            if psql_super -c "CREATE DATABASE $database;" </dev/null >/dev/null 2>&1; then
                made "$database"
                created=$((created + 1))
            else
                blocker "could not create database $database"
                failed=$((failed + 1))
            fi
        fi
    done < <(world_rows)

    if [ "$failed" -eq 0 ]; then
        ok "$created created, $existing already present"
    else
        warn "$created created, $existing already present, $failed failed"
    fi

    # Deliberately no table creation here.
    #
    # character_database_init() issues the CREATE TABLE and the ALTERs for the
    # characters, character_items and character_currencies tables at world
    # startup, and it is the only definition of that schema. A second copy in
    # setup would be a copy that drifts -- and because both use CREATE TABLE IF
    # NOT EXISTS, whichever ran first would silently win, leaving a table the
    # server believes it created and did not.
    note "tables are created by the servers on first start, which own the schema"
}

# The role the servers should actually connect as.
#
# Every world server has connected as `postgres`, the cluster superuser, since
# the beginning. That means one SQL injection anywhere, or one compromised
# world process, is not "that world's characters" -- it is every world's
# database, every other role, COPY TO PROGRAM, and the cluster itself. The
# servers need SELECT, INSERT, UPDATE and DELETE on their own world's tables
# and nothing else.
#
# The role is created and granted here, and the password is written to
# ~/.pgpass beside the postgres one, but `pg_user` in worlds.conf is NOT
# switched automatically: the servers create their own schema on first start
# and that needs table ownership, so an existing deployment has to migrate its
# tables to the new owner rather than have setup silently break its startup.
# The summary says what to do.
#
# Idempotent: an existing role keeps its password, and re-granting is a no-op.
APP_ROLE=${MMO_PG_APP_ROLE:-mmo_app}

create_app_role() {
    step "Least-privilege database role"

    if ! can_be_root; then
        report_missing_root "creating the $APP_ROLE role"
        return 1
    fi

    local created=0
    if psql_super -tAc "SELECT 1 FROM pg_roles WHERE rolname='$APP_ROLE'" </dev/null 2>/dev/null | grep -qx 1; then
        same "role $APP_ROLE"
    else
        local password
        password=$(generate_password)
        if psql_super -c "CREATE ROLE $APP_ROLE LOGIN PASSWORD '$password';" </dev/null >/dev/null 2>&1; then
            made "role $APP_ROLE"
            write_pgpass_line "$APP_ROLE" "$password"
            created=1
        else
            blocker "could not create role $APP_ROLE"
            return 1
        fi
    fi

    # Grants, per world database. CONNECT plus DML on what exists today, and a
    # default privilege so tables the servers create later are covered too --
    # without that, every new table would need this step re-run.
    local name region database host port max_players hardcore realm_port
    local granted=0
    while IFS=$'\t' read -r name region database host port max_players hardcore realm_port; do
        psql_super -c "GRANT CONNECT ON DATABASE $database TO $APP_ROLE;" </dev/null >/dev/null 2>&1 || true
        psql_super -d "$database" -c "GRANT USAGE ON SCHEMA public TO $APP_ROLE;" </dev/null >/dev/null 2>&1 || true
        psql_super -d "$database" -c "GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO $APP_ROLE;" </dev/null >/dev/null 2>&1 || true
        psql_super -d "$database" -c "GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO $APP_ROLE;" </dev/null >/dev/null 2>&1 || true
        psql_super -d "$database" -c "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO $APP_ROLE;" </dev/null >/dev/null 2>&1 || true
        psql_super -d "$database" -c "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT USAGE, SELECT ON SEQUENCES TO $APP_ROLE;" </dev/null >/dev/null 2>&1 || true
        granted=$((granted + 1))
    done < <(world_rows)

    ok "$APP_ROLE may read and write $granted world database(s), and nothing else"

    if [ "$created" = 1 ]; then
        note "    the servers still connect as 'postgres' until you set it:"
        note "      pg_user = $APP_ROLE      in setup/worlds.conf"
        note "    the schema is created by whoever connects first, so migrate"
        note "    table ownership before switching an existing deployment:"
        note "      ALTER TABLE <name> OWNER TO $APP_ROLE;"
    fi
}


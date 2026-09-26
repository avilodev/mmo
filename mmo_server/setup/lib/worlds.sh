# Read setup/worlds.conf and generate everything derived from it.
# Sourced by setup.sh.

WORLDS_CONF="$PROJECT_ROOT/setup/worlds.conf"

# Emit one world per line as TAB-separated fields:
#
#   name <TAB> region <TAB> database <TAB> host <TAB> port <TAB> max_players <TAB> hardcore <TAB> realm_port
#
# Tabs rather than spaces because a region is "North America". With space
# separation every consumer would have to know which field can contain a space,
# and the first one that forgot would silently shift every column after it.
# Consumers read these with IFS=$'\t'.
#
# realm_port is the eighth column and is optional in the file: the realm link
# has its own listener now, and a row that does not name a port for it gets
# port + WORLD_REALM_PORT_OFFSET, exactly as common/src/world_table.c does.
# Every row emitted here carries it, so consumers never have to derive it.
world_rows() {
    [ -f "$WORLDS_CONF" ] || die "missing $WORLDS_CONF"
    awk '
        /^[[:space:]]*#/ { next }
        /^[[:space:]]*$/ { next }
        NF != 7 && NF != 8 {
            printf("worlds.conf line %d has %d columns, expected 7 or 8\n", NR, NF) > "/dev/stderr"
            bad = 1
            exit 1
        }
        {
            region = $2
            gsub(/_/, " ", region)
            realm_port = (NF == 8) ? $8 : ($5 + 1000)
            printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                   $1, region, $3, $4, $5, $6, $7, realm_port)
        }
        END { if (bad) exit 1 }
    ' "$WORLDS_CONF" || die "worlds.conf is malformed"
}

world_count() { world_rows | wc -l; }

# Regions, in first-appearance order, one per line.
world_regions() {
    world_rows | cut -f2 | awk '!seen[$0]++'
}

# How many worlds a region holds.
world_region_count() {
    world_rows | cut -f2 | grep -cxF "$1"
}

# Reject a worlds.conf that would produce a broken deployment, before anything
# is written. Each of these is a failure that would otherwise surface much later
# as a server refusing to start, with nothing pointing back at this file.
validate_world_table() {
    step "World table"

    local count
    count=$(world_count)
    [ "$count" -gt 0 ] || die "setup/worlds.conf lists no worlds"

    local max_worlds
    max_worlds=$(grep -oE '#define MAX_WORLDS[[:space:]]+[0-9]+' \
        "$PROJECT_ROOT/realm_server/include/world_database_manager.h" 2>/dev/null \
        | grep -oE '[0-9]+$')
    max_worlds=${max_worlds:-10}

    if [ "$count" -gt "$max_worlds" ]; then
        die "worlds.conf lists $count worlds but MAX_WORLDS is $max_worlds.
       Raise MAX_WORLDS in realm_server/include/world_database_manager.h and rebuild,
       or remove worlds from setup/worlds.conf."
    fi

    local max_players_cap
    max_players_cap=$(grep -oE '#define MAX_PLAYERS[[:space:]]+[0-9]+' \
        "$PROJECT_ROOT/common/include/types.h" 2>/dev/null | grep -oE '[0-9]+$')
    max_players_cap=${max_players_cap:-1000}

    local name region database host port max_players hardcore realm_port
    local seen_names='' seen_ports='' seen_dbs=''

    while IFS=$'\t' read -r name region database host port max_players hardcore realm_port; do
        case " $seen_names " in *" $name "*) die "worlds.conf lists $name twice";; esac
        case " $seen_dbs "   in *" $database "*) die "worlds.conf uses database $database twice";; esac
        seen_names="$seen_names $name"
        seen_dbs="$seen_dbs $database"

        # Client ports and realm ports share one namespace: they are bound by
        # the same process on the same host, so a collision between a world's
        # client port and another world's realm port fails at startup with a
        # bind error and no explanation of which two rows disagreed.
        case " $seen_ports " in *" $port "*) die "worlds.conf uses port $port twice";; esac
        seen_ports="$seen_ports $port"
        case " $seen_ports " in *" $realm_port "*) die "worlds.conf uses port $realm_port twice (realm port of $name)";; esac
        seen_ports="$seen_ports $realm_port"

        case "$port" in ''|*[!0-9]*) die "$name has a non-numeric port '$port'";; esac
        [ "$port" -ge 1 ] && [ "$port" -le 65535 ] || die "$name has port $port outside 1..65535"

        case "$realm_port" in ''|*[!0-9]*) die "$name has a non-numeric realm port '$realm_port'";; esac
        [ "$realm_port" -ge 1 ] && [ "$realm_port" -le 65535 ] \
            || die "$name has realm port $realm_port outside 1..65535"

        case "$max_players" in ''|*[!0-9]*) die "$name has a non-numeric max_players";; esac
        [ "$max_players" -ge 1 ] || die "$name has max_players below 1"
        [ "$max_players" -le "$max_players_cap" ] \
            || die "$name asks for $max_players players but this build supports $max_players_cap
       (MAX_PLAYERS in common/include/types.h). The world would refuse to start."

        case "$hardcore" in 0|1) ;; *) die "$name has hardcore '$hardcore'; must be 0 or 1";; esac

        # The world .conf stores the address in a char[16], so an IPv4 literal
        # is all that fits. Catching it here beats a truncated address later.
        [ "${#host}" -lt 16 ] || die "$name has host '$host'; the world config field holds 15 characters"
    done < <(world_rows)

    ok "$count worlds, $(world_regions | wc -l) regions, no duplicate names, ports (client or realm), or databases"
}

# Write one world's .conf in the positional layout set_config() parses.
write_world_config() {
    local name="$1" region="$2" host="$3" port="$4" max_players="$5" hardcore="$6"
    local path="$PROJECT_ROOT/world_server/world_config/$name.conf"

    cat > "$path" <<CONF
# Server Name
$name
# Region
$region
# IP:Port
$host:$port
# Max Players
$max_players
# Hardcore (0 = No, 1 = Yes)
$hardcore

# ---------------------------------------------------------------------------
# The five values above are generated by setup/setup.sh from setup/worlds.conf.
#
# Change a world's address, capacity, or region there and re-run 'make setup',
# so worlds.txt, the database list, and the key rotation script stay in
# agreement with this file. Editing here alone puts them out of step, and
# 'make setup-check' will say so.
#
# Everything below is yours; setup will not overwrite a file whose block above
# already matches worlds.conf.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# NPC pool size (optional)
#
# How many NPCs this world can hold at once. The pool is allocated at startup
# and never grows, so this is the world's hard entity ceiling. Costs roughly
# 250 bytes per slot across the pool, its locks, and the tick snapshot.
#
# Left commented out, the compiled default in npc_world.h applies.
# ---------------------------------------------------------------------------
# max_npcs = 256

# ---------------------------------------------------------------------------
# Realm handshake admission (optional)
#
# The realm-to-world handshake has its own listener, on the world's client
# port + 1000 unless setup/worlds.conf gives an eighth column. Nothing on the
# player port can reach it. These settings bound what a peer that *can* reach
# it is able to make the world do before it has proved anything.
#
# realm_port -- move the realm listener. Normally left unset: worlds.conf is
#   what both ends read, so changing it there keeps the realm in agreement.
#   Setting it here changes only this world and will not be picked up by the
#   realm unless worlds.conf says the same thing.
#
# realm_bind -- interface the realm listener binds to. Unset means all of them.
#   Set it to the private address when the realm is on a separate network from
#   your players; that is a stronger boundary than the allowlist, because the
#   socket never exists on the public interface at all.
#
# realm_allow -- addresses permitted to open the realm listener. Repeatable; each
#   line takes one address or CIDR block. Left unset entirely, the world allows
#   127.0.0.0/8, ::1, and this world's own configured address above -- which
#   together cover any deployment where the realm and the world share a host,
#   including one where worlds.txt names a routable address so remote clients
#   can reach it. List your realm hosts explicitly when the realm runs on a
#   different machine. A rule that does not parse is refused and logged, never
#   stored as something wider. The world prints the list in effect at startup.
#
# realm_max_handlers -- ceiling on realm handler threads running at once.
#   Each handler blocks for up to 20 seconds before authentication decides
#   anything, so this is what stops an unbounded thread spawn from a peer that
#   has not proved anything yet. Default 4; a world talks to few realms.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Online roster (optional)
#
# session_list -- answer PACKET_SESSION_LIST_REQUEST. Off by default.
#   The request returns a paginated list of every online player's id, name,
#   level, race and ping to any authenticated client, scoped to nothing: not to
#   who is nearby, not to a party, not to a friend list. That is a complete and
#   refreshable census of who is playing. Turn it on only if you actually want
#   a public "who" list.
# ---------------------------------------------------------------------------
# session_list = on

# realm_port = 8778
# realm_bind = 10.0.0.5
# realm_allow = 10.0.0.0/24
# realm_max_handlers = 4

# ---------------------------------------------------------------------------
# Packet budget overrides (optional)
#
# Every inbound packet is charged against two buckets: its class bucket, and an
# overall bucket covering all traffic from that connection. A packet is allowed
# only if both can pay. Rate is sustained tokens per second; burst is how much
# can accumulate while idle.
#
# Anything left commented out keeps the compiled default from
# common/src/limit_profiles.c. Values shown are those defaults.
#
# Costs are per opcode and are NOT configurable here -- an expensive packet
# costs more tokens than a cheap one, and that pricing lives with the protocol.
# ---------------------------------------------------------------------------
# limit_overall_rate      = 200
# limit_overall_burst     = 400
# limit_movement_rate     = 150
# limit_movement_burst    = 300
# limit_combat_rate       = 30
# limit_combat_burst      = 60
# limit_item_rate         = 20
# limit_item_burst        = 40
# limit_social_rate       = 5
# limit_social_burst      = 15
# limit_query_rate        = 20
# limit_query_burst       = 40
#
# Drops tolerated inside one window before the connection is closed.
# limit_violation_limit   = 200
# limit_violation_window  = 10
CONF
}

# Read back the five positional values a .conf currently holds.
read_world_config_block() {
    grep -v '^#' "$1" 2>/dev/null | grep -v '^[[:space:]]*$' | head -5 | sed 's/[[:space:]]*$//'
}

generate_world_configs() {
    step "World configuration files"

    mkdir -p "$PROJECT_ROOT/world_server/world_config"

    local name region database host port max_players hardcore realm_port
    local wrote=0 kept=0 stale=0

    while IFS=$'\t' read -r name region database host port max_players hardcore realm_port; do
        local path="$PROJECT_ROOT/world_server/world_config/$name.conf"
        local expected
        expected=$(printf '%s\n%s\n%s:%s\n%s\n%s' "$name" "$region" "$host" "$port" "$max_players" "$hardcore")

        if [ -f "$path" ] && [ "${SETUP_FORCE_CONFIGS:-0}" != 1 ]; then
            if [ "$(read_world_config_block "$path")" = "$expected" ]; then
                same "$name.conf"
                kept=$((kept + 1))
                continue
            fi
            warn "$name.conf disagrees with worlds.conf — left untouched"
            note "    re-run as: FORCE_CONFIGS=1 make setup   (overwrites hand edits)"
            stale=$((stale + 1))
            continue
        fi

        write_world_config "$name" "$region" "$host" "$port" "$max_players" "$hardcore"
        made "$name.conf"
        wrote=$((wrote + 1))
    done < <(world_rows)

    ok "$wrote written, $kept already correct, $stale out of step"
}

generate_key_rotation_script() {
    step "Server key rotation"

    local path="$PROJECT_ROOT/common/server_keys/generate_daily_server_keys.sh"
    mkdir -p "$(dirname "$path")"

    local tmp
    tmp=$(mktemp)
    {
        cat <<'HEAD'
#!/bin/bash
# Rotate the server-to-server authentication keys held in Redis.
#
# GENERATED by setup/setup.sh from setup/worlds.conf. Re-run 'make setup' after
# adding a world rather than editing the world list here.
#
# ROTATION IS OVERLAPPING. Before a name's key is replaced, the value it is
# replacing is copied to server_auth_key:<name>:previous with a short TTL, and
# a world accepts either. Without that, the instant this script ran, a realm
# holding the value it read moments ago was refused -- and because the realm
# caches a key for a whole probe cycle, a rotation reliably landed inside one.
#
# ABSENCE IS AN OUTAGE. The realm refuses to start without
# server_auth_key:global, and a world with no key of its own refuses every
# realm handshake, which drops it off the world list with nothing said about
# why. This script exits non-zero if any key is missing after a run, so cron
# mail or a monitor notices. Check on it separately with --verify.
#
# Run from cron more often than the TTL:
#
#   0 3 * * * cd /path/to/mmo_server && bash common/server_keys/generate_daily_server_keys.sh
#
# Usage:
#   generate_daily_server_keys.sh            rotate every key
#   generate_daily_server_keys.sh --verify   check every key exists, change nothing
set -euo pipefail

REDIS_CLI=${REDIS_CLI:-redis-cli}
TTL=${SERVER_KEY_TTL:-86400}

# How long the key being replaced stays valid. Must be at least as long as the
# realm's world-probe cycle, and is capped well below the TTL so an old key
# never outlives the rotation that replaced it by much. Keep this in step with
# SERVER_KEY_OVERLAP_SECONDS in common/include/realm_world_auth.h.
OVERLAP=${SERVER_KEY_OVERLAP:-900}

VERIFY_ONLY=0
[ "${1:-}" = "--verify" ] && VERIFY_ONLY=1

MISSING=0
NAMES=()

set_key() {
    local name="$1"
    NAMES+=("$name")

    if [ "$VERIFY_ONLY" = 1 ]; then
        return 0
    fi

    # Keep the value being replaced usable for the overlap window. A peer that
    # read the old key seconds ago must still authenticate with it.
    local previous
    previous=$($REDIS_CLI GET "server_auth_key:$name" 2>/dev/null || true)
    if [ -n "$previous" ]; then
        $REDIS_CLI SETEX "server_auth_key:$name:previous" "$OVERLAP" "$previous" >/dev/null
    fi

    $REDIS_CLI SETEX "server_auth_key:$name" "$TTL" "$(openssl rand -hex 32)" >/dev/null
    echo "  rotated server_auth_key:$name"
}

# Fail loudly if a name ended up without a key. A silently missing key is the
# failure mode this whole script exists to prevent.
check_keys() {
    local name
    for name in "${NAMES[@]}"; do
        if [ "$($REDIS_CLI EXISTS "server_auth_key:$name")" != "1" ]; then
            echo "  MISSING server_auth_key:$name" >&2
            MISSING=$((MISSING + 1))
        fi
    done
}

if [ "$VERIFY_ONLY" = 1 ]; then
    echo "Verifying server auth keys (changing nothing)"
else
    echo "Rotating server auth keys (TTL ${TTL}s, previous key valid ${OVERLAP}s)"
fi

# The global key. The realm presents this to any world with no key of its own.
set_key global

# Per-world keys, from setup/worlds.conf.
HEAD
        local name rest
        while IFS=$'\t' read -r name rest; do
            printf 'set_key %s\n' "$name"
        done < <(world_rows)
        cat <<'TAIL'

check_keys

echo
if [ "$MISSING" -ne 0 ]; then
    echo "FAILED: $MISSING key(s) missing. The realm will not start and any world"
    echo "        without a key will refuse every realm handshake." >&2
    exit 1
fi

if [ "$VERIFY_ONLY" = 1 ]; then
    echo "All ${#NAMES[@]} keys present."
else
    echo "Done. Keys expire in ${TTL}s; schedule this more often than that."
fi
TAIL
    } > "$tmp"

    if [ -f "$path" ] && cmp -s "$tmp" "$path"; then
        same "generate_daily_server_keys.sh already matches worlds.conf"
        rm -f "$tmp"
    else
        mv "$tmp" "$path"
        made "generate_daily_server_keys.sh ($(world_count) worlds + global)"
    fi
    chmod +x "$path"
}

# Verify the world roster is packaged where the servers will look for it.
#
# world_table.c reads worlds.conf at startup -- the realm for its world list,
# every world server for its own database, both for display names. The file is
# copied beside each binary by the Makefile, so a build that predates a
# worlds.conf edit leaves a stale roster next to the executables. There is no
# longer a compiled copy of the mapping to drift from, which is what the old
# cross-check against world_database_config.c existed to catch.
check_worlds_conf_deployed() {
    step "Deployed world roster"

    local source="$PROJECT_ROOT/setup/worlds.conf"
    local drift=0 found=0

    local target
    for target in realm_server/bin/data/worlds.conf \
                  world_server/bin/data/worlds.conf \
                  login_server/bin/data/worlds.conf; do
        local path="$PROJECT_ROOT/$target"
        [ -f "$path" ] || continue
        found=$((found + 1))
        if cmp -s "$source" "$path"; then
            same "$target"
        else
            warn "$target is out of date"
            drift=1
        fi
    done

    if [ "$found" -eq 0 ]; then
        note "    no built binaries yet; 'make' packages worlds.conf beside each one"
        return 0
    fi

    if [ "$drift" -ne 0 ]; then
        warn "a deployed worlds.conf disagrees with setup/worlds.conf"
        note "    run 'make' to repackage it"
        return 1
    fi

    ok "all $found deployed copies match setup/worlds.conf"
}

# Create the runtime directories and produce world.dat. Sourced by setup.sh.

create_runtime_directories() {
    step "Runtime directories"

    # Each of these is created by hand today and each has a distinct failure
    # when it is missing: the SQLite one aborts the login server at open time,
    # the quest one silently drops every quest save, and the data one makes the
    # world server refuse to start.
    local dirs=(
        "database/databases/users_data"
        "database/databases/postgres_data"
        "login_server/certs"
        "world_server/data"
        "world_server/data/quests"
        "world_server/world_config"
        "realm_server/worlds"
        "common/server_keys"
    )

    local created=0
    local dir
    for dir in "${dirs[@]}"; do
        if [ -d "$PROJECT_ROOT/$dir" ]; then
            same "$dir"
        else
            mkdir -p "$PROJECT_ROOT/$dir" || { blocker "could not create $dir"; continue; }
            made "$dir"
            created=$((created + 1))
        fi
    done

    ok "$created created"
}

WORLD_DAT="$PROJECT_ROOT/world_server/data/world.dat"

# Build the client's world generator natively and produce world.dat.
#
# The map is authored by the client and consumed by both halves, and the two
# copies have to be byte-identical: the server reads its collision layer by
# seeking past a tile section whose size it computes from the header, so a file
# the client wrote differently is not detected, it is misread. Generating both
# from one run of one binary is what makes that true by construction.
generate_world_dat() {
    step "World map (world.dat)"

    if [ -f "$WORLD_DAT" ]; then
        same "world.dat present ($(du -h "$WORLD_DAT" | cut -f1))"
        return 0
    fi

    if [ ! -d "$CLIENT_ROOT" ]; then
        warn "world.dat is missing and the client tree is not at $CLIENT_ROOT"
        note "    The world server refuses to start without it: with no collision grid it"
        note "    cannot tell open ground from a wall, so movement would go unvalidated."
        note "    Generate it in the client tree with 'make world', or set MMO_CLIENT_ROOT."
        return 1
    fi

    local gen_src=(
        "$CLIENT_ROOT/Game/data/src/world_generator.c"
        "$CLIENT_ROOT/Game/data/src/worldgen_noise.c"
        "$CLIENT_ROOT/Game/data/src/worldgen_biome.c"
        "$CLIENT_ROOT/Game/data/src/worldgen_city.c"
        "$CLIENT_ROOT/Game/data/src/worldgen_tile.c"
        "$CLIENT_ROOT/Game/data/src/worldgen_write.c"
        "$CLIENT_ROOT/Game/data/src/worldgen_overview.c"
    )

    local missing=0 src
    for src in "${gen_src[@]}"; do
        [ -f "$src" ] || { warn "missing $src"; missing=1; }
    done
    [ "$missing" -eq 0 ] || { warn "cannot build the world generator"; return 1; }

    local gen_bin="$CLIENT_ROOT/Game/data/bin/worldgen"
    mkdir -p "$(dirname "$gen_bin")"

    note "building the world generator"
    gcc -Wall -std=c11 -O2 \
        -I"$CLIENT_ROOT/Game/include" -I"$PROJECT_ROOT/common/include" \
        -o "$gen_bin" "${gen_src[@]}" -lm \
        || { warn "the world generator did not build"; return 1; }

    note "generating the map (this takes a while and writes about 115 MB)"
    local client_dat="$CLIENT_ROOT/Game/bin/world.dat"
    mkdir -p "$(dirname "$client_dat")"
    "$gen_bin" "$client_dat" >/dev/null || { warn "world generation failed"; return 1; }

    cp "$client_dat" "$WORLD_DAT" || { warn "could not copy world.dat to the server"; return 1; }

    cmp -s "$client_dat" "$WORLD_DAT" \
        || { warn "the two world.dat copies differ after the copy"; return 1; }

    made "world.dat generated and installed to both trees ($(du -h "$WORLD_DAT" | cut -f1))"
}

# Confirm the two world.dat copies still match. Nothing enforces this at build
# time, and a mismatch is not detected at runtime -- the server misreads the
# file rather than rejecting it.
check_world_dat_matches_client() {
    [ -f "$WORLD_DAT" ] || return 0
    [ -d "$CLIENT_ROOT" ] || return 0

    local client_dat="$CLIENT_ROOT/Game/bin/world.dat"
    [ -f "$client_dat" ] || return 0

    if cmp -s "$client_dat" "$WORLD_DAT"; then
        same "world.dat matches the client's copy"
    else
        warn "the server's world.dat differs from the client's"
        note "    The server derives its collision map by seeking past a tile section it"
        note "    sizes from the header, so a mismatch is misread rather than refused."
        note "    Re-copy: cp '$client_dat' '$WORLD_DAT'"
    fi
}

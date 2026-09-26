#!/bin/sh
#
# Fail when the two world_format.h copies stop describing the same file.
#
# world.dat is written by the client's generator and read by two independent
# programs: the client, to draw it, and the world server, to load the collision
# layer every movement check is validated against. Neither tree can include
# from the other, so the layout is declared in a header duplicated in both --
# the same arrangement protocol.h has, and it drifts the same way.
#
# What drift costs here is worse than a refused connection. Before the format
# carried a magic number and a layer count, a fifth tile layer added on the
# client made the server's seek past the tile data land inside it, and the
# server loaded tile indices as its collision map: no crash, no error, just a
# world whose walls are somewhere else while movement validation agrees.

set -u

cd "$(dirname "$0")/.." || exit 1

SERVER="common/include/world_format.h"
CLIENT="../mmo_client/common/world_format.h"

if [ ! -f "$SERVER" ]; then
    printf '  missing %s\n' "$SERVER"
    exit 1
fi

if [ ! -f "$CLIENT" ]; then
    printf '  SKIPPED: no client tree at %s\n' "$CLIENT"
    exit 0
fi

# Compared after preprocessing, so the two copies must agree on every line the
# compiler acts on while remaining free to explain themselves differently.
strip() {
    ${CC:-cc} -fpreprocessed -dD -E -P -x c "$1" 2>/dev/null \
        | sed 's/[[:space:]]*$//' \
        | grep -v '^$'
}

server_decls=$(strip "$SERVER")
client_decls=$(strip "$CLIENT")

if [ -z "$server_decls" ]; then
    printf '  could not read declarations out of %s\n' "$SERVER"
    exit 1
fi

if [ "$server_decls" != "$client_decls" ]; then
    printf '  The two world_format.h copies no longer describe the same file.\n\n'
    printf '  Apply the change to both:\n\n'
    printf '      %s\n' "$SERVER"
    printf '      %s\n\n' "$CLIENT"
    printf '  < server   > client\n\n'

    tmp_server=$(mktemp) || exit 1
    tmp_client=$(mktemp) || { rm -f "$tmp_server"; exit 1; }
    printf '%s\n' "$server_decls" > "$tmp_server"
    printf '%s\n' "$client_decls" > "$tmp_client"
    diff "$tmp_server" "$tmp_client" | sed 's/^/    /' | head -40
    rm -f "$tmp_server" "$tmp_client"
    exit 1
fi

# The declarations agree. Now check that everything which reads or writes the
# file goes through them, rather than carrying its own copy of the numbers.
fail=0

check_uses_header() {
    file=$1
    what=$2
    [ -f "$file" ] || return 0
    if ! grep -q 'world_format\.h' "$file"; then
        printf '  %s does not include world_format.h\n' "$what"
        fail=1
    fi
}

check_uses_header "world_server/src/world_collision.c"        "the server collision reader"
check_uses_header "../mmo_client/Game/src/world/world.c"      "the client world reader"
check_uses_header "../mmo_client/Game/data/src/worldgen_write.c" "the world generator"

if [ "$fail" -ne 0 ]; then
    printf '\n  A reader that hardcodes the layout is a reader that cannot be told\n'
    printf '  the layout changed. That is the defect the version field exists for.\n'
    exit 1
fi

# And that a generated world.dat actually carries the magic, when one exists.
for world in world_server/bin/data/world.dat world_server/data/world.dat; do
    [ -f "$world" ] || continue
    magic=$(dd if="$world" bs=1 count=8 2>/dev/null)
    if [ "$magic" != "MMOWORLD" ]; then
        printf '  %s predates the versioned format (no magic number).\n' "$world"
        printf '  Regenerate it: `make world` in the client tree, or `make setup` here.\n'
        exit 1
    fi
    printf '  %s carries the format magic\n' "$world"
done

lines=$(printf '%s\n' "$server_decls" | wc -l | tr -d ' ')
printf '  both world_format.h trees describe the same %s lines of file layout\n' "$lines"
exit 0

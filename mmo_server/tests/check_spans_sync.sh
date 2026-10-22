#!/bin/sh
#
# Fail when the server and client copies of the world_spans and world_seats modules drift.
#
# Both trees compile the same span-column code: the server validates moves
# against it and the client predicts them with it. Neither tree can include
# from the other, so the files are duplicated and must stay byte-identical.

set -u

cd "$(dirname "$0")/.." || exit 1

CLIENT_DIR="../mmo_client/common"

if [ ! -d "$CLIENT_DIR" ]; then
    printf '  SKIPPED: no client tree at %s\n' "$CLIENT_DIR"
    exit 0
fi

fail=0

check_pair() {
    server=$1
    client=$2
    if [ ! -f "$server" ]; then
        printf '  missing %s\n' "$server"
        fail=1
    elif [ ! -f "$client" ]; then
        printf '  missing %s (copy it from %s)\n' "$client" "$server"
        fail=1
    elif ! cmp -s "$server" "$client"; then
        printf '  DRIFTED: %s differs from %s\n' "$client" "$server"
        fail=1
    fi
}

check_pair "common/include/world_spans_format.h" "$CLIENT_DIR/world_spans_format.h"
check_pair "common/include/world_spans.h"        "$CLIENT_DIR/world_spans.h"
check_pair "common/src/world_spans.c"            "$CLIENT_DIR/world_spans.c"
check_pair "common/include/world_motion.h"       "$CLIENT_DIR/world_motion.h"
check_pair "common/include/world_seats.h"        "$CLIENT_DIR/world_seats.h"
check_pair "common/src/world_seats.c"            "$CLIENT_DIR/world_seats.c"
check_pair "common/src/world_motion.c"           "$CLIENT_DIR/world_motion.c"

if [ "$fail" -ne 0 ]; then
    printf '\n  Apply the change to both trees; the copies must be identical.\n'
    exit 1
fi

printf '  both world_spans and world_motion copies are identical\n'
exit 0

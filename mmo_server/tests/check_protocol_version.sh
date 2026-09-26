#!/bin/sh
#
# Fail when the wire format changed but PROTOCOL_VERSION did not.
#
# The client and server trees are independent: each carries its own copy of
# protocol.h, each builds and ships on its own hardware, and neither includes
# from the other. Nothing structural can therefore notice the two copies
# drifting apart. PROTOCOL_VERSION is what notices, at runtime, on connect —
# but only if it is actually bumped when the format changes, and the one thing
# nobody remembers to do is bump a version constant.
#
# So: this script fingerprints the wire headers. If they changed and the
# version did not, the build stops here rather than shipping a client and a
# server that both claim version N and disagree about what N means.
#
# The matching script in the client tree does exactly the same thing over its
# own copies. Run both; they share no state.

set -u

cd "$(dirname "$0")/.." || exit 1

# Every header whose contents define the wire format.
WIRE_HEADERS="common/include/protocol.h common/include/world_regions.h"

RECORD="tests/protocol_version.sha256"

for h in $WIRE_HEADERS; do
    if [ ! -f "$h" ]; then
        printf '  missing wire header: %s\n' "$h"
        exit 1
    fi
done

version=$(sed -n 's/^[[:space:]]*#[[:space:]]*define[[:space:]]\{1,\}PROTOCOL_VERSION[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' \
          common/include/protocol.h | head -1)

if [ -z "$version" ]; then
    printf '  PROTOCOL_VERSION is not defined in common/include/protocol.h\n'
    exit 1
fi

# One fingerprint over all wire headers, order-stable.
hash=$(cat $WIRE_HEADERS | sha256sum | cut -d' ' -f1)

if [ ! -f "$RECORD" ]; then
    printf '%s %s\n' "$version" "$hash" > "$RECORD"
    printf '  recorded initial fingerprint for PROTOCOL_VERSION %s\n' "$version"
    exit 0
fi

recorded_version=$(cut -d' ' -f1 < "$RECORD")
recorded_hash=$(cut -d' ' -f2 < "$RECORD")

if [ "$hash" = "$recorded_hash" ]; then
    if [ "$version" != "$recorded_version" ]; then
        printf '  PROTOCOL_VERSION moved %s -> %s with no change to the wire headers.\n' \
               "$recorded_version" "$version"
        printf '  That is allowed, but check it was intended, then re-record with:\n'
        printf '      rm %s && ./tests/check_protocol_version.sh\n' "$RECORD"
        exit 1
    fi
    printf '  wire format unchanged at PROTOCOL_VERSION %s\n' "$version"
    exit 0
fi

# Hash changed. The version must have moved with it.
if [ "$version" = "$recorded_version" ]; then
    printf '  The wire headers changed but PROTOCOL_VERSION is still %s.\n\n' "$version"
    printf '  A client built before this change and a server built after it would\n'
    printf '  both announce version %s and then disagree about the bytes. Bump\n' "$version"
    printf '  PROTOCOL_VERSION to %s in BOTH trees:\n\n' "$((version + 1))"
    printf '      mmo_server/common/include/protocol.h\n'
    printf '      mmo_client/common/protocol.h\n\n'
    printf '  then re-run. If the change genuinely cannot affect the wire (a comment,\n'
    printf '  a doc block), re-record instead:\n'
    printf '      rm %s && ./tests/check_protocol_version.sh\n' "$RECORD"
    exit 1
fi

printf '%s %s\n' "$version" "$hash" > "$RECORD"
printf '  wire format changed; PROTOCOL_VERSION %s -> %s, fingerprint re-recorded\n' \
       "$recorded_version" "$version"
exit 0

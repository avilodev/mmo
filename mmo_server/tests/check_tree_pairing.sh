#!/bin/sh
#
# Fail when the server and client trees are not from the same release.
#
# The two trees are independent git repositories with no submodule, no lockfile
# and no tag linking them, and they share three surfaces that must agree: the
# wire format, the world file format, and the content the cross-tree scripts
# check. Nothing recorded which pair was ever meant to run together.
#
# check_protocol_sync.sh compares the two copies of protocol.h -- but only when
# they happen to sit side by side, and it prints SKIPPED and passes when they do
# not. So the failure mode this closes is not "the trees disagree", which that
# script already catches. It is "the trees were never compared at all, and
# nobody noticed": a CI job that checks out one repository, finds no sibling,
# skips every cross-tree check, and goes green.
#
# This refuses to be skipped quietly. A missing client tree is reported as a
# skip only when MMO_CLIENT_ROOT was not set and the default path does not
# exist -- a server-only checkout, which is legitimate -- and is a hard failure
# whenever a client tree was named and then could not be read.
#
# Both trees carry PAIRED_RELEASE. Bump `release` in both, in the same change,
# whenever a shared surface changes.

set -u

cd "$(dirname "$0")/.." || exit 1

SERVER_FILE="PAIRED_RELEASE"
CLIENT_ROOT="${MMO_CLIENT_ROOT:-../mmo_client}"
CLIENT_FILE="$CLIENT_ROOT/PAIRED_RELEASE"

# Read the release value: the first `release = <value>` line, trimmed.
read_release() {
    sed -n 's/^[[:space:]]*release[[:space:]]*=[[:space:]]*\(.*\)$/\1/p' "$1" \
        | sed 's/[[:space:]]*$//' \
        | head -1
}

if [ ! -f "$SERVER_FILE" ]; then
    printf '  missing %s — this tree does not say what release it is\n' "$SERVER_FILE"
    exit 1
fi

SERVER_RELEASE=$(read_release "$SERVER_FILE")
if [ -z "$SERVER_RELEASE" ]; then
    printf '  %s has no "release = ..." line\n' "$SERVER_FILE"
    exit 1
fi

if [ ! -f "$CLIENT_FILE" ]; then
    # Named explicitly and not there: a real problem, not a skip.
    if [ -n "${MMO_CLIENT_ROOT:-}" ]; then
        printf '  MMO_CLIENT_ROOT is set to %s but there is no PAIRED_RELEASE there.\n' \
               "$CLIENT_ROOT"
        printf '  Point it at a client tree, or unset it for a server-only build.\n'
        exit 1
    fi
    printf '  SKIPPED: no client tree at %s (server release %s)\n' \
           "$CLIENT_ROOT" "$SERVER_RELEASE"
    printf '           Set MMO_CLIENT_ROOT to check the pair.\n'
    exit 0
fi

CLIENT_RELEASE=$(read_release "$CLIENT_FILE")
if [ -z "$CLIENT_RELEASE" ]; then
    printf '  %s has no "release = ..." line\n' "$CLIENT_FILE"
    exit 1
fi

if [ "$SERVER_RELEASE" = "$CLIENT_RELEASE" ]; then
    printf '  server and client trees are both release %s\n' "$SERVER_RELEASE"
    exit 0
fi

printf '  The server and client trees are from different releases.\n\n'
printf '    server: %s  (%s)\n' "$SERVER_RELEASE" "$SERVER_FILE"
printf '    client: %s  (%s)\n\n' "$CLIENT_RELEASE" "$CLIENT_FILE"
printf '  These two trees were not released together, so nothing guarantees they\n'
printf '  agree about the wire format, the world file format, or the content the\n'
printf '  other cross-tree checks compare. A mismatch that PROTOCOL_VERSION\n'
printf '  happens to catch is refused at the handshake; one it does not is a\n'
printf '  packet misread in silence.\n\n'
printf '  Check out the matching pair, or -- if this change is deliberate --\n'
printf '  bump "release" in BOTH PAIRED_RELEASE files in the same change.\n'
exit 1

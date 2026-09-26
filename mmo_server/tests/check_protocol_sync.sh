#!/bin/sh
#
# Fail when the two protocol.h copies stop declaring the same wire format.
#
# The client and server trees are independent: each carries its own protocol.h,
# each builds on its own hardware, and neither includes from the other. They are
# kept in sync by hand, and by hand is exactly how an enum value or an opcode
# gets added to one and forgotten in the other.
#
# check_protocol_version.sh catches a change to the format that forgot to bump
# PROTOCOL_VERSION. It cannot catch the trees disagreeing, because it only ever
# reads the server's copy. This reads both.
#
# Comments are stripped before comparing, deliberately: the two files carry
# different amounts of explanation and always have, and requiring them to match
# character for character would mean either duplicating server-side reasoning
# into the client tree or never writing any. What must match is every line that
# the compiler acts on.

set -u

cd "$(dirname "$0")/.." || exit 1

SERVER="common/include/protocol.h"
CLIENT="../mmo_client/common/protocol.h"

if [ ! -f "$SERVER" ]; then
    printf '  missing %s\n' "$SERVER"
    exit 1
fi

if [ ! -f "$CLIENT" ]; then
    printf '  SKIPPED: no client tree at %s\n' "$CLIENT"
    exit 0
fi

# Strip comments the way the compiler sees it, then drop blank lines and
# trailing whitespace so formatting alone cannot fail the build.
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

if [ "$server_decls" = "$client_decls" ]; then
    lines=$(printf '%s\n' "$server_decls" | wc -l | tr -d ' ')
    printf '  both protocol.h trees declare the same %s lines of wire format\n' "$lines"
    exit 0
fi

printf '  The two protocol.h copies no longer declare the same thing.\n\n'
printf '  A packet added, renamed or resized in one tree and not the other is a\n'
printf '  client and a server that agree on PROTOCOL_VERSION and disagree about\n'
printf '  the bytes. Apply the change to both:\n\n'
printf '      %s\n' "$SERVER"
printf '      %s\n\n' "$CLIENT"
printf '  < server   > client\n\n'

tmp_server=$(mktemp) || exit 1
tmp_client=$(mktemp) || { rm -f "$tmp_server"; exit 1; }
printf '%s\n' "$server_decls" > "$tmp_server"
printf '%s\n' "$client_decls" > "$tmp_client"

diff "$tmp_server" "$tmp_client" | sed 's/^/    /' | head -60

rm -f "$tmp_server" "$tmp_client"
exit 1

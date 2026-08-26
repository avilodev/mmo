#!/bin/sh
#
# Fail when a scale constant is defined in more than one place.
#
# These bounds size real arrays in more than one subsystem. A second #define
# does not collide loudly — the two copies agree on the day they are written,
# and the failure only appears later, when one is tuned and the other is not.
# What you get then is a buffer sized 1000 being indexed against a bound of
# 4000, which is a silent overrun rather than a build error.
#
# Single ownership is the fix; this script is what keeps it that way.

set -u

cd "$(dirname "$0")/.." || exit 1

# Constants that must have exactly one definition site in the tree.
# MAX_NPCS is deliberately absent: the NPC pool is heap-allocated and sized
# from each world's .conf, so there is no compile-time ceiling to guard.
# MAX_QUESTS, MAX_DIALOGUES and MAX_PLAYER_QUESTS are absent for the same
# reason -- those registries and a character's quest log all grow to fit, and
# none of them sizes an array any more.
#
# SESSION_EXPIRY_SECONDS does not size an array, and is guarded anyway: it was
# defined in both types.h and session.h, which is the same drift with a
# different consequence -- a session that two subsystems disagree about the
# lifetime of.
GUARDED="MAX_PLAYERS MAX_SESSIONS MAX_ACTIVE_EFFECTS
         NPC_CAPACITY_DEFAULT NPC_CAPACITY_MAX
         SESSION_EXPIRY_SECONDS MAX_PACKET_SIZE"

status=0

for name in $GUARDED; do
    # Match a real definition line only: "#define NAME <value>", not
    # MAX_PLAYERS_FOO and not a mention inside a comment.
    hits=$(grep -rnE "^[[:space:]]*#[[:space:]]*define[[:space:]]+${name}[[:space:]]+" \
                 --include='*.h' --include='*.c' . 2>/dev/null)
    count=$(printf '%s' "$hits" | grep -c . )

    if [ "$count" -eq 0 ]; then
        printf '  %-20s MISSING — guarded constant has no definition\n' "$name"
        status=1
    elif [ "$count" -gt 1 ]; then
        printf '  %-20s DUPLICATED in %d places:\n' "$name" "$count"
        printf '%s\n' "$hits" | sed 's/^/      /'
        status=1
    else
        printf '  %-20s ok (%s)\n' "$name" "$(printf '%s' "$hits" | cut -d: -f1)"
    fi
done

if [ "$status" -ne 0 ]; then
    printf '\nEach constant above must be defined once and included where needed.\n'
    printf 'Do not add an #ifndef guard around the second copy: that hides the\n'
    printf 'divergence instead of preventing it.\n'
    exit 1
fi

printf '  all guarded constants have exactly one definition\n'
exit 0

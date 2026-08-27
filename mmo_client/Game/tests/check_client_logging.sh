#!/bin/sh
#
# Fail when a client source writes diagnostics to a console instead of the log.
#
# A released client has no console. Anything printed to stdout or stderr from
# Game/src goes nowhere a player can send back, so a crash report is worth
# exactly what Game/logs/client.log is worth -- and for a long time that was
# almost nothing: the network layer logged properly through NET_LOG/NET_WARN,
# and everything else, 150 call sites across the state machine, the game loop,
# asset loading and the player, printed to a stream nobody sees.
#
# The rule this enforces is narrow on purpose. It is not "no printf": writing
# to a real FILE* is how settings are saved, and the tests print their own
# results. It is "no printf, and no fprintf to stdout or stderr, under
# Game/src" -- use CLOG_INFO and friends from core/client_log.h, which write
# the file *and* put warnings and errors on the console for a developer running
# from a terminal.
#
# One exemption, listed below rather than pattern-matched, because the reason
# is specific: client_log.c reports a log file it could not open, and it cannot
# report that through itself.

set -u

cd "$(dirname "$0")/../.." || exit 1

SRC="Game/src"

# path:reason
EXEMPT="Game/src/core/client_log.c"

if [ ! -d "$SRC" ]; then
    printf '  missing %s\n' "$SRC"
    exit 1
fi

found=0

for file in $(find "$SRC" -name '*.c' | sort); do
    skip=0
    for e in $EXEMPT; do
        [ "$file" = "$e" ] && skip=1
    done
    [ "$skip" = 1 ] && continue

    # Bare printf(, or fprintf( whose stream is stdout or stderr. snprintf and
    # vsnprintf are excluded by the word boundary; fprintf to any other stream
    # is a file being written, not a diagnostic.
    hits=$(grep -nE '(^|[^a-zA-Z0-9_])printf[[:space:]]*\(|fprintf[[:space:]]*\([[:space:]]*(stdout|stderr)[[:space:]]*,' "$file" \
           | grep -vE '(^|[^a-zA-Z0-9_])(sn|vsn|vf|vs)printf')

    if [ -n "$hits" ]; then
        if [ "$found" = 0 ]; then
            printf '  client diagnostics must go through CLOG_* (core/client_log.h),\n'
            printf '  not to a console a released client does not have:\n\n'
        fi
        found=1
        printf '%s\n' "$hits" | while IFS= read -r line; do
            printf '    %s:%s\n' "$file" "$line"
        done
    fi
done

if [ "$found" = 1 ]; then
    printf '\n  Replace with CLOG_ERROR / CLOG_WARN / CLOG_INFO / CLOG_DEBUG.\n'
    printf '  Warnings and errors still reach stderr; the file keeps everything.\n'
    exit 1
fi

printf '  every client source logs through client_log.h\n'
exit 0

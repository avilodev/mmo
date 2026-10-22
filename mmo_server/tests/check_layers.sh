#!/bin/sh
#
# Fail when a source file includes a header from a layer it may not use.
#
# The server is built in five layers, and each folder says which one a file is in:
#
#   rank 4  network   network/, login_server/, realm_server/, world_server/network/
#   rank 3  systems   world_server/systems/, services/
#   rank 2  world     world_server/world/
#   rank 1  storage   storage/
#   rank 0  base      common/, world_server/base/   (usable from every layer)
#
# A file may include its own layer, the layer directly below it, and base.
# services/ is the exception: it sits between the login and realm network code
# and storage, and there is no world layer in those two servers, so it may also
# include storage directly.
#
# Files that start the process (main.c and config.c in world_server/, and every
# main.c) wire the layers together and are not checked.
#
# Only the storage layer may include a database library, so no other layer can
# talk to PostgreSQL, SQLite or Redis behind the storage code's back.
#
# Tests are not checked: they link and stub across layers on purpose.

set -u

cd "$(dirname "$0")/.." || exit 1

# print the rank of a path: 0-4, 3s for services, 9 for files that wire things
rank_of() {
    case "$1" in
        */main.c|main.c|world_server/config.c) echo 9 ;;
        common/*|world_server/base/*) echo 0 ;;
        storage/*) echo 1 ;;
        world_server/world/*) echo 2 ;;
        world_server/systems/*) echo 3 ;;
        services/*) echo 3s ;;
        network/*|world_server/network/*|login_server/*|realm_server/*) echo 4 ;;
        *) echo x ;;
    esac
}

HEADERS=$(find common network storage services world_server login_server realm_server \
    -path '*/include/*.h' -type f 2>/dev/null | sort)
SOURCES=$(find common network storage services world_server login_server realm_server \
    \( -path '*/src/*.c' -o -path '*/include/*.h' -o -name main.c -o -name config.c \) \
    -type f 2>/dev/null | sort)

fail=0
checked=0

for file in $SOURCES; do
    from=$(rank_of "$file")
    [ "$from" = "x" ] && continue

    # which server tree the file is in, so same-named headers resolve to its own
    tree=${file%%/*}

    # the quoted includes of this file
    for name in $(sed -n 's/^#include "\([^"]*\)".*/\1/p' "$file"); do
        base=${name##*/}

        # resolve the header: its own tree first, then the shared folders
        target=""
        for candidate in $(printf '%s\n' "$HEADERS" | grep "/include/$base\$"); do
            case "$candidate" in "$tree"/*) target=$candidate; break ;; esac
        done
        if [ -z "$target" ]; then
            for candidate in $(printf '%s\n' "$HEADERS" | grep "/include/$base\$"); do
                case "$candidate" in
                    common/*|network/*|storage/*|services/*) target=$candidate; break ;;
                esac
            done
        fi
        [ -z "$target" ] && continue

        to=$(rank_of "$target")
        checked=$((checked + 1))

        # files that wire the process may use anything; base is open to all
        [ "$from" = "9" ] && continue
        [ "$to" = "0" ] && continue
        [ "$to" = "9" ] && continue

        allowed=0
        case "$from" in
            3s) [ "$to" = "1" ] || [ "$to" = "2" ] || [ "$to" = "3" ] || [ "$to" = "3s" ] && allowed=1 ;;
            *)
                [ "$to" = "$from" ] && allowed=1
                [ "$to" = "3s" ] && [ "$from" = "4" ] && allowed=1
                [ "$to" = "3s" ] && [ "$from" = "3" ] && allowed=1
                [ "$from" != "3s" ] && [ "$to" != "3s" ] && [ $((to + 1)) -eq "$from" ] && allowed=1
                ;;
        esac

        if [ "$allowed" -eq 0 ]; then
            printf '  FAIL  %s (rank %s) includes %s (rank %s)\n' "$file" "$from" "$target" "$to"
            fail=1
        fi
    done
done

# database libraries belong to the storage layer alone
for file in $SOURCES; do
    case "$file" in storage/*) continue ;; esac
    hit=$(grep -nE '^#include <(libpq-fe|sqlite3|hiredis/hiredis)\.h>' "$file" 2>/dev/null)
    if [ -n "$hit" ]; then
        printf '  FAIL  %s includes a database library outside storage/: %s\n' "$file" "$hit"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    printf '\nlayering broken: a file includes a layer it may not use\n'
    exit 1
fi

printf '  %s includes checked, every one stays inside its layer\n' "$checked"
exit 0

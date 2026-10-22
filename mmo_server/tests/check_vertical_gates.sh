#!/bin/sh
#
# Fail when a gameplay file loses (or gains) a world_vertical_ok( gate.
#
# Modules with no test harness (combat) keep their height checks pinned here.
# Each row is "file expected_count [pattern]". Add a row in the same change as
# its gates. The pattern is a fixed string, world_vertical_ok( when omitted.

set -u

cd "$(dirname "$0")/.." || exit 1

TABLE="
world_server/systems/src/combat.c 3
world_server/systems/src/ability_handler.c 4
world_server/systems/src/zone_owner.c 2
world_server/systems/src/npc_ai_flush.c 2
world_server/systems/src/npc_behavior.c 2
world_server/systems/src/npc_triggers.c 1
world_server/systems/src/loot.c 1
world_server/systems/src/dialogue_handler.c 2
world_server/systems/src/shop_trade.c 1
world_server/systems/src/projectile.c 2 proj_overlaps_body(proj->
"

fail=0

while read -r file expected pattern; do
    [ -z "$file" ] && continue
    if [ ! -f "$file" ]; then
        printf '  missing %s\n' "$file"
        fail=1
        continue
    fi
    [ -z "$pattern" ] && pattern='world_vertical_ok('
    actual=$(grep -oF -e "$pattern" "$file" | wc -l | tr -d ' ')
    if [ "$actual" -ne "$expected" ]; then
        printf '  %s: expected %s %s gates, found %s\n' \
            "$file" "$expected" "$pattern" "$actual"
        fail=1
    fi
done <<EOT
$TABLE
EOT

if [ "$fail" -ne 0 ]; then
    printf '\n  Update the gates or the table in tests/check_vertical_gates.sh.\n'
    exit 1
fi

printf '  every listed file has its expected vertical gates\n'
exit 0

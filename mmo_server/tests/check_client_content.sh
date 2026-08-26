#!/bin/sh
#
# Fail when the client has no name for an NPC the server spawns.
#
# npc_types.json is the client's, not the server's: the world spawns NPCs from
# spawns.json and puts a one-byte type id on the wire, and the client turns that
# id into a name and a sprite. A type the client has never heard of renders as a
# nameless thing standing in the courtyard -- and if it is a quest target, the
# overhead badge has no name to draw either.
#
# opening_sequence_test cannot see this, because it links the server and the
# server's copy of npc_types.json is empty by design. Only something that reads
# across both trees can, which is what this does.

set -u

cd "$(dirname "$0")/.." || exit 1

SPAWNS="world_server/data/spawns.json"
CLIENT_TYPES="../mmo_client/Game/data/npc_types.json"

if [ ! -f "$SPAWNS" ]; then
    printf '  missing %s\n' "$SPAWNS"
    exit 1
fi

if [ ! -f "$CLIENT_TYPES" ]; then
    printf '  SKIPPED: no client tree at %s\n' "$CLIENT_TYPES"
    exit 0
fi

python3 - "$SPAWNS" "$CLIENT_TYPES" <<'PY'
import json, sys

spawns_path, types_path = sys.argv[1], sys.argv[2]

try:
    spawns = json.load(open(spawns_path))["spawns"]
    known = json.load(open(types_path))["npc_types"]
except (OSError, ValueError, KeyError) as err:
    print(f"  could not read the content files: {err}")
    sys.exit(1)

named = {}
for entry in known:
    type_id = entry.get("id")
    if type_id is None:
        print(f"  an npc_types.json entry has no \"id\": {entry}")
        sys.exit(1)
    if type_id in named:
        print(f"  npc_types.json defines type {type_id} twice "
              f"('{named[type_id]}' and '{entry.get('name')}')")
        sys.exit(1)
    named[type_id] = entry.get("name", "")

missing = {}
for spawn in spawns:
    type_id = spawn.get("npc_type_id")
    if type_id is None:
        print(f"  a spawn has no \"npc_type_id\": {spawn.get('name', spawn)}")
        sys.exit(1)
    if type_id not in named:
        missing.setdefault(type_id, spawn.get("name", "?"))

if missing:
    print("  The client has no name for an NPC type the world spawns.\n")
    print("  Add each of these to ../mmo_client/Game/data/npc_types.json:\n")
    for type_id, name in sorted(missing.items()):
        print(f'      {{ "id": {type_id}, "name": "{name}" }}')
    print("\n  (spawns.json is the server's; npc_types.json is the client's.)")
    sys.exit(1)

spawned = {s.get("npc_type_id") for s in spawns}
unused = sorted(t for t in named if t not in spawned)

print(f"  the client names all {len(spawned)} NPC types the world spawns")
if unused:
    listed = ", ".join(str(t) for t in unused)
    print(f"  (it also names {len(unused)} nothing spawns yet: {listed})")
PY

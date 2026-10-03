# 3D movement: walkable surfaces with height

Status: design, awaiting review. Written 2026-10-03. Supersedes D4 of the
deleted `Next_steps/3d_refactor.md` ("gameplay never gains height"; see git
history). The game is a 3D game: the player walks up ramps and stairs, and
walkable surfaces stack (bridges, upper floors, balconies).

## Understanding

- **Outcome:** characters walk up and down ramps, stairs and slopes in
  `ennara_city.blend`, and can walk over and under stacked surfaces, on client
  and server, with the server still authoritative.
- **Not asked for:** jumping, falling mechanics, 3D combat. They will be added
  later, so nothing here may make them harder.
- **Assumed:** `ennara_city.blend` already contains the ramps and stairs
  (objects named `stairs_stone_*`, `stairs_grand_*`, `BankStairs`, `slope_*`,
  `ramp`). Which collection each is in has not been confirmed, because Blender
  is not installed on the dev box.

## Approach

Spans per tile (a Recast-style heightfield). Each (x, y) tile holds a list of
spans. A span is a walkable floor height and the clear height above it.
Stacked surfaces are several spans in one tile. Chosen over server-side
triangle collision (12.8M triangles, too costly per move) and a navmesh (a
large new subsystem, awkward for free movement).

## Data: the bake

`Game/data/export_city_collision.py` emits spans instead of solid/open tiles.

- A face tilted no more than `WALK_MAX_SLOPE_DEG` (45) from horizontal is
  floor. Ramps, stairs and hills are floor.
- Floor with at least `BODY_HEIGHT` (40 units, 1.83 m) of clearance above it
  becomes a span. Anything less is not walkable.
- Steeper faces and anything inside a span's body height are walls: the
  tile's blocked flag for that span.
- Door sills, lintels and `MIN_WALL_SUBCELLS` logic are kept for walls.
- Output: `city_collision.bin` version 2 holds the spans (row run-lengths of
  span lists). `city_heights.bin` is derived from the tallest blocking height,
  for the camera as today. The Blender run is manual; the script is written
  here and the user runs it.

`worldgen_city_collision.c` and `worldgen_write.c` read the new layout and
write it into `world.dat`. Loading refuses a version-1 file.

## Server

- `world_collision` gains a span query: `world_spans_at(x, y, out[], max)` and
  `world_floor_reachable(x, y, from_z, &out_z)`, which returns the span
  reachable from `from_z` (floor within `STEP_UP` of it, or at most `DROP_MAX`
  below it) with headroom. It fails closed outside the world.
- `world_collision_check_box` and `_path` become span-aware: a sample is
  blocked when no reachable span exists, or a wall blocks it at that height.
- `move_validate` takes `from_z` and `to_z`. It rejects with the new
  `MOVE_REJECT_HEIGHT` when `to_z` is not a reachable span's floor (within a
  small tolerance). Speed is measured in 3D distance.
- NPC movement uses the same queries, keeping its own `z`.
- Anti-cheat: the server recomputes the floor from the span; a client's `z` is
  only accepted if it matches one.

## Protocol

Every position-carrying packet and struct in `protocol.h` gains a `pos_z`
float, on both copies (`mmo_server/common/include/protocol.h`,
`mmo_client/common/protocol.h`): `PlayerMovePacket` (plus `vel_z`),
`PlayerPositionPacket`, `NearbyPlayerData`, `NPCPositionData`, the move ack,
and the other position structs listed there. The wire version is bumped; older
clients are refused at login. `server_types.h`, `broadcast_snapshot.h`,
`combat_config.h` and `ability_handler.h` carry `pos_z` but combat ignores it
for now.

## Client

- `player.c` and `locomotion.c` carry `z`, using the same span query as the
  server (the world file is shared). Walking onto a higher span eases `z`
  over a short time rather than snapping; ramps follow the surface.
- `character_renderer.c` places bodies at `(x, z, y)` instead of `(x, 0, y)`,
  and drops shadows on the surface below. Name plates, health bars and cards
  follow the body.
- The camera focus uses `z + 24`; wall clearance reads the new heights.
- Click picking uses the 3D camera ray against the player's span at the
  character's height.

## Out of scope (hooks left)

- Jumping and falling: spans plus `DROP_MAX` and `vel_z` on the wire already
  carry what it needs.
- Combat range, telegraphs, projectiles and NPC AI ignore `z` for now.

## Testing

Written first, in the style of `world_collision_test`, `move_validator_test`,
`worldgen_city_collision_test` and `city_heights_test`:

- Span lookup: flat ground, ramp, stairs, bridge over ground (two spans).
- Reachability: step too tall rejected, in-limit step accepted, no headroom
  rejected, drop within limit accepted, outside the world fails closed.
- `move_validate`: bad `z` rejected, 3D speed budget, path over a ramp.
- Bake round trip: a small synthetic span file loads and queries correctly.
- Spawn, every NPC and every quest NPC's path remain open and reachable.

Not verifiable here: the Blender bake and the GL client (no Blender, no
`gl.h`). The user runs the bake and plays the result.

## Risks

- Stair and slope objects may not be in the `COLLISION` collection, in which
  case they will not bake. The first task is a short Blender script that lists
  what is in the collection.
- A 0.73 m tile may be too coarse for thin stairs; the span bake samples
  finer and reduces, as the doorway logic does now.
- Protocol change touches roughly ten structs on both sides in one step.

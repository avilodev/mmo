# 3D Refactor — Phase 1: World & Camera, and the start of Phase 2

**Goal:** the world is viewed through a tilted, player-rotatable 3D camera
(League-style angle, but the player can spin it). The client *draws* in 3D;
the server keeps simulating a flat 2D game and does not change in this phase.

Status: **Phase 1 steps 0–7 implemented 2026-09-26.** **Phase 2 started the same
day** (§11): 3D characters, a 3D world grown from the collision map, the
authored courtyard, and the 2D path and art removed. Awaiting a play-test on
Windows hardware (§8) · Written 2026-09-26

---

## 1. Direction (already settled)

- **Stay in C, OpenGL, GLFW.** No engine switch. Modernize only the render layer.
- **3D visuals, 2D gameplay.** The server's `(x, y)` stays the truth for
  collision, movement validation, combat, projectiles, telegraphs and interest.
  Height is visual only.
- **3D characters later, not sprites long-term.** A rotating camera makes
  directional sprites scale badly (angles × animations × races × gear). Phase 1
  uses camera-facing sprites only as placeholders.
- **The refactor is the render layer, not the codebase.** Raw GL calls
  live almost entirely in `render/renderer.c` (49), `world/world.c` (26) and
  `texture/texture.c` (10). Networking, UI, states and combat logic stay.

## 2. Design decisions

Decided here so implementation doesn't stall. Each can be revisited, but a
change should be made on purpose and recorded in this doc.

### Coordinates & scale
| # | Decision | Why |
|---|---|---|
| D1 | Server/world `(x, y)` maps to GL `(x, 0, y)`. **Y is up.** | glTF (Blender export, Phase 2 models) is Y-up; no axis conversion on every model later. |
| D2 | 3D units = existing world units. 1 tile = 16 units (`game.c:111`). Ground is `y = 0`. | No conversion between server and render space. |
| D3 | A character stands ~2.5 tiles (40 units) tall. | Current sprite footprint is 2 tiles wide (`player.c`); slightly taller reads as a figure, not a box. |
| D4 | Gameplay never gains height: no walkable surface over another walkable surface in the open world. Interiors and multi-floor places are separate zones. | Keeps server collision, move validation and anti-cheat 2D. This is the rule that prevents a rewrite later. |

### Camera
| # | Decision | Why |
|---|---|---|
| D5 | **Perspective** projection, narrow vertical FOV ≈ 40° (tunable constant). | Reads as 3D and gives depth when rotating; narrow FOV keeps League-like low distortion. Ortho looks flat when spun. |
| D6 | *Superseded by D39 (free pitch, 55° default).* **Pitch fixed at 55°** in Phase 1. | Consistent look, and players can't tilt toward the horizon to see across the map (fairness + bounded draw distance). |
| D7 | **Yaw is free and continuous** (full 360°). Default yaw = 0 = north-up, identical framing to the old 2D view. | That's the feature. North-up default keeps the map and old habits aligned. |
| D8 | Rotate with **Q/E** (held, fixed deg/sec) and **middle-mouse drag**. Both rebindable via `keybinds.cfg`. | Q/E are unbound today (`core/keybinds.c:79-95`). Middle mouse doesn't conflict with UI clicks. |
| D9 | **Scroll = camera distance**, clamped min/max. Replaces the current `camera.zoom` 1×–2× (`main.c:48`). *Revised 2026-09-26 after the first look:* range 1.05–3.5, default/Home 1.6 (was 1.05–2.0, default 1.05). | Same feel as today, expressed in 3D. |
| D10 | Camera **stays where the player left it**. No auto spring-back. A **reset key (Home)** returns to yaw 0 / default distance. | Spring-back fights the player mid-fight. Reset covers "I'm lost". |
| D11 | Camera is **locked to the player** (smooth follow, as today). Detached camera is a class feature, out of scope (§5). | Matches the design: you can't look away unless your class allows it. |

### World rendering
| # | Decision | Why |
|---|---|---|
| D12 | The existing 4 tile layers stay flat at `y = 0`. Draw order base → floor → interior with **depth writes off**, then entities with depth on. | Same-height layers would flicker against each other (z-fight). Fixed draw order is how they work today. |
| D13 | **"Above" layer (roofs):** Phase 1 fades roof tiles within a radius of the player and draws the rest flat. Real 3D buildings with see-through walls are Phase 2. | Cheap, keeps players visible indoors. Flat roofs are a known placeholder. |
| D14 | Chunk culling uses the **camera's ground footprint** (frustum corners intersected with `y = 0`, then a bounding box), replacing `visible_chunk_range`'s screen rectangle. | A rotated, tilted view isn't a screen-aligned rectangle; the old math drops chunks at the edges. |
| D15 | **Max camera distance is clamped so the footprint fits the streamed area.** *As built:* at 1920×1080 the far corners at zoom 1.0 reach ~1550 units at 45° yaw, far past radius 2 (1024), so `LOAD_RADIUS_CHUNKS` is **3** (49 resident chunks of 64) and `CAMERA_ZOOM_MIN` is **1.05** to fit inside 3. Pinned by `camera_math_test` TEST 6. | The far edge of a tilted view extends past the top-down rectangle. Radius 2 would have needed zoom ≥ 1.5, which removed too much of the zoom range. |
| D16 | *(Built; inactive until textured tilesets ship, because today's world is flat-colour.)* **Keep pixel art crisp:** magnify `GL_NEAREST`, minify with mipmaps, anisotropic filtering 8× where supported. Pad tileset atlases with 1–2px extruded gutters at load time so mipmaps don't bleed neighbouring tiles. | Tiles at an angle shimmer without mipmaps; atlases bleed seams with them unless padded. Today everything is `GL_NEAREST` with no mips (`texture.c:47`). |
| D17 | World overview map stays **top-down, north-up**, and shows a **view cone** for the camera's yaw. | A rotating minimap is disorienting; a cone tells you which way you're looking. |

### Entities & overlays
| # | Decision | Why |
|---|---|---|
| D18 | *Superseded by D30 (Phase 2):* players and NPCs are 3D models; projectiles and damage numbers stay cards. *Was:* players, NPCs, projectiles and damage numbers become **screen-aligned cards**: each entity's existing 2D drawing code runs unchanged inside `camera_billboard_begin/end`, placed at its projected feet and scaled by perspective. Placeholder until Phase 2 models. | A true vertical (cylindrical) card seen from 55° above is squashed to ~57% height; screen-aligned cards keep sprite art at full proportion, and reuse every existing draw call with no rewrite. |
| D19 | Each card is depth-tested as one flat card at its feet's depth, with **alpha test** so transparent texels don't hide what's behind. | No back-to-front sorting; nearer entities cover farther ones regardless of draw order. |
| D20 | **Telegraphs (circles/cones)** stay ground-drawn at `y = 0` with polygon offset. | They're already world-space; they land on the floor for free. |
| D21 | *Revised:* **health bars, names, damage numbers ride on the entity's card.** *Phase 2:* the card is anchored at the top of the 3D body's head (`camera_billboard_begin(x, y, 0, height)`), not its feet. | Same result as a separate screen-space pass (upright, readable at any yaw) with no second code path; they scale with zoom exactly as they did top-down. |

### Input & movement
| # | Decision | Why |
|---|---|---|
| D22 | **WASD is camera-relative:** rotate the input vector by camera yaw before the existing collision/move code (`player.c:74`). Existing diagonal normalization stays. | W must mean "away from camera" once the camera spins. The server sees the same position updates. |
| D23 | Character **facing = last movement direction** (League-style "turn by moving"). | Needed for Phase 2 models; costs nothing now. |
| D24 | **No click-to-move in Phase 1.** It needs pathfinding (none exists client or server). Separate doc. | Keep this phase to camera and world. |
| D25 | *Corrected:* abilities were **already cursor-aimed**, and click-targeting already used the mouse, through three inline top-down formulas in `state_playing_input.c`. All three now use the camera: ability aim and item pickup use **ground picking**; clicking players/NPCs hits their **standing card** (`camera_hit_billboard`). No protocol change. | The old formulas would have aimed at the wrong place the moment the camera tilted. |

### Engineering
| # | Decision | Why |
|---|---|---|
| D26 | *As built:* **cglm 0.9.4** vendored at `Game/include/cglm` (MIT, header-only, always included via `camera/cglm_config.h` with `CGLM_ALL_UNALIGNED`). Instead of glad, a **30-function hand-written loader** (`render/gl_loader.c`) over `glfwGetProcAddress`. The game explicitly requests a **3.3 compatibility** context. | Asking for plain 3.3 returns a *core* context on Mesa, where every legacy `glBegin` silently fails. Compatibility lets old and new GL coexist. A generated loader for 30 functions wasn't worth vendoring. |
| D27 | Camera math lives in a **GL-free module** (`camera/camera_math.c`) so it's unit-testable in `Game/tests` without a context. | Picking, footprint and yaw-rotation bugs are silent visual bugs; tests catch them. |
| D28 | *Done 2026-09-26:* the owner moved the game to 3D outright, so the top-down path, F9, `camera_view_build_2d` and the roof split/fade are **deleted**. *Was:* **F9 toggles 3D ↔ top-down.** *Revised:* the top-down path is kept **until the 3D view has been play-tested on Windows hardware**, then deleted. It costs little: both views share one `CameraView`, and the top-down roof split is a shader uniform. | The change was verified on Linux with software GL only. A working fallback on the real machine is worth one more cycle. |
| D29 | Perf budget: **60 fps at max camera distance** in the densest area of Ennara, **on integrated graphics** (owner answer, §6). Measured, not assumed. *Not yet measured:* the only GL available during development was a software renderer. | Tilted views draw more chunks than top-down; catch it early. |

## 3. Steps

Each step lists what "done" means. Don't start the next until done holds.

**Step 0 — Setup**
- Vendor cglm. Add `camera_math.c` + test target. Add the F9 toggle (D28).
- Done: builds clean with the strict flags; F9 flips a flag; test target runs.

**Step 1 — The camera**
- Camera gains yaw, fixed pitch (D6), distance (D9); builds view + projection
  with cglm and loads them with `glLoadMatrixf` in `camera_apply()`.
- Q/E + middle- or right-drag rotate (D8), scroll distance, Home reset (D10), smooth follow kept.
- Done: in 3D mode the real map shows tilted and rotates smoothly around the player; F9 returns to identical old behaviour.

**Step 2 — Floor draws correctly**
- Layer order and depth rules (D12), roof fade (D13), footprint culling (D14),
  distance clamp vs streaming (D15), filtering and atlas gutters (D16).
- Done: no flicker between layers, no missing chunks at any yaw or distance, no visible tile seams, no shimmer while walking.

**Step 3 — Mouse → world**
- `camera_screen_to_world` = ray/ground intersection; `camera_world_to_screen` = full projection.
- Tests: round-trip screen→world→screen within 0.5px at yaw 0/45/90/180/270 and min/max distance; footprint contains every tile that projects on screen.
- Done: tests pass; a debug marker drawn at the picked point sits under the cursor at any angle.

**Step 4 — Stand everything up**
- Billboards for players/NPCs/projectiles (D18, D19); telegraphs on the ground (D20);
  bars/names/damage numbers projected to screen (D21).
- Done: all entities upright at every yaw, labels readable, telegraphs line up with the hits the server reports.

**Step 5 — Camera-relative movement**
- Rotate WASD by yaw (D22); track facing (D23).
- Test: input "W" at yaw θ produces a unit vector pointing away from the camera.
- Done: W always moves "into the screen"; no increase in server move rejections (check `move_validator` logs).

**Step 6 — Minimap and polish**
- View cone on the world overview (D17). Tune FOV, pitch, distance limits, rotation speed.
- Done: perf budget (D29) met; tuning constants in one header.

**Step 7 — Modernize the renderer underneath** *(done, with two revisions)*
- GL 3.3 loader (D26). Chunk display lists → VBOs drawn by a GLSL 330 ground
  shader, which also does the roof fade and the top-down roof split as uniforms.
- *Revised:* the sprite batcher is deferred to Phase 2 (§7), and the top-down
  path is kept until the Windows play-test (D28, §8).
- Done: all tests pass; full tree builds `-Werror` (apart from the pre-existing
  friends-panel warning); changed files compile under MinGW `-Werror`; offscreen
  renders of real `world.dat` match expectations.

## 4. Server impact

**None in Phase 1.** Positions, move validation, collision, combat and
interest are untouched. If the server log shows more `MOVE_REJECT_*` verdicts
after step 5, it's a client bug.

## 5. Out of scope (later phases, own docs)

- **Phase 2:** static 3D models (cgltf), 3D buildings replacing the roof layer,
  see-through/fading walls, Blender → world pipeline that also generates the
  server collision grid from the same source.
- **Phase 3:** skeletal animation; 4 shared skeletons (humanoid for all Human
  Forms, quadruped, bird, snake); Mixamo / CC0 packs as a starting point.
- **Detached-camera class:** server sends updates around a validated camera
  point with a server-enforced max offset (`interest_collect_fds` is
  player-centred today).
- **Click-to-move + pathfinding**, **cursor-aimed abilities.**

## 6. Owner answers (2026-09-26)

1. **Hardware:** both discrete and **integrated** GPUs must run it. Models will
   stay simple. OpenGL 3.3 is the floor; the game says so and exits cleanly
   if the driver lacks it.
2. **Extended view:** the **flying forms** (Crow, Hawk) get an extended
   viewpoint **while in Animal Form and flying**. Later phase: a larger max
   camera distance for that state, plus a server-side interest radius around
   a server-validated camera point (`interest_collect_fds` is player-centred
   today). Needs the flying state to exist first.
3. **Pitch stays fixed** (D6), permanently.

## 7. Implementation notes (2026-09-26)

What exists now, file by file:

| Piece | Where |
|---|---|
| Camera math (pure, tested) | `Game/src/camera/camera_math.c`, `camera_math.h`, `camera_tuning.h` (every tuning number) |
| Camera state, GL load, billboards, picking | `Game/src/camera/camera.c` |
| GL 3.3 loader | `Game/src/render/gl_loader.c` |
| Ground shader + chunk VBOs | `Game/src/render/ground_renderer.c`, `Game/src/world/chunk_mesh.c` (pure, tested) |
| Tileset gutters + mipmaps + anisotropy | `Game/src/texture/texture_atlas.c` (pure, tested), `texture_load_tileset()` |
| Controls | Q/E rotate, middle- or right-drag rotate (yaw only), scroll zoom, Home reset; rebindable in `keybinds.cfg` (`camera_rotate_left/right`, `camera_reset`). F9 was removed with the top-down view (D28). |
| Tests | `camera_math_test`, `chunk_mesh_test`, `texture_atlas_test` (in `make test`) |

**Deliberately not done in step 7:** a sprite batcher for UI/entity quads.
Everything above the ground still draws in immediate mode through the
unchanged `renderer_*` API. That is a few hundred quads a frame, and a batcher
touches every UI screen, which can't be exercised without a running server. It
belongs with Phase 2, when entities become models and the entity path is
rewritten anyway.

**Found along the way:**
- The shipped `world.dat` is a **flat-colour world** (zero tilesets), so D16's
  gutters and mipmaps are built and tested but inactive until textured
  tilesets ship.
- `player_reconcile_test` had not built since `render_stubs.c` gained
  `renderer_draw_text_centered`, so `make test` was failing at HEAD. The
  duplicate stub was removed.
- A pre-existing `-Wformat-truncation` in `ui/friends_panel_render.c:160`
  still fails `make strict`. Untouched.
- Code review caught that chunk eviction could dead-end once one frame touched
  more than 64 chunks, which radius 3 makes likely right after a respawn,
  leaving holes in the ground. Eviction now always falls back to the
  least-recently-used chunk. The cache wasn't enlarged: `GameState` (670 KB)
  lives on `main`'s stack.
- Temporary tile modifications that expire don't mark their chunk dirty, so
  the expired tile keeps drawing until something else rebuilds the chunk.
  Pre-existing. Untouched.

## 8. Play-test checklist

Play-test on the Windows machine, preferably also on integrated graphics.
Regenerate the world first (`make world` in `mmo_client`, then rebuild or
`make data` in `mmo_server` and restart the world server) -- the courtyard (§11)
lives in world.dat, and client and server must load the same file.

- [ ] Game starts. The client log shows `[GL] ... Compatibility Profile`, `[GROUND] Shader ready`, `[MODEL] Shader ready` and `[CHARACTER] Model ready`.
- [ ] You wake up on the rift in the courtyard. The Warden, the nine leaders and the Gate Sentry stand where they did, as 3D figures, and turn to face you as you walk up; the one you are talking to plays the talking clip.
- [ ] You cannot walk through a lamp, column, tree, the courtyard wall or a building -- and nothing stops you where nothing is drawn.
- [ ] Characters walk and run with their feet roughly matching the ground, turn the way they move, and stand idle when they stop.
- [ ] Q/E, middle-drag and right-drag rotate smoothly (right-drag: sideways only; right-click on an NPC still targets it); drag right turns the view right, drag up looks up; tilt runs -39°–85° and the camera never goes under the floor; Home resets; scroll zooms 1.05–20 (over the shoulder at 20); starts (and Home returns) at 1.6 and 55°.
- [ ] W always walks "into the screen" at any rotation; no rise in server `MOVE_REJECT_*` in the world server log.
- [ ] Right-click an NPC and left-click a player at several rotations: the thing under the cursor is what gets targeted.
- [ ] Abilities land where the cursor points at several rotations.
- [ ] No blank ground at the screen edges at max zoom-out while walking.
- [ ] Minimap cone points the way the camera faces.
- [ ] 60 fps at max zoom-out in Ennara (D29), with the courtyard's NPCs in view.

## 9. Follow-ups outside Phase 1

- **Server view radii vs the tilted view.** The far edge of the screen is up to
  ~1500 units from the player, but the world server sends other players within
  `PLAYER_VIEW_RADIUS` 800 (`world_server/src/main.c:899`), events within
  `INTEREST_EVENT_RADIUS` 800 and telegraphs within 500
  (`npc_ai_flush.c:42`). NPCs (2000) are fine. Players and telegraphs in the
  far part of the screen pop in late. This was already true sideways in the
  old view (±960), just smaller. Raise the radii to ~1600, weighing the
  bandwidth.

## 10. Risks

| Risk | Mitigation |
|---|---|
| Existing pixel art looks muddy or shimmery at an angle | D16; judge at step 2 before investing further. Art direction can change in Phase 2. |
| Tilted view shows un-streamed (blank) chunks at the far edge | D15 clamp; test at max distance at every 45° of yaw. |
| Two render paths drift during development | Resolved: the 2D path is deleted (D28). |
| Client and server disagree on walls once the world is 3D-authored (Phase 2) | Generate server collision from the same source file; never hand-maintain both. |

## 11. Phase 2, started 2026-09-26: 3D characters and a small 3D world

The owner's call: move the game to 3D outright, take the character model from
`opengl_3d/models/characters`, and build a *small* 3D world with the NPCs whose
dialogue already exists -- keeping coordinates consistent with the server.

### Decisions

| # | Decision | Why |
|---|---|---|
| D30 | **Every player and NPC wears one rigged model**: Quaternius `UAL1_Standard.glb` (CC0; 8.5k vertices, 65 joints, 43 clips), tinted by race colour (players) or NPC display style (NPCs), scaled by the NPC's size. | The owner's model. One rig serves every form until per-race models exist (Phase 3's four skeletons). |
| D31 | **The loader, posing and skinning are ported from `opengl_3d`** (`Game/src/model/`), GL-free and tested; a small GLSL 330 shader draws them (`render/model_renderer.c`). Skinning stays on the CPU. | Already written and proven against this exact rig. ~0.2 ms per body; bodies off screen skip it, bodies beyond 700 units re-skin at 15 Hz. |
| D32 | **Animation is inferred from the drawn position** (`render/character_motion.c`): idle / walk / run by smoothed speed, playback rate scaled to speed, facing = direction of travel (D23), 0.2 s cross-fades. Standing quest NPCs turn to face a nearby player and play the talk clip while in dialogue. | The server sends positions, not animation states; no protocol change. |
| D33 | **The 3D world is grown from world.dat's collision layer** (`world/structure_mesh.c`): solid building tiles get walls and a hip roof, walls/towers become blocks, mountain rock rises, courtyard props (column, lamp, tree) are one model per solid footprint. Nothing is raised on a walkable tile. | Client and server then *cannot* disagree about walls: both read the same tiles (the risk in §10). |
| D34 | **The small world is the Ennara courtyard, authored in the generator** (`Game/data/src/worldgen_courtyard.c`): rift scar on the spawn, lawns and paths, a walkway on the leaders' ring, 6 lamps, 16 columns, 12 trees, and a low wall with gate towers where the roads leave. | "Coordinates matter": the layout is written into the same world.dat the server validates movement against, so every solid thing is solid on both ends. |
| D35 | **Nothing solid is placed on an NPC or between the spawn and a leader.** Lamps stand midway between leaders and 20°+ off every road; columns stand outside the ring. `worldgen_courtyard_test` reads the server's `spawns.json` and fails if any NPC stands on a solid tile or any walk from the spawn to a dialogue NPC is blocked for a player-sized box. | The spawns are fixed coordinates in the server tree; the map has to fit them, not the reverse. |
| D36 | **The courtyard wall stops short of its corners, and no buildings stand within 116 tiles of the centre.** gen_opening.py puts the diagonal hostile camps on the courtyard's corners, and one camp member (Fox-bonded Fanatic) had been spawning *inside a building* in the old map. | Found by D35's test. Fixing the map kept every NPC coordinate unchanged. |
| D37 | **The 2D art is deleted**: player sprite sheet and paperdoll layers (and `paperdoll.c`), world tilesets, decoration sprites, and the unused LDtk map pipeline (`Game/data/map`). Kept: ability icons, fonts, UI panels, the menu background, and the tileset *code* (ground textures may return). | Nothing references them in a 3D client. |
| D38 | *Added on owner request, revised twice:* **GTA-style locomotion with three movement modes** (`player/locomotion.c`, `locomotion_test`): **walk** 32% of the server speed (~64 u/s, walk clip), **run** 75% (~150 u/s, jog clip, the default), **sprint** 100% (200 u/s, sprint clip; the mode exists, no key yet). Left Ctrl toggles walk and run (`toggle_walk` in keybinds.cfg). Speed accelerates and brakes; the body always moves the way it faces and turns toward the keys (540°/s slow, 360°/s fast, so fast turns curve); reversing from a standstill pivots on the spot before moving, and at speed it brakes, pivots and sets off. Sprint is the server's top speed, which is why run sits below it. Clip rates are calibrated from the root motion in `UAL1_Standard_RM.glb`. | "Think GTA 5 movement." The server only ever sees speeds at or below its maximum. |
| D39 | *Owner request, supersedes D6 and the fixed-pitch owner answer:* **an FF14-style free camera.** Middle/right drag orbits in yaw *and* pitch; the view follows the mouse (drag right turns the view right, drag up looks up). Pitch runs -39°–85° (default 55°): below 0 the eye is under the character's chest looking up, and a camera that would go through the floor slides in along it instead, keeping the eye 4 units up (`camera_math_floor_distance`); zoom runs 1.05–20 (at 20 the eye is ~75 units, 3.4 m, from the character -- over the shoulder); the camera orbits the character's chest (24 units up), not their feet. Because a low camera sees to the horizon: the far plane is `distance + 3000`, only chunks within `LOAD_RADIUS_CHUNKS` of the player are drawn, and the world, bodies and shadows fade into a sky-coloured fog from 950 to 1450 units (`world_light.h`), inside the 1536 units always streamed. Name plates stop growing past their default-zoom size (`CAMERA_CARD_MAX_SCALE`). No camera collision with buildings yet. | "Zoom in closer to the character model ... almost free like FF14." |
| D40 | *Owner request, 2026-09-29:* **the authored city scene is drawn** (`render/city_renderer.c`, loader `model/obj_mesh.c`, `obj_mesh_test`): Blender's `example_city.obj` (4.3M triangles, 2 atlas textures), in metres scaled by the character's 21.9 units/m, its origin on Ennara's courtyard centre. Split into 512-unit cells; only cells inside the fog and the view frustum are drawn. The parsed mesh is cached beside the OBJ (`.objcache`, ~266 MB, untracked) and rebuilt when the OBJ changes. Drawn after the ground and before the ground marks. **Visual only:** world.dat's collision (and its grown structures) are unchanged, so the server does not know the scene exists. | "Render it in my engine." |
| D41 | *Owner request, 2026-09-29:* **the city scene is the whole world, and it has no enemies.** `playing_render()` no longer draws world.dat's ground or the structures grown from it (`world_render`, `world_render_structures`; the code stays). `gen_opening.py` no longer places the enemy camps, so `spawns.json` holds only the 11 courtyard quest NPCs. **world.dat's collision is still live** on client and server: old walls, columns and lamps block movement where nothing is now drawn, until collision comes from the city scene. | "Should be JUST this new map." |
| D42 | *Owner request, 2026-09-29:* **nothing is solid.** `worldgen_write.c` writes an all-zero collision layer, so the old layout's walls, buildings, columns, lamps, trees and rock no longer block movement on client or server (they were invisible after D41). The layout code and its tests still describe what *would* be solid. Players now also walk through the city scene's buildings, until collision is generated from that scene. | "There are random barriers where the player can't pass." |

### What changed where

| Piece | Where |
|---|---|
| glTF loading, posing, skinning (pure, tested) | `Game/src/model/`, `Game/include/model/`; `model_test` |
| Model shader | `Game/src/render/model_renderer.c` |
| Bodies per entity, clip choice, facing | `Game/src/render/character_renderer.c`, `character_motion.c` (`character_motion_test`), tuning in `character_tuning.h` |
| One sun for walls and bodies | `Game/include/render/world_light.h` |
| Structures from collision | `Game/src/world/structure_mesh.c` (`structure_mesh_test`); drawn after the ground with depth by `world_render_structures()` |
| The courtyard | `Game/data/src/worldgen_courtyard.c` (`worldgen_courtyard_test`); palette entries in `tile_palette.h` |
| Frame order | `playing_render()`: ground → ground marks (shadows, rings, telegraphs) → structures → 3D bodies → cards (labels, projectiles, damage numbers) |
| Click targeting | `camera_hit_standing()`: the cursor against the projected feet-to-head segment of the body |

### Operational note

world.dat changed. `make world` in `mmo_client` regenerates it and copies it to
`mmo_server/world_server/data`; the server's `make data` copies it into
`bin/data`. A world server started on the old file would let players walk
through the courtyard's lamps, columns and trees.

### Next

- Per-race models / Animal Forms (Phase 3's skeletons); the mannequin is a stand-in for all of them, animals included.
- Attack, cast, hit and death clips driven by combat events (the clips are in the file: `Sword_Attack`, `Spell_Simple_Shoot`, `Hit_Chest`, `Death01`).
- A silhouette for the local player when a roof hides them.
- Ground textures, and props authored outside the courtyard.

# 3D Refactor — Phase 1: World & Camera

**Goal:** the world is viewed through a tilted, player-rotatable 3D camera
(League-style angle, but the player can spin it). The client *draws* in 3D;
the server keeps simulating a flat 2D game and does not change in this phase.

Status: **steps 0–7 implemented 2026-09-26**, awaiting a play-test on Windows hardware (see §8) · Written 2026-09-26

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
| D6 | **Pitch fixed at 55°** in Phase 1. | Consistent look, and players can't tilt toward the horizon to see across the map (fairness + bounded draw distance). |
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
| D15 | **Max camera distance is clamped so the footprint fits the streamed area.** *As built:* at 1920×1080 the far corners at zoom 1.0 reach ~1550 units at 45° yaw, far past radius 2 (1024), so `LOAD_RADIUS_CHUNKS` is **3** (49 resident chunks of 64) and `CAMERA_ZOOM_MIN` is **1.05** to fit inside 3. Pinned by `camera_math_test` TEST 7. | The far edge of a tilted view extends past the top-down rectangle. Radius 2 would have needed zoom ≥ 1.5, which removed too much of the zoom range. |
| D16 | *(Built; inactive until textured tilesets ship, because today's world is flat-colour.)* **Keep pixel art crisp:** magnify `GL_NEAREST`, minify with mipmaps, anisotropic filtering 8× where supported. Pad tileset atlases with 1–2px extruded gutters at load time so mipmaps don't bleed neighbouring tiles. | Tiles at an angle shimmer without mipmaps; atlases bleed seams with them unless padded. Today everything is `GL_NEAREST` with no mips (`texture.c:47`). |
| D17 | World overview map stays **top-down, north-up**, and shows a **view cone** for the camera's yaw. | A rotating minimap is disorienting; a cone tells you which way you're looking. |

### Entities & overlays
| # | Decision | Why |
|---|---|---|
| D18 | *Revised:* players, NPCs, projectiles and damage numbers become **screen-aligned cards**: each entity's existing 2D drawing code runs unchanged inside `camera_billboard_begin/end`, placed at its projected feet and scaled by perspective. Placeholder until Phase 2 models. | A true vertical (cylindrical) card seen from 55° above is squashed to ~57% height; screen-aligned cards keep sprite art at full proportion, and reuse every existing draw call with no rewrite. |
| D19 | Each card is depth-tested as one flat card at its feet's depth, with **alpha test** so transparent texels don't hide what's behind. | No back-to-front sorting; nearer entities cover farther ones regardless of draw order. |
| D20 | **Telegraphs (circles/cones)** stay ground-drawn at `y = 0` with polygon offset. | They're already world-space; they land on the floor for free. |
| D21 | *Revised:* **health bars, names, damage numbers ride on the entity's card.** | Same result as a separate screen-space pass (upright, readable at any yaw) with no second code path; they scale with zoom exactly as they did top-down. |

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
| D28 | **F9 toggles 3D ↔ top-down.** *Revised:* the top-down path is kept **until the 3D view has been play-tested on Windows hardware**, then deleted. It costs little: both views share one `CameraView`, and the top-down roof split is a shader uniform. | The change was verified on Linux with software GL only. A working fallback on the real machine is worth one more cycle. |
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
| Controls | Q/E rotate, middle- or right-drag rotate (yaw only), scroll zoom, Home reset, F9 view; rebindable in `keybinds.cfg` (`camera_rotate_left/right`, `camera_reset`, `camera_toggle_view`) |
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

## 8. Before deleting the top-down path (D28)

Play-test on the Windows machine, preferably also on integrated graphics:

- [ ] Game starts. The client log shows `[GL] ... Compatibility Profile` and `[GROUND] Shader ready`.
- [ ] Q/E, middle-drag and right-drag rotate smoothly (right-drag: sideways only; right-click on an NPC still targets it); Home resets; scroll zooms 1.05–3.5; starts (and Home returns) at 1.6.
- [ ] W always walks "into the screen" at any rotation; no rise in server `MOVE_REJECT_*` in the world server log.
- [ ] Right-click an NPC and left-click a player at several rotations: the thing under the cursor is what gets targeted.
- [ ] Abilities land where the cursor points at several rotations.
- [ ] No blank ground at the screen edges at max zoom-out while walking.
- [ ] Indoors: the roof fades around you.
- [ ] Minimap cone points the way the camera faces.
- [ ] 60 fps at max zoom-out in Ennara (D29).

Then delete `CAMERA_MODE_TOP_DOWN`, the F9 binding, `camera_view_build_2d`,
and the north/south roof split.

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
| Two render paths drift during development | D28: delete 2D at step 7, no later. |
| Client and server disagree on walls once the world is 3D-authored (Phase 2) | Generate server collision from the same source file; never hand-maintain both. |

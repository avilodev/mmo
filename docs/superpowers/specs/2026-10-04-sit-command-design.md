# /sit command and a scalable chat command table

Status: implemented (client only), uncommitted code; see "Later: everyone sees it".
The sections below describe what was built; "Changes from the first draft" at the end lists where it differs.

## Goal

Typing `/sit` next to a bench plays the robot's sit animation on the nearest
bench. `/stand` (or pressing a movement key) gets up. Chat commands become a
table, so the next command is one entry, not another branch in an if/else
chain.

## Scope

In scope:

- A chat command table that replaces the `strncmp` chain in
  `state_playing_input.c`, with the existing commands moved into it unchanged.
- `/sit` and `/stand`.
- A seat state machine on the local player.
- Three new animation states (sit enter, sit idle, sit exit).
- Drawing the local body at the seat, facing the seat's heading.

Not in scope:

- Other players seeing the sit. Nothing is sent to the server.
- Server-side validation of sitting.
- `Sitting_Talking_Loop` while chatting.
- Sitting on anything that is not a `SIT_*` marker.
- A key binding for sit.

## What exists

- `Game/include/world/city_seats.h`: `city_seats_count()`, `city_seats_get(i)`,
  `city_seats_nearest(x, y, max_dist)`. Positions are world units, heading is
  radians with facing `(sin, cos)`. Baked from the blend's `SIT_*` empties.
- The robot (`UAL1_Standard.glb`) has `Sitting_Enter`, `Sitting_Idle_Loop` and
  `Sitting_Exit`.
- `CharacterAnim` (`render/character_motion.h`) has idle, walk, run, talk, jog.
  Clips are mapped to names in `character_tuning.h` and bound in
  `character_renderer.c`.
- The local body is drawn in `state_playing_render.c` from `game->player`, with
  `CharacterHints` (facing, pivoting).
- Chat submit lives in `state_playing_input.c`: `/invite /leave /friend
  /unfriend /block /unblock /g /p /w /r` are an if/else chain; an unknown
  command is silently dropped.

## Design

### 1. Chat command table

New `include/states/chat_commands.h` and `src/states/chat_commands.c`.

```c
typedef enum { CHAT_ARGS_NONE, CHAT_ARGS_REQUIRED } ChatArgs;

typedef struct {
    const char* name;      /* without the slash: "sit" */
    ChatArgs    args;      /* NONE: the word alone. REQUIRED: a space and text. */
    void      (*run)(GameState* game, const char* args);
    const char* help;
} ChatCommand;

/* @return 1 when `line` starts with '/', whether or not a command ran. */
int chat_command_dispatch(GameState* game, const char* line);
```

- The dispatcher takes the word after `/` (up to the first space), finds it in
  a static table, checks the argument rule, and calls `run`.
- Behaviour of the existing commands does not change: `/leave` with trailing
  text is still dropped, `/invite` with no name is still dropped, an unknown
  command is still silently dropped. This is pinned by tests before the move.
- Handlers for the existing commands call the same `network_*` functions as
  today. `/r` keeps reading `chat.whisper_reply_target`.
- Adding a command is one table entry and one handler. Nothing else changes.

### 2. Seat state

New `include/player/player_seat.h` and `src/player/player_seat.c`. Pure
arithmetic, no GL, headless-testable.

```c
typedef enum { SEAT_NONE, SEAT_ENTERING, SEAT_SEATED, SEAT_EXITING } SeatPhase;

typedef struct {
    SeatPhase phase;
    int       seat;      /* index into city_seats; valid unless SEAT_NONE */
    float     timer;     /* seconds into ENTERING / EXITING */
} SeatState;

#define SEAT_REACH_UNITS 66.0f   /* about 3 m, CITY_UNITS_PER_METRE * 3 */

/* @return 1 when a seat is in reach and the sit began. */
int  seat_sit(SeatState* s, float x, float y);
void seat_stand(SeatState* s);                 /* ENTERING/SEATED -> EXITING */
void seat_update(SeatState* s, float dt, float enter_seconds, float exit_seconds);
int  seat_holds_body(const SeatState* s);      /* phase != SEAT_NONE */
```

- `seat_sit` does nothing unless the phase is `SEAT_NONE` and
  `city_seats_nearest(x, y, SEAT_REACH_UNITS)` finds a seat.
- `seat_update` moves ENTERING to SEATED when `timer >= enter_seconds`, and
  EXITING to NONE when `timer >= exit_seconds`. If the seat has gone (the seats
  file reloaded or failed), it frees the body.
- `seat_draw_pose` gives the drawn x, y, height and heading (see 4).
- Durations come from the clip lengths, passed in (as
  `character_motion_update` already does), so the state does not know clips.
- `SeatState` lives in `PlayerState` and is the only seated state there is. A
  later network packet writes this same struct.

### 3. Animation

- Add `CHARACTER_ANIM_SIT_ENTER`, `CHARACTER_ANIM_SIT_IDLE`,
  `CHARACTER_ANIM_SIT_EXIT` before `CHARACTER_ANIM_COUNT`, with clip names
  `Sitting_Enter`, `Sitting_Idle_Loop`, `Sitting_Exit` in `character_tuning.h`
  and entries in `CLIP_NAMES`.
- `CharacterHints` gets `int has_pose` and `CharacterAnim pose`: when set, the
  body plays that clip regardless of speed.
- ENTER and EXIT play once and hold their last frame (the existing `advance`
  wraps, so one-shot clips clamp instead). IDLE loops.
- `state_playing_render.c` sets the hint from `SeatState`: ENTERING is
  SIT_ENTER, SEATED is SIT_IDLE, EXITING is SIT_EXIT.

### 4. Drawing at the seat

The benches have collision boxes, so a seat position is inside solid geometry.
Moving the player's real position there would make the server reject the move
and snap the player back. So:

- `game->player.x/y` and its vertical state do not change when sitting.
  Nothing new is sent. Movement sync keeps sending the standing position.
- While `seat_holds_body`, the local body is drawn by `seat_draw_pose`: where
  it stands at the start of `Sitting_Enter`, sliding onto the seat (and turning
  the short way round to the seat's heading) as the clip plays, on the seat
  while seated, and sliding back off during `Sitting_Exit`. So nothing pops
  and the speed estimate never sees a jump.
- The body's origin is at its feet, so it is drawn `SEAT_CLIP_SEAT_HEIGHT`
  (0.45 m in world units, `player_seat.h`) below the seat's height: the seat
  height the sit clips were authored for. That value is a guess and cannot be
  checked here; it is tuned by looking at the running game.
- The camera keeps following the real position, a few units from the seat.
- The name label and health bar keep the real position; the shadow follows
  the drawn body.

### 5. Getting up

- While `seat_holds_body`, the movement and jump keys are not read: the input
  handler passes an empty input to `player_update_movement` and
  `player_update_vertical` (gravity still runs). It sits after the chat
  early-out, so typing a W into chat does not stand you up.
- A held movement or jump key, or `/stand`, while ENTERING or SEATED starts
  EXITING.
- When EXITING finishes, control returns at the real position.

## Data flow

`/sit` typed -> `chat_command_dispatch` -> `/sit` handler ->
`seat_sit(&player.seat, player.x, player.y)` -> per frame `seat_update` ->
render picks pose and draw position from `SeatState` and `city_seats_get`.

## Errors and edge cases

- No seat in reach, or already sitting or standing up: `/sit` does nothing.
  (No chat feedback is printed; the existing commands are silent too.)
- Seats file not loaded (`city_seats_count() == 0`): `/sit` does nothing.
  `city_seats_load(CITY_SEATS_PATH)` is added at startup next to
  `city_heights_load` in `main.c`, non-fatal, logged on failure.
- A missing sit clip leaves that animation at the default (existing behaviour
  for any missing clip: `character_renderer.c` logs it).
- Dying, being stunned or entering combat while seated are not handled in
  this change; the state has one exit path, `seat_stand`, that a later change
  can call.

## Testing

Headless unit tests, in the style of `city_heights_test`:

- `chat_commands_test`: each existing command dispatches to its handler with
  the expected argument text; `/leave x` and `/invite` alone are dropped;
  unknown commands are dropped; a line not starting with `/` is not handled;
  `/sit` and `/stand` reach their handlers.
- `player_seat_test`: reach (in, out, edge); the nearer of two seats; no sit
  with no seats; ENTERING to SEATED to EXITING to NONE on timers; standing from
  either phase; no double sit; the body freed if the seats vanish; the drawn
  pose at each phase and the short-way turn.
- `character_motion_test` additions: a forced pose plays whatever the speed, a
  posed body's slide is not movement, one-shot clips hold their last frame and
  the seated idle loops.
- `model_test` now also requires the three sit clips in the shipped model.
- A one-off differential check (not kept) ran the original `strncmp` chain
  against the table on 400,000 generated lines: zero differences.

Compile checks only for `state_playing_render.c`, `state_playing_input.c` and
`main.c`, since the client cannot be built or run on the development machine.
Nothing about how it looks in the game is verified until it is run.

## Later: everyone sees it

The server needs a seated state and the interest-managed broadcast that
carries it, and a decision on whether it validates that a sitter was near a
seat. On the client, the incoming packet writes `SeatState` for remote bodies
and the render code already reads `SeatState`. This design adds no protocol
field, but keeps the seated state in one struct so that change is additive.

## Changes from the first draft

- `seat_update` has no `move_input` parameter: the input handler decides when
  a key stands you up, where the chat early-out already applies.
- The drawn body slides on and off the seat (4) instead of snapping, which the
  first draft would have done, popping up to 3 m on standing.
- `SEAT_CLIP_SEAT_HEIGHT` replaces `SEAT_BODY_DROP`.
- `character_renderer_clip_seconds()` is new, so the seat state can use the
  clips' real lengths.

## Assumptions to confirm

- Reach of 66 world units (about 3 m).
- A movement key stands you up.
- Existing commands are moved into the table with behaviour unchanged.

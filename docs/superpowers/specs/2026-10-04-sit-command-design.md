# /sit command and a scalable chat command table

Status: design, awaiting review. Client only; see "Later: everyone sees it".

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
void seat_update(SeatState* s, float dt, int move_input,
                 float enter_seconds, float exit_seconds);
int  seat_holds_body(const SeatState* s);      /* phase != SEAT_NONE */
```

- `seat_sit` does nothing unless the phase is `SEAT_NONE` and
  `city_seats_nearest(x, y, SEAT_REACH_UNITS)` finds a seat.
- `seat_update` moves ENTERING to SEATED when `timer >= enter_seconds`, and
  EXITING to NONE when `timer >= exit_seconds`. A true `move_input` while
  ENTERING or SEATED calls `seat_stand`.
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
- While `seat_holds_body`, the local body is drawn at the seat's x, y and
  height, facing the seat's heading, instead of at the player position.
- The seat height is the marker's height; the body's origin is at its feet, so
  a `SEAT_BODY_DROP` constant in `character_tuning.h` lowers it to where the
  sit pose puts the hips on the seat. Its value cannot be checked here and is
  tuned in the running game.
- The camera keeps following the real position, a few units from the seat.
- The name label and health bar keep the real position.

### 5. Getting up

- While `seat_holds_body`, movement and jump input are ignored (the early-out
  that already skips movement while typing in chat covers the same place).
- Any movement key or `/stand` while ENTERING or SEATED starts EXITING.
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
- `player_seat_test`: reach (in, out, edge); no sit with no seats loaded;
  ENTERING to SEATED to EXITING to NONE on timers; movement input stands up;
  `/sit` while already seated does nothing.
- `character_motion_test` additions: a forced pose plays that clip, and a
  one-shot clip holds its last frame.

Compile checks only for `state_playing_render.c`, `state_playing_input.c` and
`main.c`, since the client cannot be built or run on the development machine.
Nothing about how it looks in the game is verified until it is run.

## Later: everyone sees it

The server needs a seated state and the interest-managed broadcast that
carries it, and a decision on whether it validates that a sitter was near a
seat. On the client, the incoming packet writes `SeatState` for remote bodies
and the render code already reads `SeatState`. This design adds no protocol
field, but keeps the seated state in one struct so that change is additive.

## Assumptions to confirm

- Reach of 66 world units (about 3 m).
- A movement key stands you up.
- Existing commands are moved into the table with behaviour unchanged.

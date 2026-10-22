#ifndef EMOTE_H
#define EMOTE_H

// Emotes on the client: the table, the check before asking, a body's state.

// To add an emote: an EmoteId, a row in EMOTES (emote.c), and a server row.

#include "protocol.h"
#include "render/character_motion.h"
#include "render/city_renderer.h"
#include "world_seats.h"

#include <stddef.h>
#include <stdint.h>

// Marks "this emote has no such clip" in an emote_def.
#define EMOTE_NO_CLIP CHARACTER_ANIM_COUNT

// Seat height the sit clips were authored for, in world units; tune by looking.
#define EMOTE_SEAT_CLIP_HEIGHT (0.45f * CITY_UNITS_PER_METRE)

// Longest line an emote says in chat.
#define EMOTE_TEXT_MAX 64

typedef enum {
	EMOTE_ANCHOR_NONE, // where it stands
	EMOTE_ANCHOR_SEAT // on the seat the emote's arg names
} emote_anchor;

typedef struct {
	EmoteId id;
	const char* command; // without the slash
	const char* verb; // said in chat when it starts: "Sits"
	const char* stop_text; // said in chat when it ends: "Stands"
	emote_anchor anchor;
	CharacterAnim enter; // plays once first, or EMOTE_NO_CLIP
	CharacterAnim hold; // loops until it ends
	CharacterAnim exit; // plays once last, or EMOTE_NO_CLIP
} emote_def;

typedef enum {
	EMOTE_PHASE_NONE = 0,
	EMOTE_PHASE_ENTERING,
	EMOTE_PHASE_HELD,
	EMOTE_PHASE_EXITING
} emote_phase;

// One body's emote. Zero-initialized is "not emoting".
typedef struct {
	uint8_t id; // EmoteId; EMOTE_NONE when phase is NONE
	uint8_t arg;
	emote_phase phase;
	float timer; // seconds into ENTERING or EXITING
} emote_state;

// What a nearby player is in, for telling whether a seat is taken.
typedef struct {
	uint8_t id;
	uint8_t arg;
} emote_occupant;

// Where a body is drawn while it emotes; heading faces (sin, cos).
typedef struct {
	float x, y, z;
	float heading;
} emote_pose;

// The server's word on the local player's emote (an EmoteStatePacket).
typedef struct {
	uint8_t emote; // EmoteId the player is in now
	uint8_t arg;
	uint8_t result; // EmoteResult: why it changed
	uint8_t requested; // EmoteId asked for, or the one just ended
} emote_event;

// How a line is said in chat: an error in red, an emote as plain text.
typedef enum {
	EMOTE_SAY_NONE = 0,
	EMOTE_SAY_ERROR,
	EMOTE_SAY_EMOTE
} emote_say;

// What to do about an event.
typedef struct {
	emote_say say;
	char text[EMOTE_TEXT_MAX];
	int end_local; // nonzero: the body's own emote must end
} emote_reaction;

// The row for an emote, or NULL for EMOTE_NONE and anything unknown.
const emote_def* emote_lookup(uint8_t id);

// The row for a command word like "sit", or NULL.
const emote_def* emote_find_command(const char* word, size_t length);

// Checks that a body may start an emote before the server is asked.

// A seat emote picks the nearest free seat in reach into out_arg.

// Returns EMOTE_RESULT_STARTED or the refusal the server would likely give.
EmoteResult emote_check(const emote_def* def, float x, float y, int is_dead,
		const emote_occupant* others, int other_count,
		uint8_t* out_arg);

// Starts an emote: ENTERING, or HELD at once without an enter clip.
void emote_begin(emote_state* s, uint8_t id, uint8_t arg);

// Stops one: EXITING, or NONE at once without an exit clip.
void emote_end(emote_state* s);

// Advances one frame; durations holds each CharacterAnim's clip length.
void emote_update(emote_state* s, float dt,
		const float durations[CHARACTER_ANIM_COUNT]);

// Follows what the server says another player is in.

// A first sighting already emoting appears mid-emote, with no enter clip.
void emote_observe(emote_state* s, uint8_t id, uint8_t arg, int first_sight);

// Whether the body is in an emote at all.
int emote_holds_body(const emote_state* s);

// Whether the body must stay put: an anchored emote, until the exit is done.
int emote_pins_body(const emote_state* s);

// The clip the body should play now.
CharacterAnim emote_clip(const emote_state* s);

// The emote clip (an index in the emote clip table) a clip emote plays now:
// its intro, its exit, or the clip itself. -1 for any other emote.
int emote_custom_clip(const emote_state* s);

// Where to draw an anchored emote, sliding on and off it so nothing pops.

// Returns 1 and fills out when drawn away from the real spot, else 0.
int emote_draw_pose(const emote_state* s, float real_x, float real_y,
		float real_z, float real_heading,
		const float durations[CHARACTER_ANIM_COUNT], emote_pose* out);

// A shopkeeper stands on its seat; it sits there for good, at its spot.

#define EMOTE_KEEPER_MATCH_UNITS WORLD_SEAT_KEEPER_MATCH_UNITS

// Returns 1 and fills the drawn pose and clip when (x, y) is on a keeper seat.
int emote_keeper_pose(float x, float y, emote_pose* out, CharacterAnim* clip);

// Works out what an event means for the local player; every wording is here.
emote_reaction emote_react(const emote_event* ev, const char* name,
		const emote_state* local);

// "<name> <stop_text>": said when a player ends their own emote.
void emote_say_stop(char* out, size_t size, const char* name,
		const emote_def* def);

#endif /* EMOTE_H */

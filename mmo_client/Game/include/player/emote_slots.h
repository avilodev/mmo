#ifndef EMOTE_SLOTS_H
#define EMOTE_SLOTS_H

// The six clips the player picks in the emote hub, and where they are kept.

typedef enum {
	EMOTE_SLOT_WALK,
	EMOTE_SLOT_RUN,
	EMOTE_SLOT_SIT,
	EMOTE_SLOT_EMOTE_1,
	EMOTE_SLOT_EMOTE_2,
	EMOTE_SLOT_EMOTE_3,
	EMOTE_SLOT_COUNT
} emote_slot;

// The three slots that start an emote on their own key
#define EMOTE_SLOT_KEYED 3

// Every slot back to its default clip; nothing is saved
void emote_slots_defaults(void);

// Reads the player's file over the defaults, and keeps the path for saving
void emote_slots_load(const char* path);

// Writes the slots to the file named at load; does nothing before that
void emote_slots_save(void);

// The clip (an index in the emote clip table) a slot holds
int emote_slots_get(int slot);

// Puts a clip in a slot and saves; anything may go in any slot
void emote_slots_set(int slot, int clip);

// The slot's name in the hub: "Walk", "Emote 2"
const char* emote_slot_label(int slot);

// The keybinding for an Emote slot (0 to 2), or 0 for another slot
int emote_slot_key(int slot);

#endif /* EMOTE_SLOTS_H */

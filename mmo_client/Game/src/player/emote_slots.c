// The six clips the player picks in the emote hub, kept in a small file.

#include "player/emote_slots.h"
#include "player/emote_clips.h"
#include "core/keybinds.h"
#include "core/client_log.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	const char* id; // its name in the file
	const char* label; // shown in the hub
	const char* clip; // the default, by name
} slot_row;

static const slot_row SLOTS[EMOTE_SLOT_COUNT] = {
	[EMOTE_SLOT_WALK] = { "walk", "Walk", "Walk_Loop" },
	[EMOTE_SLOT_RUN] = { "run", "Run", "Jog_Fwd_Loop" },
	[EMOTE_SLOT_SIT] = { "sit", "Sit", "Sitting_Idle_Loop" },
	[EMOTE_SLOT_EMOTE_1] = { "emote_1", "Emote 1", "Dance_Loop" },
	[EMOTE_SLOT_EMOTE_2] = { "emote_2", "Emote 2", "Yes" },
	[EMOTE_SLOT_EMOTE_3] = { "emote_3", "Emote 3", "Idle_No_Loop" },
};

static int s_clip[EMOTE_SLOT_COUNT];
static int s_ready;
static char s_path[256];

void emote_slots_defaults(void) {
	for(int i = 0; i < EMOTE_SLOT_COUNT; i++) {
		int clip = emote_clip_find(SLOTS[i].clip);

		s_clip[i] = clip < 0 ? 0 : clip;
	}
	s_ready = 1;
}

static void ensure_ready(void) {
	if(!s_ready)
		emote_slots_defaults();
}

// A whole emote (a lean) is not a clip, so only an emote slot can hold one
static int slot_takes(int slot, int clip) {
	return clip < EMOTE_CLIP_COUNT || slot >= EMOTE_SLOT_EMOTE_1;
}

int emote_slots_get(int slot) {
	ensure_ready();
	if(slot < 0 || slot >= EMOTE_SLOT_COUNT)
		return 0;

	return s_clip[slot];
}

const char* emote_slot_label(int slot) {
	if(slot < 0 || slot >= EMOTE_SLOT_COUNT)
		return "";

	return SLOTS[slot].label;
}

int emote_slot_key(int slot) {
	int n = slot - EMOTE_SLOT_EMOTE_1;

	if(n < 0 || n >= EMOTE_SLOT_KEYED)
		return 0;

	return g_keybinds.emote[n];
}

void emote_slots_save(void) {
	FILE* f;

	if(!s_path[0])
		return;

	f = fopen(s_path, "w");
	if(!f) {
		CLOG_WARN("[EMOTES] Could not write %s", s_path);
		return;
	}

	fprintf(f, "# Your emote hub choices, rewritten by the game: slot=clip name\n");
	for(int i = 0; i < EMOTE_SLOT_COUNT; i++)
		fprintf(f, "%s=%s\n", SLOTS[i].id, emote_clip_name(s_clip[i]));

	if(fclose(f) != 0)
		CLOG_WARN("[EMOTES] Could not write %s", s_path);
}

void emote_slots_set(int slot, int clip) {
	ensure_ready();
	if(slot < 0 || slot >= EMOTE_SLOT_COUNT || !emote_clip_name(clip) || !slot_takes(slot, clip))
		return;

	s_clip[slot] = clip;
	emote_slots_save();
}

// One "slot=clip" line; an unknown slot or clip keeps what the slot holds
static void apply_line(char* line) {
	char* eq = strchr(line, '=');
	int clip;

	if(line[0] == '#' || !eq)
		return;

	*eq = '\0';
	clip = emote_clip_find(eq + 1);
	if(clip < 0) {
		CLOG_WARN("[EMOTES] Unknown clip '%s' for '%s'", eq + 1, line);
		return;
	}

	for(int i = 0; i < EMOTE_SLOT_COUNT; i++) {
		if(strcmp(line, SLOTS[i].id) == 0 && slot_takes(i, clip)) {
			s_clip[i] = clip;
			return;
		}
	}
}

void emote_slots_load(const char* path) {
	char line[128];
	FILE* f;

	emote_slots_defaults();
	snprintf(s_path, sizeof(s_path), "%s", path);

	f = fopen(path, "r");
	if(!f) {
		CLOG_INFO("[EMOTES] %s not found, using the default clips", path);
		return;
	}

	while(fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		apply_line(line);
	}

	fclose(f);
	CLOG_INFO("[EMOTES] Loaded from %s", path);
}

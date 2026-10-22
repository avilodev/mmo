// The clips an emote can play, by name, and how they are shown.

// The order is the wire: an EMOTE_CUSTOM names its clip by index. Add to the
// end, and raise EMOTE_CLIP_COUNT in protocol.h (both trees) with it.

#include "player/emote_clips.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char* const CLIPS[] = {
	"Sword_Regular_A",
	"Sword_Regular_B",
	"Sword_Regular_C",
	"Sword_Heavy_Combo",
	"Punch_Jab",
	"Punch_Cross",
	"Melee_Hook",
	"Spell_Simple_Shoot",
	"OverhandThrow",
	"Sword_Attack",
	"Sword_Regular_Combo",
	"Sword_Regular_A_Rec",
	"Sword_Regular_B_Rec",
	"Melee_Hook_Rec",
	"Sword_Block",
	"Shield_OneShot",
	"Shield_Dash",
	"Sword_Dash",
	"Idle_Shield_Break",
	"Zombie_Scratch",
	"Spell_Simple_Enter",
	"Spell_Simple_Idle_Loop",
	"Spell_Simple_Exit",
	"Roll",
	"Slide_Start",
	"Slide_Loop",
	"Slide_Exit",
	"NinjaJump_Start",
	"NinjaJump_Idle_Loop",
	"NinjaJump_Land",
	"Pistol_Shoot",
	"Pistol_Reload",
	"Hit_Chest",
	"Hit_Head",
	"Hit_Knockback",
	"Death01",
	"Chest_Open",
	"ClimbUp_1m",
	"ClimbUp_1m_RM",
	"Consume",
	"Crouch_Fwd_Loop",
	"Crouch_Idle_Loop",
	"Dance_Loop",
	"Driving_Loop",
	"Farm_Harvest",
	"Farm_PlantSeed",
	"Farm_Watering",
	"Fixing_Kneeling",
	"Hit_Knockback_RM",
	"Idle_ClawInspect",
	"Idle_FoldArms_Loop",
	"Idle_Lantern_Loop",
	"Idle_Loop",
	"Idle_No_Loop",
	"Idle_Rail_Call",
	"Idle_Rail_Loop",
	"Idle_Shield_Loop",
	"Idle_Talking_Loop",
	"Idle_TalkingPhone_Loop",
	"Idle_Torch_Loop",
	"Interact",
	"Jog_Fwd_Loop",
	"Jump_Land",
	"Jump_Loop",
	"Jump_Start",
	"LayToIdle",
	"PickUp_Table",
	"Pistol_Aim_Down",
	"Pistol_Aim_Neutral",
	"Pistol_Aim_Up",
	"Pistol_Idle_Loop",
	"Pivot_Left_Loop",
	"Pivot_Right_Loop",
	"Push_Loop",
	"Shield_Dash_RM",
	"Sitting_Enter",
	"Sitting_Exit",
	"Sitting_Idle_Loop",
	"Sitting_Talking_Loop",
	"Sprint_Loop",
	"Swim_Fwd_Loop",
	"Swim_Idle_Loop",
	"Sword_Dash_RM",
	"Sword_Idle",
	"TreeChopping_Loop",
	"Walk_Carry_Loop",
	"Walk_Formal_Loop",
	"Walk_Loop",
	"Walk_RP_1",
	"Walk_RP_2",
	"Walk_RP_3",
	"Yes",
	"Zombie_Idle_Loop",
	"Zombie_Walk_Fwd_Loop",
};

_Static_assert(sizeof(CLIPS) / sizeof(CLIPS[0]) == EMOTE_CLIP_COUNT,
		"EMOTE_CLIP_COUNT in protocol.h must match the clip table");

// Clips that play as intro, middle and exit. Each name is a clip in CLIPS.
typedef struct {
	const char* label; // what the hub calls the set
	const char* enter;
	const char* middle;
	const char* exit;
} clip_set;

static const clip_set SETS[] = {
	{ "Spell Simple", "Spell_Simple_Enter", "Spell_Simple_Idle_Loop", "Spell_Simple_Exit" },
	{ "Slide", "Slide_Start", "Slide_Loop", "Slide_Exit" },
	{ "Ninja Jump", "NinjaJump_Start", "NinjaJump_Idle_Loop", "NinjaJump_Land" },
	{ "Sit Down", "Sitting_Enter", "Sitting_Idle_Loop", "Sitting_Exit" },
	{ "Jump", "Jump_Start", "Jump_Loop", "Jump_Land" },
};

#define SET_COUNT ((int)(sizeof(SETS) / sizeof(SETS[0])))

// The whole-emote entries, in the order of EMOTE_CLIP_LEAN_1 on
static const char* const WHOLE_NAMES[] = { "Lean_1", "Lean_2" };
static const uint8_t WHOLE_EMOTES[] = { EMOTE_LEAN, EMOTE_LEAN_FOOT };

int emote_clip_count(void) {
	return EMOTE_HUB_CLIP_COUNT;
}

uint8_t emote_clip_emote(int index) {
	if(index >= EMOTE_CLIP_COUNT && index < EMOTE_HUB_CLIP_COUNT)
		return WHOLE_EMOTES[index - EMOTE_CLIP_COUNT];

	return EMOTE_CUSTOM;
}

const char* emote_clip_name(int index) {
	if(index < 0 || index >= EMOTE_HUB_CLIP_COUNT)
		return NULL;

	return index < EMOTE_CLIP_COUNT ? CLIPS[index] : WHOLE_NAMES[index - EMOTE_CLIP_COUNT];
}

int emote_clip_find(const char* name) {
	if(!name)
		return -1;

	for(int i = 0; i < EMOTE_HUB_CLIP_COUNT; i++)
		if(strcmp(emote_clip_name(i), name) == 0)
			return i;

	return -1;
}

static int set_of_middle(int index) {
	const char* name = emote_clip_name(index);

	for(int i = 0; name && i < SET_COUNT; i++)
		if(strcmp(SETS[i].middle, name) == 0)
			return i;

	return -1;
}

int emote_clip_enter(int index) {
	int set = set_of_middle(index);

	return set < 0 ? -1 : emote_clip_find(SETS[set].enter);
}

int emote_clip_exit(int index) {
	int set = set_of_middle(index);

	return set < 0 ? -1 : emote_clip_find(SETS[set].exit);
}

// An intro or an exit of a set: shown with its middle clip, not on its own
static int is_hidden(int index) {
	const char* name = emote_clip_name(index);

	for(int i = 0; name && i < SET_COUNT; i++)
		if(strcmp(SETS[i].enter, name) == 0 || strcmp(SETS[i].exit, name) == 0)
			return 1;

	return 0;
}

int emote_hub_count(void) {
	int n = 0;

	for(int i = 0; i < EMOTE_HUB_CLIP_COUNT; i++)
		if(!is_hidden(i))
			n++;

	return n;
}

int emote_hub_clip(int entry) {
	if(entry < 0)
		return -1;

	for(int i = 0; i < EMOTE_HUB_CLIP_COUNT; i++) {
		if(is_hidden(i))
			continue;
		if(entry == 0)
			return i;
		entry--;
	}

	return -1;
}

// "Idle_Torch_Loop" reads "Idle Torch": spaces for underscores, no "Loop"; a set
// reads as its own name
void emote_clip_label(int index, char* out, size_t size) {
	const char* name = emote_clip_name(index);
	int set = set_of_middle(index);
	size_t n;

	if(size == 0)
		return;
	if(!name) {
		out[0] = '\0';
		return;
	}
	if(set >= 0) {
		snprintf(out, size, "%s", SETS[set].label);
		return;
	}

	n = strlen(name);
	if(n > 5 && strcmp(name + n - 5, "_Loop") == 0)
		n -= 5;
	if(n > size - 1)
		n = size - 1;

	for(size_t i = 0; i < n; i++)
		out[i] = name[i] == '_' ? ' ' : name[i];
	out[n] = '\0';
}

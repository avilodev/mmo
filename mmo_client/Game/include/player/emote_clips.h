#ifndef EMOTE_CLIPS_H
#define EMOTE_CLIPS_H

// The clips an emote can play: one table for the hub, the slots and the wire.

#include "protocol.h"

#include <stddef.h>

// Past the wire clips come hub entries that start a whole emote (enter, idle,
// exit). They are not clips, so they never go over the wire as one.
#define EMOTE_CLIP_LEAN_1 EMOTE_CLIP_COUNT
#define EMOTE_CLIP_LEAN_2 (EMOTE_CLIP_COUNT + 1)
#define EMOTE_HUB_CLIP_COUNT (EMOTE_CLIP_COUNT + 2)

// Every hub entry: the wire clips, then the whole-emote entries
int emote_clip_count(void);

// The emote an entry starts: EMOTE_CUSTOM for a clip, or the emote that is it
uint8_t emote_clip_emote(int index);

// A clip with an intro and an exit plays them around itself: the emote's arg is
// the middle clip, and these name the other two. -1 for a clip that has none.
int emote_clip_enter(int index);
int emote_clip_exit(int index);

// The hub shows each such set once, as its middle clip: the intro and exit are
// hidden. Entry n is a clip index; the others skip over the hidden ones.
int emote_hub_count(void);
int emote_hub_clip(int entry);

// The entry's name (a clip's name in the model), or NULL outside the table
const char* emote_clip_name(int index);

// The index of a name, or -1
int emote_clip_find(const char* name);

// A name made readable for the hub: "Idle_Torch_Loop" becomes "Idle Torch"
void emote_clip_label(int index, char* out, size_t size);

#endif /* EMOTE_CLIPS_H */

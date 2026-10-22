#ifndef EMOTE_PREVIEW_H
#define EMOTE_PREVIEW_H

// The player's body drawn into small textures, one per box in the emote hub.

#include <stdint.h>

// A box's picture, in pixels; the hub draws it at the same size in layout units
#define EMOTE_PREVIEW_PX_W 250
#define EMOTE_PREVIEW_PX_H 200

// Boxes drawn at once: the six slots and the grid
#define EMOTE_PREVIEW_CELLS 32

// What a box shows the body doing
typedef enum {
	PREVIEW_WALK, // walking, in this clip
	PREVIEW_RUN, // running, in this clip
	PREVIEW_SIT, // seated, in this clip
	PREVIEW_EMOTE // the clip as an emote
} preview_kind;

typedef struct {
	int cell; // the texture it fills, 0 to EMOTE_PREVIEW_CELLS - 1
	unsigned int body; // the body's id: the same body keeps its place in the clip
	preview_kind kind;
	int clip; // an index in the emote clip table
} preview_job;

// Draws each job into its cell's texture; needs the character model and GL
void emote_preview_render(const preview_job* jobs, int count, const char* race_key,
		uint32_t appearance, float dt);

// The cell's texture, or 0 before it has been drawn
unsigned int emote_preview_texture(int cell);

// Frees the textures and the framebuffer
void emote_preview_release(void);

#endif /* EMOTE_PREVIEW_H */

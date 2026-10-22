// Draw the loading screen: backdrop, title, progress bar and current step.

#include "ui/loading_screen.h"
#include "renderer.h"

#include <stdio.h>

#define BAR_WIDTH 640.0f
#define BAR_HEIGHT 18.0f
#define BAR_EDGE 3.0f

// Draw a line of text centred on a column
static void draw_centered(float centre_x, float y, const char* text, float r, float g, float b) {
	renderer_draw_text_colored(centre_x - renderer_text_width(text) / 2.0f, y, text, r, g, b);
}

void loading_screen_draw(const GameState* game, float progress, const char* label,
                         const char* error) {
	float vw = (float)game->camera.viewport_width;
	float vh = (float)game->camera.viewport_height;
	float bar_x = (vw - BAR_WIDTH) / 2.0f;
	float bar_y = vh * 0.62f;
	char percent[16];

	if(progress < 0.0f)
		progress = 0.0f;
	if(progress > 1.0f)
		progress = 1.0f;

	renderer_begin_screen_space();

	if(game->textures.background != 0)
		renderer_draw_sprite(0, 0, vw, vh, game->textures.background);
	renderer_draw_rect(0, 0, vw, vh, 0.04f, 0.04f, 0.08f, game->textures.background ? 0.7f : 1.0f);

	draw_centered(vw / 2.0f, vh * 0.45f, "Entering the world", 1.0f, 1.0f, 1.0f);

	// Bar: dark frame, darker trough, orange fill
	renderer_draw_rect(bar_x - BAR_EDGE, bar_y - BAR_EDGE, BAR_WIDTH + BAR_EDGE * 2.0f,
	                   BAR_HEIGHT + BAR_EDGE * 2.0f, 0.35f, 0.35f, 0.42f, 1.0f);
	renderer_draw_rect(bar_x, bar_y, BAR_WIDTH, BAR_HEIGHT, 0.08f, 0.08f, 0.12f, 1.0f);
	if(!error)
		renderer_draw_rect(bar_x, bar_y, BAR_WIDTH * progress, BAR_HEIGHT, 0.95f, 0.6f, 0.2f, 1.0f);

	if(error) {
		draw_centered(vw / 2.0f, bar_y + 60.0f, error, 1.0f, 0.4f, 0.4f);
		draw_centered(vw / 2.0f, bar_y + 100.0f, "Press Esc to return to the main menu",
		              0.8f, 0.8f, 0.8f);
	} else {
		snprintf(percent, sizeof(percent), "%d%%", (int)(progress * 100.0f));
		draw_centered(vw / 2.0f, bar_y + 60.0f, label, 0.85f, 0.85f, 0.9f);
		draw_centered(vw / 2.0f, bar_y + 100.0f, percent, 0.6f, 0.6f, 0.7f);
	}

	renderer_end_screen_space();
}

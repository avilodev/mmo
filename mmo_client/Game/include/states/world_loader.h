#ifndef WORLD_LOADER_H
#define WORLD_LOADER_H

// Loads what the game needs when a character enters it, a step a frame: the
// world files, then everything the server sends at join. The menus and
// character select never pay for any of it.

#include "core/game_types.h"

// Queue the world's steps. Nothing loads until world_loader_update runs.
void world_loader_begin(GameState* game);

// Nonzero while the loading screen is up: loading, or stopped on an error
int world_loader_active(void);

// Nonzero if a required step failed
int world_loader_failed(void);

// Run the next step, once the loading screen has been drawn at least once
void world_loader_update(void);

// 0 to 1
float world_loader_progress(void);

// What is loading now, or what failed
const char* world_loader_label(void);

// Why the load stopped, as a line for the screen; empty while all is well
const char* world_loader_error(void);

// Free everything the loader loaded. Safe to call when nothing was loaded.
void world_loader_unload(GameState* game);

#endif /* WORLD_LOADER_H */

#ifndef KEY_ICONS_H
#define KEY_ICONS_H

/**
 * @file
 * Declare the key and mouse button images drawn on the key bindings page.
 */

// Load the sprites; safe to call again once they are loaded
void key_icons_load(void);

// Free the sprites
void key_icons_unload(void);

// Width key_icon_draw gives a key at this size, to place what follows it
float key_icon_width(int key, float size);

// Draw a key in a size by size square, its left edge at x and centred on cy
float key_icon_draw(int key, float x, float cy, float size);

#endif // KEY_ICONS_H

#ifndef KEY_ICON_FILE_H
#define KEY_ICON_FILE_H

/**
 * @file
 * Declare which sprite shows each bindable key on the key bindings page.
 */

#define KEY_ICON_DIR "Game/Sprites/UI/Keyboard/"

// The sprite file of a key or mouse button, or NULL when it has none
const char* key_icon_file(int key);

// How many keys have a sprite, where a key's sprite is listed, and the nth file
int key_icon_count(void);
int key_icon_index(int key);
const char* key_icon_file_at(int index);

#endif // KEY_ICON_FILE_H

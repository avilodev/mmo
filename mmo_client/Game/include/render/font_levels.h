#ifndef FONT_LEVELS_H
#define FONT_LEVELS_H

// The sizes the font is baked at, so text uses the bake nearest its size

// How many bakes there are; each is the one before times the square root of 2
#define FONT_LEVEL_COUNT 5

// The pixel height of a level's bake, given the height of level 0
float font_level_size(int level, float base_size);

// The level whose bake is closest in size to a pixel height
int font_level_for(float pixel_height, float base_size);

#endif /* FONT_LEVELS_H */

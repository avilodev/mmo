#ifndef CHAR_SELECT_NAV_H
#define CHAR_SELECT_NAV_H

// Which entry of the character select cycle is showing: every character in
// the list, then one last entry for a new character.

// How many entries there are for a list of this many characters
int cs_nav_count(int characters);

// The index brought into range; anything before the first entry is the first
int cs_nav_clamp(int index, int characters);

// One step in a direction (-1 or 1), wrapping at both ends
int cs_nav_step(int index, int characters, int dir);

// Whether the index is the new character entry
int cs_nav_is_new(int index, int characters);

#endif /* CHAR_SELECT_NAV_H */

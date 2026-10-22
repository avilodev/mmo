// Pick among the characters and the new character entry.

#include "ui/char_select_nav.h"

int cs_nav_count(int characters) {
	if(characters < 0)
		characters = 0;

	return characters + 1;
}

int cs_nav_clamp(int index, int characters) {
	int last = cs_nav_count(characters) - 1;

	if(index < 0)
		return 0;

	return index > last ? last : index;
}

int cs_nav_step(int index, int characters, int dir) {
	int count = cs_nav_count(characters);
	int at = cs_nav_clamp(index, characters);

	if(dir < 0)
		return (at + count - 1) % count;

	if(dir > 0)
		return (at + 1) % count;

	return at;
}

int cs_nav_is_new(int index, int characters) {
	return cs_nav_clamp(index, characters) == cs_nav_count(characters) - 1;
}

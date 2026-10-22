#ifndef MAP_SIDEBAR_H
#define MAP_SIDEBAR_H

// The map's information column: who you are, where you are going, who is near.

#include "ui/map_cut.h"
#include "ui/map_entities.h"
#include "ui/map_layout.h"

void map_sidebar_draw(const GameState* game, const map_rect* side,
                      const map_entity_list* list, const map_cut_info* cut);

#endif /* MAP_SIDEBAR_H */

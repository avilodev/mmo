#ifndef WORLD_DATABASE_CONFIG_H
#define WORLD_DATABASE_CONFIG_H

#include <stdint.h>

const char* get_database_for_world(const char* world_name);
const char* get_database_for_world_id(uint32_t world_id);
const char* get_world_name_by_id(uint32_t world_id);

#endif 
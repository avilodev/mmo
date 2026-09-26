/**
 * @file
 * Map configured world names and identifiers to PostgreSQL endpoints.
 *
 * The mapping itself lives in world_table.c, read from worlds.conf. This file
 * is the compatibility surface the three services already call. It used to
 * hold the mapping as three parallel hardcoded ladders -- name to connection
 * string, identifier to connection string, identifier to name -- which had to
 * be edited in lockstep with worlds.conf, the realm's world list, each world's
 * .conf and the key-rotation script. They are one table now, and this is a
 * lookup into it.
 */
#include "world_database_config.h"
#include "log.h"
#include "world_table.h"

#include <stdio.h>

/**
 * Select a PostgreSQL connection string by case-insensitive world name.
 *
 * @return      The world's connection string, or NULL for an unknown world.
 *              Owned by the world table and valid until it is reloaded.
 */
const char* get_database_for_world(const char* world_name) {
    if (!world_name) return NULL;

    const WorldEntry* world = world_table_by_name(world_name);
    if (!world) {
        LOG_ERROR("Unknown world server: %s", world_name);
        return NULL;
    }
    return world->conninfo;
}

/**
 * Select a PostgreSQL connection string by world identifier.
 *
 * @return      The world's connection string, or NULL for an invalid identifier.
 *              Owned by the world table and valid until it is reloaded.
 */
const char* get_database_for_world_id(uint32_t world_id) {
    const WorldEntry* world = world_table_by_id(world_id);
    if (!world) {
        LOG_ERROR("Invalid world_id: %u", world_id);
        return NULL;
    }
    return world->conninfo;
}

/**
 * Select a display name by world identifier.
 *
 * @return      The world's name, or "Unknown" for an invalid identifier.
 */
const char* get_world_name_by_id(uint32_t world_id) {
    const WorldEntry* world = world_table_by_id(world_id);
    return world ? world->name : "Unknown";
}

/**
 * @file
 * Map configured world names and identifiers to PostgreSQL endpoints.
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>

/**
 * Select a PostgreSQL connection string by case-insensitive world name.
 *
 * @return      A static connection string, or NULL for an unknown world.
 */
const char* get_database_for_world(const char* world_name) {
    if (!world_name) return NULL;
    
    // North America
    if (strcasecmp(world_name, "Armeia") == 0) {
        return "host=localhost dbname=armeia_db user=postgres";
    }
    if (strcasecmp(world_name, "Bosteuis") == 0) {
        return "host=localhost dbname=bosteuis_db user=postgres";
    }
    if (strcasecmp(world_name, "Cardinal") == 0) {
        return "host=localhost dbname=cardinal_db user=postgres";
    }
    if (strcasecmp(world_name, "Derive") == 0) {
        return "host=localhost dbname=derive_db user=postgres";
    }
    if (strcasecmp(world_name, "Exodus") == 0) {
        return "host=localhost dbname=exodus_db user=postgres";
    }
    if (strcasecmp(world_name, "Prototype") == 0) {
        return "host=localhost dbname=prototype_db user=postgres";
    }
    
    // Europe
    if (strcasecmp(world_name, "Karmel") == 0) {
        return "host=localhost dbname=karmel_db user=postgres";
    }
    if (strcasecmp(world_name, "Longevity") == 0) {
        return "host=localhost dbname=longevity_db user=postgres";
    }
    if (strcasecmp(world_name, "Nervow") == 0) {
        return "host=localhost dbname=nervow_db user=postgres";
    }
    
    // Asia
    if (strcasecmp(world_name, "Jatrus") == 0) {
        return "host=localhost dbname=jatrus_db user=postgres";
    }
    
    fprintf(stderr, "Unknown world server: %s\n", world_name);
    return NULL;
}

/**
 * Select a PostgreSQL connection string by world identifier.
 *
 * @return      A static connection string, or NULL for an invalid identifier.
 */
const char* get_database_for_world_id(uint32_t world_id) {
    switch(world_id) {
        case 1: return "host=localhost dbname=armeia_db user=postgres";
        case 2: return "host=localhost dbname=bosteuis_db user=postgres";
        case 3: return "host=localhost dbname=cardinal_db user=postgres";
        case 4: return "host=localhost dbname=derive_db user=postgres";
        case 5: return "host=localhost dbname=exodus_db user=postgres";
        case 6: return "host=localhost dbname=prototype_db user=postgres";
        case 7: return "host=localhost dbname=karmel_db user=postgres";
        case 8: return "host=localhost dbname=longevity_db user=postgres";
        case 9: return "host=localhost dbname=nervow_db user=postgres";
        case 10: return "host=localhost dbname=jatrus_db user=postgres";
        default:
            fprintf(stderr, "Invalid world_id: %u\n", world_id);
            return NULL;
    }
}

/**
 * Select a display name by world identifier.
 *
 * @return      A static world name, or "Unknown" for an invalid identifier.
 */
const char* get_world_name_by_id(uint32_t world_id) {
    switch(world_id) {
        case 1: return "Armeia";
        case 2: return "Bosteuis";
        case 3: return "Cardinal";
        case 4: return "Derive";
        case 5: return "Exodus";
        case 6: return "Prototype";
        case 7: return "Karmel";
        case 8: return "Longevity";
        case 9: return "Nervow";
        case 10: return "Jatrus";
        default: return "Unknown";
    }
}

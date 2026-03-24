#ifndef ZONE_SYSTEM_H
#define ZONE_SYSTEM_H

#include <stdint.h>

#define MAX_WORLD_ZONES 64

typedef struct {
    uint8_t  id;
    uint8_t  type;        // ZONE_TYPE_* from headers.h
    char     name[48];
    float    x, y, w, h; // World-unit rectangle
} WorldZone;

// Load zones from a JSON file. Returns number of zones loaded, -1 on error.
int  zone_system_init(const char* json_path);
void zone_system_cleanup(void);

// Returns the zone the point falls inside, or NULL if none match.
// When multiple zones overlap, the smallest area wins (more specific zone).
const WorldZone* zone_lookup(float world_x, float world_y);

#endif

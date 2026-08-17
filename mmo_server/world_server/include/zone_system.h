#ifndef ZONE_SYSTEM_H
#define ZONE_SYSTEM_H

#include <stdint.h>

/** Bound rectangular zones loaded for one world. */
#define MAX_WORLD_ZONES 64

/** Define one named axis-aligned zone in world units. */
typedef struct {
    uint8_t  id;
    uint8_t  type;        // ZONE_TYPE_* from headers.h
    char     name[48];
    float    x, y, w, h; // World-unit rectangle
} WorldZone;

// return the loaded zone count or -1 on JSON failure
int  zone_system_init(const char* json_path);
void zone_system_cleanup(void);

// return the smallest matching zone or NULL when outside all zones
const WorldZone* zone_lookup(float world_x, float world_y);

#endif

#ifndef ATTACK_TYPES_H
#define ATTACK_TYPES_H

#include <stdint.h>
#include "protocol.h"

/** Mirror the server class identifier values. */
typedef enum {
    CLASS_NONE       = 0,
    CLASS_GLADIATOR  = 1,
    CLASS_NINJA      = 2,
    CLASS_LANDWEAVER = 3,
    CLASS_SPIRIT     = 4,
    CLASS_COUNT      = 5
} ClassId;

typedef struct AttackDef AttackDef;

/** Render an attack definition at normalized cast progress. */
typedef void (*AttackRenderFn)(const AttackDef* def, 
                                float origin_x, float origin_y,
                                float aim_x, float aim_y,
                                float progress);

/** Describe class attack timing, geometry, color, and optional rendering. */
struct AttackDef {
    uint8_t      id;
    const char*  name;
    AttackType   type;
    
    float        cast_time;      /**< Cast duration in seconds. */
    float        cooldown;       /**< Cooldown duration in seconds. */
    
    float        range;
    float        radius;         /**< Area-of-effect radius in world units. */
    float        cone_angle;     /**< Full cone angle in degrees. */
    float        line_width;     /**< Line attack width in world units. */
    
    float        color_r;
    float        color_g;
    float        color_b;
    float        color_a;
    
    AttackRenderFn custom_render; /**< Optional renderer, or NULL for the default. */
};

#endif // ATTACK_TYPES_H

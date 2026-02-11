#ifndef ATTACK_TYPES_H
#define ATTACK_TYPES_H

#include <stdint.h>

// Attack shape types (must match server's AttackType enum)
typedef enum {
    ATTACK_TYPE_SINGLE = 0,
    ATTACK_TYPE_AOE    = 1,
    ATTACK_TYPE_CONE   = 2,
    ATTACK_TYPE_LINE   = 3,
    ATTACK_TYPE_COUNT  = 4
} AttackType;

// Class IDs (must match server)
typedef enum {
    CLASS_NONE       = 0,
    CLASS_GLADIATOR  = 1,
    CLASS_NINJA      = 2,
    CLASS_LANDWEAVER = 3,
    CLASS_SPIRIT     = 4,
    CLASS_COUNT      = 5
} ClassId;

// Forward declaration for function pointers
typedef struct AttackDef AttackDef;

// Function pointer types for polymorphic behavior
typedef void (*AttackRenderFn)(const AttackDef* def, 
                                float origin_x, float origin_y,
                                float aim_x, float aim_y,
                                float progress);

// Attack definition structure (data-driven)
struct AttackDef {
    uint8_t      id;
    const char*  name;
    AttackType   type;
    
    // Timing
    float        cast_time;
    float        cooldown;
    
    // Shape parameters
    float        range;
    float        radius;        // AOE: circle radius
    float        cone_angle;    // CONE: full angle in degrees
    float        line_width;    // LINE: width of beam
    
    // Visuals
    float        color_r;
    float        color_g;
    float        color_b;
    float        color_a;
    
    // Optional custom render (NULL = use default for type)
    AttackRenderFn custom_render;
};

#endif // ATTACK_TYPES_H
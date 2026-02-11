#include "attack_defs.h"
#include <stddef.h>

// Attack definitions matching server's g_class_profiles
static const AttackDef g_attack_defs[CLASS_COUNT] = {
    // [0] None/Invalid
    {
        .id = 0,
        .name = "None",
        .type = ATTACK_TYPE_SINGLE,
        .cast_time = 0.0f,
        .cooldown = 0.0f,
        .range = 0.0f,
        .radius = 0.0f,
        .cone_angle = 0.0f,
        .line_width = 0.0f,
        .color_r = 0.5f, .color_g = 0.5f, .color_b = 0.5f, .color_a = 0.5f,
        .custom_render = NULL
    },
    
    // [1] Gladiator - Single target heavy melee
    {
        .id = 1,
        .name = "Gladiator Strike",
        .type = ATTACK_TYPE_SINGLE,
        .cast_time = 1.2f,
        .cooldown = 0.8f,
        .range = 50.0f,
        .radius = 20.0f,  // Target indicator size
        .cone_angle = 0.0f,
        .line_width = 0.0f,
        .color_r = 1.0f, .color_g = 0.3f, .color_b = 0.2f, .color_a = 0.6f,
        .custom_render = NULL
    },
    
    // [2] Ninja - Cone cleave
    {
        .id = 2,
        .name = "Shadow Cleave",
        .type = ATTACK_TYPE_CONE,
        .cast_time = 0.3f,
        .cooldown = 0.2f,
        .range = 50.0f,
        .radius = 0.0f,
        .cone_angle = 60.0f,  // 30 degrees each side
        .line_width = 0.0f,
        .color_r = 0.6f, .color_g = 0.2f, .color_b = 0.8f, .color_a = 0.5f,
        .custom_render = NULL
    },
    
    // [3] Landweaver - AOE circle
    {
        .id = 3,
        .name = "Earth Shatter",
        .type = ATTACK_TYPE_AOE,
        .cast_time = 0.8f,
        .cooldown = 1.0f,
        .range = 80.0f,  // Range IS the radius for AOE
        .radius = 80.0f,
        .cone_angle = 0.0f,
        .line_width = 0.0f,
        .color_r = 0.4f, .color_g = 0.8f, .color_b = 0.3f, .color_a = 0.5f,
        .custom_render = NULL
    },
    
    // [4] Spirit - Line/pierce
    {
        .id = 4,
        .name = "Spirit Lance",
        .type = ATTACK_TYPE_LINE,
        .cast_time = 0.6f,
        .cooldown = 0.8f,
        .range = 150.0f,
        .radius = 0.0f,
        .cone_angle = 0.0f,
        .line_width = 12.0f,
        .color_r = 0.3f, .color_g = 0.6f, .color_b = 1.0f, .color_a = 0.6f,
        .custom_render = NULL
    }
};

const AttackDef* attack_def_get(ClassId class_id) {
    if (class_id >= CLASS_COUNT) {
        return &g_attack_defs[CLASS_GLADIATOR]; // Fallback
    }
    return &g_attack_defs[class_id];
}

const AttackDef* attack_def_get_by_type(AttackType type) {
    // Find first attack def matching this type
    for (int i = 1; i < CLASS_COUNT; i++) {
        if (g_attack_defs[i].type == type) {
            return &g_attack_defs[i];
        }
    }
    return &g_attack_defs[CLASS_GLADIATOR];
}
#include "npc.h"
#include "renderer.h"

void npc_render(const VisibleNPC* npc, int tile_size) {
    if (!npc->is_alive) return;
    
    int npc_size = tile_size * 2;
    float x = npc->pos_x;
    float y = npc->pos_y;
    
    // Draw NPC body (red square)
    renderer_draw_rect(
        x - npc_size / 2, 
        y - npc_size / 2,
        npc_size, npc_size,
        0.8f, 0.2f, 0.2f, 1.0f
    );
    
    // Draw health bar
    float bar_width = 40.0f;
    float bar_height = 4.0f;
    float bar_x = x - bar_width / 2;
    float bar_y = y - npc_size / 2 - 8;
    
    // Background
    renderer_draw_rect(bar_x, bar_y, bar_width, bar_height,
                      0.2f, 0.2f, 0.2f, 1.0f);
    
    // Health fill
    if (npc->max_health > 0) {
        float health_pct = (float)npc->health / (float)npc->max_health;
        renderer_draw_rect(bar_x, bar_y, bar_width * health_pct, bar_height,
                          0.2f, 0.8f, 0.2f, 1.0f);
    }
}

void npc_render_all(const VisibleNPC* npcs, int count, int tile_size) {
    for (int i = 0; i < count; i++) {
        npc_render(&npcs[i], tile_size);
    }
}

const VisibleNPC* npc_find_by_id(const VisibleNPC* npcs, int count, uint32_t npc_id) {
    for (int i = 0; i < count; i++) {
        if (npcs[i].npc_id == npc_id) {
            return &npcs[i];
        }
    }
    return NULL;
}

int npc_get_position(const VisibleNPC* npcs, int count, uint32_t npc_id,
                     float* out_x, float* out_y) {
    const VisibleNPC* npc = npc_find_by_id(npcs, count, npc_id);
    if (npc) {
        *out_x = npc->pos_x;
        *out_y = npc->pos_y;
        return 1;
    }
    return 0;
}
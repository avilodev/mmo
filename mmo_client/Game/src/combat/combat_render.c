/**
 * @file
 * Render combat telegraphs, cast progress, cooldowns, and damage events.
 */
#include "combat_render.h"
#include "renderer.h"
#include "camera/camera.h"
#include "camera/camera_tuning.h"
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** Approximate a circle outline with rectangular samples. */
static void draw_circle_outline(float cx, float cy, float radius, 
                                float r, float g, float b, float a,
                                int segments) {
    float thickness = 3.0f;
    
    for (int i = 0; i < segments; i++) {
        float angle = (2.0f * (float)M_PI * i) / segments;
        float x = cx + cosf(angle) * radius;
        float y = cy + sinf(angle) * radius;
        
        renderer_draw_rect(x - thickness/2, y - thickness/2, 
                          thickness, thickness, r, g, b, a);
    } 
}

/** Approximate a filled circle with concentric sampled rings. */
static void draw_circle_filled(float cx, float cy, float radius,
                               float r, float g, float b, float a,
                               int segments) {
    // Draw concentric rings of dots to fill
    int rings = (int)(radius / 4.0f);
    if (rings < 3) rings = 3;
    
    for (int ring = 1; ring <= rings; ring++) {
        float ring_radius = (radius * ring) / rings;
        int ring_segments = segments * ring / rings;
        if (ring_segments < 8) ring_segments = 8;
        
        for (int i = 0; i < ring_segments; i++) {
            float angle = (2.0f * (float)M_PI * i) / ring_segments;
            float x = cx + cosf(angle) * ring_radius;
            float y = cy + sinf(angle) * ring_radius;
            renderer_draw_rect(x - 2, y - 2, 4, 4, r, g, b, a * 0.3f);
        }
    }
}

/** Draw a sampled cone telegraph from an origin toward an aim point. */
static void draw_cone(float origin_x, float origin_y,
                      float aim_x, float aim_y,
                      float range, float angle_degrees,
                      float r, float g, float b, float a) {
    
    float aim_angle = atan2f(aim_y - origin_y, aim_x - origin_x);
    float half_angle = (angle_degrees / 2.0f) * ((float)M_PI / 180.0f);
    
    int arc_segments = 16;
    float angle_step = (2.0f * half_angle) / arc_segments;
    float start_angle = aim_angle - half_angle;
    
    // Draw the arc at the cone's edge
    for (int i = 0; i <= arc_segments; i++) {
        float angle = start_angle + angle_step * i;
        float x = origin_x + cosf(angle) * range;
        float y = origin_y + sinf(angle) * range;
        renderer_draw_rect(x - 2, y - 2, 5, 5, r, g, b, a);
    }
    
    // Draw left edge line (origin to arc)
    float left_angle = aim_angle - half_angle;
    float left_end_x = origin_x + cosf(left_angle) * range;
    float left_end_y = origin_y + sinf(left_angle) * range;
    
    int line_segments = 12;
    for (int i = 0; i <= line_segments; i++) {
        float t = (float)i / line_segments;
        float x = origin_x + (left_end_x - origin_x) * t;
        float y = origin_y + (left_end_y - origin_y) * t;
        renderer_draw_rect(x - 1.5f, y - 1.5f, 4, 4, r, g, b, a);
    }
    
    // Draw right edge line
    float right_angle = aim_angle + half_angle;
    float right_end_x = origin_x + cosf(right_angle) * range;
    float right_end_y = origin_y + sinf(right_angle) * range;
    
    for (int i = 0; i <= line_segments; i++) {
        float t = (float)i / line_segments;
        float x = origin_x + (right_end_x - origin_x) * t;
        float y = origin_y + (right_end_y - origin_y) * t;
        renderer_draw_rect(x - 1.5f, y - 1.5f, 4, 4, r, g, b, a);
    }
    
    // Fill the cone with semi-transparent dots
    for (int i = 0; i < arc_segments; i++) {
        float a1 = start_angle + angle_step * i;
        float a2 = start_angle + angle_step * (i + 1);
        
        // Draw dots along radial lines
        for (int j = 1; j < line_segments; j++) {
            float t = (float)j / line_segments;
            float r1 = range * t;
            
            float x1 = origin_x + cosf(a1) * r1;
            float y1 = origin_y + sinf(a1) * r1;
            float x2 = origin_x + cosf(a2) * r1;
            float y2 = origin_y + sinf(a2) * r1;
            
            renderer_draw_rect((x1 + x2)/2 - 1, (y1 + y2)/2 - 1, 
                              3, 3, r, g, b, a * 0.25f);
        }
    }
}

/** Draw a sampled rectangular beam along an aim direction. */
static void draw_line_beam(float origin_x, float origin_y,
                           float aim_x, float aim_y,
                           float range, float width,
                           float r, float g, float b, float a) {
    
    float dx = aim_x - origin_x;
    float dy = aim_y - origin_y;
    float len = sqrtf(dx*dx + dy*dy);
    
    // Default direction if aim is same as origin
    if (len < 0.001f) {
        dx = 1.0f;
        dy = 0.0f;
        len = 1.0f;
    }
    
    // Normalize direction
    float dir_x = dx / len;
    float dir_y = dy / len;
    
    // Perpendicular vector for width
    float perp_x = -dir_y;
    float perp_y = dir_x;
    
    float half_width = width / 2.0f;
    
    // Four corners of the beam
    float x1 = origin_x + perp_x * half_width;
    float y1 = origin_y + perp_y * half_width;
    float x2 = origin_x - perp_x * half_width;
    float y2 = origin_y - perp_y * half_width;
    float x3 = origin_x + dir_x * range - perp_x * half_width;
    float y3 = origin_y + dir_y * range - perp_y * half_width;
    float x4 = origin_x + dir_x * range + perp_x * half_width;
    float y4 = origin_y + dir_y * range + perp_y * half_width;
    
    int segments = 20;
    
    // Draw top edge (x1 to x4)
    for (int i = 0; i <= segments; i++) {
        float t = (float)i / segments;
        float x = x1 + (x4 - x1) * t;
        float y = y1 + (y4 - y1) * t;
        renderer_draw_rect(x - 2, y - 2, 4, 4, r, g, b, a);
    }
    
    // Draw bottom edge (x2 to x3)
    for (int i = 0; i <= segments; i++) {
        float t = (float)i / segments;
        float x = x2 + (x3 - x2) * t;
        float y = y2 + (y3 - y2) * t;
        renderer_draw_rect(x - 2, y - 2, 4, 4, r, g, b, a);
    }
    
    // Draw end cap (x3 to x4)
    for (int i = 0; i <= 5; i++) {
        float t = (float)i / 5;
        float x = x3 + (x4 - x3) * t;
        float y = y3 + (y4 - y3) * t;
        renderer_draw_rect(x - 2, y - 2, 4, 4, r, g, b, a);
    }
    
    // Fill with semi-transparent line
    for (int i = 1; i < segments; i++) {
        float t = (float)i / segments;
        float cx = origin_x + dir_x * range * t;
        float cy = origin_y + dir_y * range * t;
        renderer_draw_rect(cx - half_width/2, cy - 1, width/2, 3, r, g, b, a * 0.3f);
    }
}

/** Draw concentric targeting rings and crosshairs. */
static void draw_target_reticle(float x, float y, float radius,
                                float r, float g, float b, float a) {
    // Outer circle
    draw_circle_outline(x, y, radius, r, g, b, a, 20);
    
    // Inner circle
    draw_circle_outline(x, y, radius * 0.5f, r, g, b, a * 0.7f, 12);
    
    // Cross hairs
    for (int i = 0; i < 4; i++) {
        float angle = (float)M_PI / 2.0f * i;
        float start_r = radius * 0.6f;
        float end_r = radius * 1.2f;
        
        for (int j = 0; j < 5; j++) {
            float t = (float)j / 4;
            float dist = start_r + (end_r - start_r) * t;
            float px = x + cosf(angle) * dist;
            float py = y + sinf(angle) * dist;
            renderer_draw_rect(px - 1.5f, py - 1.5f, 3, 3, r, g, b, a);
        }
    }
}

/**
 * Render the active attack telegraph with cast-progress animation.
 */
void combat_render_indicator(const CombatState* combat) {
    if (!combat || !combat->is_casting) {
        return;
    }
    
    // Calculate progress for animation effects
    float progress = 0.0f;
    if (combat->cast_duration > 0.0f) {
        progress = combat->cast_elapsed / combat->cast_duration;
        if (progress > 1.0f) progress = 1.0f;
    }
    
    // Pulsing alpha effect
    float pulse = 0.5f + 0.5f * sinf(progress * (float)M_PI * 6.0f);
    float alpha = combat->color_a * (0.6f + 0.4f * pulse);
    
    float r = combat->color_r;
    float g = combat->color_g;
    float b = combat->color_b;
    
    switch (combat->attack_type) {
        case ATTACK_TYPE_AOE:
            {
                // Growing circle effect
                float current_radius = combat->radius * (0.2f + 0.8f * progress);
                
                // Filled area (faint)
                draw_circle_filled(combat->origin_x, combat->origin_y,
                                  current_radius, r, g, b, alpha * 0.3f, 24);
                
                // Current radius outline (bright)
                draw_circle_outline(combat->origin_x, combat->origin_y,
                                   current_radius, r, g, b, alpha, 32);
                
                // Max radius outline (dim)
                draw_circle_outline(combat->origin_x, combat->origin_y,
                                   combat->radius, r, g, b, alpha * 0.3f, 32);
            }
            break;
            
        case ATTACK_TYPE_CONE:
            draw_cone(combat->origin_x, combat->origin_y,
                     combat->aim_x, combat->aim_y,
                     combat->range, combat->cone_angle,
                     r, g, b, alpha);
            break;
            
        case ATTACK_TYPE_LINE:
            draw_line_beam(combat->origin_x, combat->origin_y,
                          combat->aim_x, combat->aim_y,
                          combat->range, combat->line_width,
                          r, g, b, alpha);
            break;
            
        case ATTACK_TYPE_SINGLE:
            // Draw reticle at aim position (where target is)
            draw_target_reticle(combat->aim_x, combat->aim_y,
                               combat->radius, r, g, b, alpha);
            
            // Draw connecting line from player to target
            {
                float dx = combat->aim_x - combat->origin_x;
                float dy = combat->aim_y - combat->origin_y;
                float dist = sqrtf(dx*dx + dy*dy);
                
                if (dist > 1.0f) {
                    int segments = (int)(dist / 10.0f);
                    if (segments < 5) segments = 5;
                    if (segments > 30) segments = 30;
                    
                    for (int i = 0; i < segments; i++) {
                        float t = (float)i / segments;
                        // Animated dash pattern
                        float dash_offset = progress * 10.0f;
                        if (((int)(t * segments + dash_offset)) % 2 == 0) {
                            float px = combat->origin_x + dx * t;
                            float py = combat->origin_y + dy * t;
                            renderer_draw_rect(px - 1.5f, py - 1.5f, 3, 3, 
                                              r, g, b, alpha * 0.5f);
                        }
                    }
                }
            }
            break;
            
        default:
            break;
    }
}

/**
 * Render active floating combat events with age-based motion and fading.
 */
void combat_render_damage_numbers(CombatState* combat) {
    if (!combat) return;
    
    for (int i = 0; i < MAX_DAMAGE_EVENTS; i++) {
        DamageEvent* evt = &combat->damage_events[i];
        if (!evt->active) continue;
        
        // Float upward animation
        float offset_y = -evt->age * 40.0f;
        
        // Fade out over time
        float alpha = 1.0f - (evt->age / 2.0f);
        if (alpha < 0.0f) alpha = 0.0f;
        
        // Scale up briefly at start
        float scale = 1.0f;
        if (evt->age < 0.15f) {
            scale = 1.0f + (1.0f - evt->age / 0.15f) * 0.5f;
        }
        
        // Color based on event type
        float r, g, b;
        if (evt->amount < 0) {
            r = 0.7f; g = 0.7f; b = 0.7f;  // Gray  — MISS
        } else if (evt->is_heal && evt->is_crit) {
            r = 0.0f; g = 1.0f; b = 0.4f;  // Bright green — crit heal
        } else if (evt->is_heal) {
            r = 0.2f; g = 0.85f; b = 0.2f; // Green — normal heal
        } else if (evt->is_kill) {
            r = 1.0f; g = 0.8f; b = 0.0f;  // Gold  — kill
        } else if (evt->is_crit) {
            r = 1.0f; g = 0.5f; b = 0.0f;  // Orange — crit damage
        } else {
            r = 1.0f; g = 0.3f; b = 0.3f;  // Red   — normal damage
        }
        
        // Background shadow
        float text_x = evt->world_x;
        float text_y = evt->world_y + offset_y;

        /* Upright over the target in 3D, rising from mid-body. */
        camera_billboard_begin(evt->world_x, evt->world_y, CAMERA_DAMAGE_TEXT_RISE, 0.0f);
        
        renderer_draw_rect(text_x - 20 * scale, text_y - 8 * scale,
                          40 * scale, 18 * scale,
                          0.0f, 0.0f, 0.0f, alpha * 0.5f);
        
        // Damage number text (or "MISS")
        char buf[16];
        if (evt->amount < 0) {
            snprintf(buf, sizeof(buf), "MISS");
        } else if (evt->is_heal) {
            snprintf(buf, sizeof(buf), "+%d", evt->amount);
        } else {
            snprintf(buf, sizeof(buf), "%d", evt->amount);
        }
        
        renderer_draw_text(text_x - 15, text_y + 5, buf);
        
        // Draw colored indicator square next to number
        renderer_draw_rect(text_x + 15, text_y - 5, 8, 8, r, g, b, alpha);

        camera_billboard_end();
    }
}

/**
 * Render cast progress at the bottom center of the viewport.
 */
void combat_render_cast_bar(const CombatState* combat, float screen_width, float screen_height) {
    if (!combat || !combat->is_casting) {
        return;
    }
    
    // Cast bar dimensions and position (bottom center of screen)
    float bar_width = 250.0f;
    float bar_height = 20.0f;
    float bar_x = (screen_width - bar_width) / 2.0f;
    float bar_y = screen_height - 80.0f;
    
    float progress = 0.0f;
    if (combat->cast_duration > 0.0f) {
        progress = combat->cast_elapsed / combat->cast_duration;
        if (progress > 1.0f) progress = 1.0f;
    }
    
    // Background
    renderer_draw_rect(bar_x - 2, bar_y - 2, bar_width + 4, bar_height + 4,
                      0.0f, 0.0f, 0.0f, 0.8f);
    
    // Empty bar
    renderer_draw_rect(bar_x, bar_y, bar_width, bar_height,
                      0.15f, 0.15f, 0.2f, 1.0f);
    
    // Fill bar with attack color
    renderer_draw_rect(bar_x, bar_y, bar_width * progress, bar_height,
                      combat->color_r, combat->color_g, combat->color_b, 0.9f);
    
    // Glowing edge at fill point
    if (progress > 0.01f && progress < 0.99f) {
        float edge_x = bar_x + bar_width * progress - 2;
        renderer_draw_rect(edge_x, bar_y, 4, bar_height,
                          1.0f, 1.0f, 1.0f, 0.8f);
    }
    
    // Border
    renderer_draw_rect(bar_x, bar_y, bar_width, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(bar_x, bar_y + bar_height - 2, bar_width, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(bar_x, bar_y, 2, bar_height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(bar_x + bar_width - 2, bar_y, 2, bar_height, 0.6f, 0.6f, 0.6f, 1.0f);
    
    // Cast time text
    char time_buf[32];
    float remaining = combat->cast_duration - combat->cast_elapsed;
    if (remaining < 0) remaining = 0;
    snprintf(time_buf, sizeof(time_buf), "%.1fs", remaining);
    renderer_draw_text(bar_x + bar_width + 10, bar_y + bar_height - 5, time_buf);
}

/**
 * Render the active cooldown overlay for an ability icon.
 */
void combat_render_cooldown(const CombatState* combat, float x, float y, float size) {
    if (!combat || combat->cooldown_remaining <= 0.0f) {
        return;
    }
    
    // Darkened overlay
    renderer_draw_rect(x, y, size, size, 0.0f, 0.0f, 0.0f, 0.6f);
    
    // Cooldown sweep (simplified - just show remaining as vertical wipe)
    float pct = combat->cooldown_remaining / combat->cooldown_total;
    if (pct > 1.0f) pct = 1.0f;
    
    float wipe_height = size * pct;
    renderer_draw_rect(x, y, size, wipe_height, 0.0f, 0.0f, 0.0f, 0.4f);
    
    // Cooldown time text
    char cd_buf[8];
    snprintf(cd_buf, sizeof(cd_buf), "%.1f", combat->cooldown_remaining);
    renderer_draw_text(x + size/4, y + size/2 + 5, cd_buf);
}

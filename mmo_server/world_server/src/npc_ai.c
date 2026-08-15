// ============================================================================
// npc_ai.c — NPC AI system implementation
//
// Handles: target acquisition, movement (follow/stationary/maintain range),
// projectile firing, FF14-style telegraph attacks (cast time + ground AOE).
//
// Threading: state mutations under world->lock, sends OUTSIDE all locks.
// Called from combat_update_thread at 20Hz.
// ============================================================================

#include "npc_ai.h"
#include "projectile.h"
#include "player_data.h"
#include "combat_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "utils.h"

// ---------------------------------------------------------------------------
// Extern references
// ---------------------------------------------------------------------------

extern ActivePlayer active_players[];
extern pthread_mutex_t active_players_lock;

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static NPCAIProfile g_ai_profiles[MAX_NPC_AI_PROFILES];
static int          g_ai_profile_count = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static float dist2d(float ax, float ay, float bx, float by) {
    float dx = bx - ax;
    float dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

// ---------------------------------------------------------------------------
// JSON parsing helpers (same minimal parser pattern as loot.c)
// ---------------------------------------------------------------------------

static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static const char* skip_ws(const char* p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char* find_key(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    return skip_ws(pos);
}

static const char* find_matching(const char* start, char open, char close) {
    int depth = 0;
    const char* p = start;
    while (*p) {
        if (*p == open) depth++;
        if (*p == close) { depth--; if (depth == 0) return p; }
        p++;
    }
    return NULL;
}

static void parse_string_val(const char* json, const char* key, char* out, int max_len) {
    const char* v = find_key(json, key);
    if (!v || *v != '"') { out[0] = '\0'; return; }
    v++;
    int i = 0;
    while (*v && *v != '"' && i < max_len - 1) {
        out[i++] = *v++;
    }
    out[i] = '\0';
}

// ---------------------------------------------------------------------------
// Parse npc_types.json
// ---------------------------------------------------------------------------

static int parse_npc_types(const char* json) {
    const char* types_start = strstr(json, "\"npc_types\"");
    if (!types_start) {
        printf("[NPC_AI] No npc_types section found\n");
        return 1;
    }

    const char* arr_start = strchr(types_start, '[');
    if (!arr_start) return 0;
    const char* arr_end = find_matching(arr_start, '[', ']');
    if (!arr_end) return 0;

    int arr_len = (int)(arr_end - arr_start + 1);
    char* arr_json = malloc(arr_len + 1);
    if (!arr_json) {
        fprintf(stderr, "[NPC_AI] malloc failed while parsing npc_types\n");
        return 0;
    }
    memcpy(arr_json, arr_start, arr_len);
    arr_json[arr_len] = '\0';

    const char* pos = arr_json + 1;
    while (*pos && g_ai_profile_count < MAX_NPC_AI_PROFILES) {
        pos = skip_ws(pos);
        if (*pos == ']') break;
        if (*pos != '{') { pos++; continue; }

        const char* obj_end = find_matching(pos, '{', '}');
        if (!obj_end) break;

        int obj_len = (int)(obj_end - pos + 1);
        char* obj = malloc(obj_len + 1);
        if (!obj) {
            fprintf(stderr, "[NPC_AI] malloc failed while parsing NPC type object\n");
            free(arr_json);
            return 0;
        }
        memcpy(obj, pos, obj_len);
        obj[obj_len] = '\0';

        NPCAIProfile* prof = &g_ai_profiles[g_ai_profile_count];
        memset(prof, 0, sizeof(NPCAIProfile));

        const char* v;
        v = find_key(obj, "npc_type_id");
        if (v) prof->npc_type_id = (uint16_t)atoi(v);

        char move_str[32] = {0};
        parse_string_val(obj, "movement", move_str, sizeof(move_str));
        if (strcmp(move_str, "follow") == 0)
            prof->movement_type = NPC_MOVE_FOLLOW;
        else if (strcmp(move_str, "maintain_range") == 0)
            prof->movement_type = NPC_MOVE_MAINTAIN_RANGE;
        else
            prof->movement_type = NPC_MOVE_STATIONARY;

        v = find_key(obj, "move_speed");
        if (v) prof->move_speed = (float)atof(v);
        else prof->move_speed = 80.0f;

        v = find_key(obj, "preferred_range");
        if (v) prof->preferred_range = (float)atof(v);
        else prof->preferred_range = 200.0f;

        v = find_key(obj, "aggro_range");
        if (v) prof->aggro_range = (float)atof(v);
        else prof->aggro_range = 300.0f;

        v = find_key(obj, "leash_range");
        if (v) prof->leash_range = (float)atof(v);
        else prof->leash_range = 500.0f;

        // Parse abilities array
        const char* abilities_start = strstr(obj, "\"abilities\"");
        if (abilities_start) {
            const char* ab_arr = strchr(abilities_start, '[');
            if (ab_arr) {
                const char* ab_end = find_matching(ab_arr, '[', ']');
                if (ab_end) {
                    const char* ap = ab_arr + 1;
                    while (*ap && prof->ability_count < MAX_NPC_ABILITIES) {
                        ap = skip_ws(ap);
                        if (*ap == ']') break;
                        if (*ap != '{') { ap++; continue; }

                        const char* ae = find_matching(ap, '{', '}');
                        if (!ae) break;

                        int ae_len = (int)(ae - ap + 1);
                        char* ab_obj = malloc(ae_len + 1);
                        if (!ab_obj) {
                            fprintf(stderr, "[NPC_AI] malloc failed while parsing ability object\n");
                            free(obj);
                            free(arr_json);
                            return 0;
                        }
                        memcpy(ab_obj, ap, ae_len);
                        ab_obj[ae_len] = '\0';

                        NPCAbilityDef* ab = &prof->abilities[prof->ability_count];
                        memset(ab, 0, sizeof(NPCAbilityDef));

                        v = find_key(ab_obj, "ability_id");
                        if (v) ab->ability_id = (uint16_t)atoi(v);

                        v = find_key(ab_obj, "damage");
                        if (v) ab->damage = atoi(v);

                        v = find_key(ab_obj, "range");
                        if (v) ab->range = (float)atof(v);

                        v = find_key(ab_obj, "cooldown");
                        if (v) ab->cooldown = (float)atof(v);
                        else ab->cooldown = 2.0f;

                        // Delivery type
                        char delivery_str[32] = {0};
                        parse_string_val(ab_obj, "delivery", delivery_str, sizeof(delivery_str));
                        if (strcmp(delivery_str, "telegraph") == 0)
                            ab->delivery = NPC_DELIVERY_TELEGRAPH;
                        else
                            ab->delivery = NPC_DELIVERY_PROJECTILE;

                        // Projectile fields
                        v = find_key(ab_obj, "projectile_speed");
                        if (v) ab->projectile_speed = (float)atof(v);

                        v = find_key(ab_obj, "projectile_width");
                        if (v) ab->projectile_width = (float)atof(v);
                        else ab->projectile_width = 15.0f;

                        // Telegraph fields
                        v = find_key(ab_obj, "cast_time");
                        if (v) ab->cast_time = (float)atof(v);

                        char shape_str[32] = {0};
                        parse_string_val(ab_obj, "telegraph_shape", shape_str, sizeof(shape_str));
                        if (strcmp(shape_str, "cone") == 0)
                            ab->telegraph_shape = NPC_TELEGRAPH_CONE;
                        else if (strcmp(shape_str, "rectangle") == 0)
                            ab->telegraph_shape = NPC_TELEGRAPH_RECTANGLE;
                        else if (strcmp(shape_str, "line") == 0)
                            ab->telegraph_shape = NPC_TELEGRAPH_LINE;
                        else
                            ab->telegraph_shape = NPC_TELEGRAPH_CIRCLE;

                        v = find_key(ab_obj, "telegraph_radius");
                        if (v) ab->telegraph_radius = (float)atof(v);

                        v = find_key(ab_obj, "telegraph_angle");
                        if (v) ab->telegraph_angle = (float)atof(v);

                        v = find_key(ab_obj, "telegraph_width");
                        if (v) ab->telegraph_width = (float)atof(v);

                        v = find_key(ab_obj, "telegraph_length");
                        if (v) ab->telegraph_length = (float)atof(v);

                        v = find_key(ab_obj, "telegraph_at_target");
                        if (v) ab->telegraph_at_target = (uint8_t)atoi(v);

                        v = find_key(ab_obj, "teleport_on_resolve");
                        if (v) ab->teleport_on_resolve = (uint8_t)atoi(v);

                        prof->ability_count++;
                        free(ab_obj);

                        ap = ae + 1;
                        while (*ap && *ap != ',' && *ap != ']') ap++;
                        if (*ap == ',') ap++;
                    }
                }
            }
        }

        if (prof->npc_type_id > 0) {
            printf("[NPC_AI] Loaded profile npc_type_id=%u: movement=%s, aggro=%.0f, %d abilities\n",
                   prof->npc_type_id, move_str, prof->aggro_range, prof->ability_count);
            g_ai_profile_count++;
        }

        free(obj);
        pos = obj_end + 1;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }

    free(arr_json);
    return 1;
}

// ============================================================================
// API — Init / Cleanup
// ============================================================================

int npc_ai_init(const char* json_path) {
    g_ai_profile_count = 0;

    char* json = read_file(json_path);
    if (!json) {
        fprintf(stderr, "[NPC_AI] Failed to read %s\n", json_path);
        return 0;
    }

    int result = parse_npc_types(json);
    free(json);

    printf("[NPC_AI] Initialized with %d NPC type profiles\n", g_ai_profile_count);
    return result;
}

void npc_ai_cleanup(void) {
    g_ai_profile_count = 0;
    printf("[NPC_AI] Cleaned up\n");
}

const NPCAIProfile* npc_ai_get_profile(uint16_t npc_type_id) {
    for (int i = 0; i < g_ai_profile_count; i++) {
        if (g_ai_profiles[i].npc_type_id == npc_type_id) {
            return &g_ai_profiles[i];
        }
    }
    return NULL;
}

// ============================================================================
// Telegraph geometry — check if a point is inside a telegraph shape
// ============================================================================

static int point_in_circle(float px, float py, float cx, float cy, float radius) {
    return dist2d(px, py, cx, cy) <= radius;
}

static int point_in_cone(float px, float py, float cx, float cy,
                          float dir_x, float dir_y, float radius, float angle_deg) {
    float dx = px - cx;
    float dy = py - cy;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > radius) return 0;
    if (dist < 0.001f) return 1; // At center

    float dot = (dx * dir_x + dy * dir_y) / dist;
    float half_angle_rad = (angle_deg / 2.0f) * (float)M_PI / 180.0f;
    return dot >= cosf(half_angle_rad);
}

static int point_in_rectangle(float px, float py, float cx, float cy,
                                float dir_x, float dir_y, float width, float length) {
    // Rectangle extends from center in dir direction for length, width perpendicular
    float dx = px - cx;
    float dy = py - cy;

    // Project onto direction axis (length)
    float along = dx * dir_x + dy * dir_y;
    if (along < 0.0f || along > length) return 0;

    // Project onto perpendicular axis (width)
    float perp = dx * (-dir_y) + dy * dir_x;
    float half_w = width / 2.0f;
    return (perp >= -half_w && perp <= half_w);
}

static int point_in_telegraph(float px, float py,
                                uint8_t shape,
                                float cx, float cy,
                                float dir_x, float dir_y,
                                float radius, float angle,
                                float width, float length) {
    switch (shape) {
        case NPC_TELEGRAPH_CIRCLE:
            return point_in_circle(px, py, cx, cy, radius);
        case NPC_TELEGRAPH_CONE:
            return point_in_cone(px, py, cx, cy, dir_x, dir_y, radius, angle);
        case NPC_TELEGRAPH_RECTANGLE:
            return point_in_rectangle(px, py, cx, cy, dir_x, dir_y, width, length);
        case NPC_TELEGRAPH_LINE:
            return point_in_rectangle(px, py, cx, cy, dir_x, dir_y, width, length);
        default:
            return 0;
    }
}

// ============================================================================
// Deferred send types — collected under lock, sent after unlock
// ============================================================================

#define MAX_DEFERRED 128

typedef enum {
    DSEND_TELEGRAPH_START,
    DSEND_TELEGRAPH_RESOLVE,
    DSEND_TELEGRAPH_DAMAGE,
    DSEND_PROJECTILE_SHOT
} DeferredType;

typedef struct {
    DeferredType type;
    union {
        // Telegraph start — broadcast to nearby players
        struct {
            uint32_t npc_id;
            uint16_t ability_id;
            uint8_t  shape;
            float    pos_x, pos_y;
            float    dir_x, dir_y;
            float    radius, angle, width, length;
            float    cast_time;
            float    npc_x, npc_y; // NPC position for range check
        } tstart;

        // Telegraph resolve — broadcast + deal damage
        struct {
            uint32_t npc_id;
            uint16_t ability_id;
            uint8_t  shape;
            float    pos_x, pos_y;
            float    dir_x, dir_y;
            float    radius, angle, width, length;
            int      damage;
            float    npc_x, npc_y;
        } tresolve;

        // Projectile shot
        struct {
            uint32_t npc_id;
            uint16_t ability_id;
            float    origin_x, origin_y;
            float    target_x, target_y;
            int      damage;
            float    speed;
            float    width;
            float    range;
        } proj;
    };
} DeferredAction;

typedef struct {
    DeferredAction items[MAX_DEFERRED];
    int count;
} DeferredQueue;

static void dq_init(DeferredQueue* q) { q->count = 0; }

static void dq_push(DeferredQueue* q, const DeferredAction* item) {
    if (q->count < MAX_DEFERRED) {
        q->items[q->count++] = *item;
    }
}

// ============================================================================
// Flush deferred queue — OUTSIDE all locks
// ============================================================================

static void dq_flush(DeferredQueue* q) {
    for (int i = 0; i < q->count; i++) {
        DeferredAction* d = &q->items[i];

        switch (d->type) {
            case DSEND_TELEGRAPH_START: {
                // Build packet
                NPCTelegraphStartPacket pkt = {0};
                pkt.header.type         = PACKET_NPC_TELEGRAPH_START;
                pkt.header.player_id    = 0;
                pkt.header.payload_size = htons(sizeof(NPCTelegraphStartPacket) - sizeof(PacketHeader));
                pkt.npc_id     = htonl(d->tstart.npc_id);
                pkt.ability_id = htons(d->tstart.ability_id);
                pkt.shape      = d->tstart.shape;
                pkt.pos_x      = d->tstart.pos_x;
                pkt.pos_y      = d->tstart.pos_y;
                pkt.dir_x      = d->tstart.dir_x;
                pkt.dir_y      = d->tstart.dir_y;
                pkt.radius     = d->tstart.radius;
                pkt.angle      = d->tstart.angle;
                pkt.width      = d->tstart.width;
                pkt.length     = d->tstart.length;
                pkt.cast_time  = d->tstart.cast_time;

                // Send to nearby players
                pthread_mutex_lock(&active_players_lock);
                for (int p = 0; p < MAX_PLAYERS; p++) {
                    if (!active_players[p].is_loaded) continue;
                    float pd = dist2d(d->tstart.npc_x, d->tstart.npc_y,
                                      active_players[p].pos_x, active_players[p].pos_y);
                    if (pd <= 500.0f) {
                        server_send(active_players[p].client_fd, &pkt, sizeof(pkt));
                    }
                }
                pthread_mutex_unlock(&active_players_lock);
                break;
            }

            case DSEND_TELEGRAPH_RESOLVE: {
                // Send resolve packet to nearby players
                NPCTelegraphResolvePacket rpkt = {0};
                rpkt.header.type         = PACKET_NPC_TELEGRAPH_RESOLVE;
                rpkt.header.player_id    = 0;
                rpkt.header.payload_size = htons(sizeof(NPCTelegraphResolvePacket) - sizeof(PacketHeader));
                rpkt.npc_id     = htonl(d->tresolve.npc_id);
                rpkt.ability_id = htons(d->tresolve.ability_id);

                // Deal damage to players inside the shape
                pthread_mutex_lock(&active_players_lock);
                for (int p = 0; p < MAX_PLAYERS; p++) {
                    if (!active_players[p].is_loaded) continue;

                    // Send resolve packet to nearby players
                    float pd = dist2d(d->tresolve.npc_x, d->tresolve.npc_y,
                                      active_players[p].pos_x, active_players[p].pos_y);
                    if (pd <= 500.0f) {
                        server_send(active_players[p].client_fd, &rpkt, sizeof(rpkt));
                    }

                    // Check if player is inside the telegraph shape
                    if (active_players[p].is_dead) continue;

                    int inside = point_in_telegraph(
                        active_players[p].pos_x, active_players[p].pos_y,
                        d->tresolve.shape,
                        d->tresolve.pos_x, d->tresolve.pos_y,
                        d->tresolve.dir_x, d->tresolve.dir_y,
                        d->tresolve.radius, d->tresolve.angle,
                        d->tresolve.width, d->tresolve.length);

                    if (!inside) continue;

                    // Apply damage
                    pthread_mutex_lock(&active_players[p].lock);

                    int damage = d->tresolve.damage;
                    int variance = (rand() % 21) - 10;
                    damage += (damage * variance) / 100;
                    damage = combat_apply_defense(damage, active_players[p].defense);
                    if (damage < 1) damage = 1;

                    active_players[p].health -= damage;
                    if (active_players[p].health < 0) active_players[p].health = 0;
                    active_players[p].last_combat_time = get_time();

                    int new_hp = active_players[p].health;
                    uint8_t is_kill = (new_hp == 0) ? 1 : 0;
                    int client_fd = active_players[p].client_fd;
                    uint32_t char_id = active_players[p].character_id;

                    pthread_mutex_unlock(&active_players[p].lock);

                    // Send damage via AbilityEffectPacket
                    AbilityEffectPacket epkt = {0};
                    epkt.header.type         = PACKET_ABILITY_EFFECT;
                    epkt.header.payload_size = htons(sizeof(AbilityEffectPacket) - sizeof(PacketHeader));
                    epkt.header.player_id  = htonl(d->tresolve.npc_id);
                    epkt.caster_id         = htonl(d->tresolve.npc_id);
                    epkt.target_id         = htonl(char_id);
                    epkt.ability_id        = htons(d->tresolve.ability_id);
                    epkt.damage            = htonl((uint32_t)damage);
                    epkt.healing           = 0;
                    epkt.target_new_health = htonl((uint32_t)new_hp);
                    epkt.is_kill           = is_kill;
                    server_send(client_fd, &epkt, sizeof(epkt));

                    printf("[NPC_AI] Telegraph hit player %u for %d dmg (hp=%d)%s\n",
                           char_id, damage, new_hp, is_kill ? " — KILLED" : "");
                }
                pthread_mutex_unlock(&active_players_lock);
                break;
            }

            case DSEND_PROJECTILE_SHOT: {
                ProjectileSpawnInfo info = {0};
                info.owner_type  = PROJECTILE_OWNER_NPC;
                info.owner_id    = d->proj.npc_id;
                info.ability_id  = d->proj.ability_id;
                info.owner_fd    = -1;
                info.origin_x    = d->proj.origin_x;
                info.origin_y    = d->proj.origin_y;
                info.aim_x       = d->proj.target_x;
                info.aim_y       = d->proj.target_y;
                info.speed       = d->proj.speed;
                info.width       = d->proj.width;
                info.max_range   = d->proj.range;
                info.damage      = d->proj.damage;
                info.damage_type = ABILITY_DMG_PHYSICAL;
                projectile_spawn(&info);
                break;
            }

            case DSEND_TELEGRAPH_DAMAGE:
                break; // Handled as part of RESOLVE
        }
    }
}

// ============================================================================
// AI Tick — called from combat_update_thread at 20Hz
// ============================================================================

void npc_ai_tick(NPCWorld* world, double delta_time) {
    double now = get_time();
    float dt = (float)delta_time;

    // -------------------------------------------------------------------
    // 1. Snapshot active player positions
    // -------------------------------------------------------------------
    typedef struct {
        uint32_t character_id;
        float    pos_x, pos_y;
        int      is_dead;
        int      valid;
    } PlayerTarget;

    PlayerTarget targets[MAX_PLAYERS];
    int target_count = 0;

    pthread_mutex_lock(&active_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) {
            targets[i].valid = 0;
            continue;
        }
        targets[i].valid = 1;
        targets[i].character_id = active_players[i].character_id;
        targets[i].pos_x = active_players[i].pos_x;
        targets[i].pos_y = active_players[i].pos_y;
        targets[i].is_dead = active_players[i].is_dead;
        target_count++;
    }
    pthread_mutex_unlock(&active_players_lock);

    if (target_count == 0) return;

    // -------------------------------------------------------------------
    // 2. Process each NPC under world lock
    // -------------------------------------------------------------------
    DeferredQueue q;
    dq_init(&q);

    pthread_mutex_lock(&world->lock);

    for (int n = 0; n < MAX_NPCS; n++) {
        NPCEntity* npc = &world->npcs[n];
        if (npc->id == 0 || !npc->is_alive) continue;
        if (npc->npc_type_id == 0) continue;

        const NPCAIProfile* prof = npc_ai_get_profile(npc->npc_type_id);
        if (!prof || prof->ability_count == 0) continue;

        // Seed per-enemy cooldown phases once — each NPC instance gets a unique
        // random offset within its full cooldown window so enemies of the same
        // type never fire in sync with each other.
        if (!npc->ai_cd_seeded) {
            for (int a = 0; a < prof->ability_count && a < MAX_NPC_ABILITIES_RT; a++) {
                double cd = prof->abilities[a].cooldown;
                if (cd > 0.0) {
                    // Place this ability at a random point in [0, cd] of its cycle.
                    // Storing (now - random_elapsed) means next fire in (cd - random_elapsed).
                    double elapsed = cd * ((double)(rand() % 1000) / 1000.0);
                    npc->ai_ability_cooldowns[a] = now - elapsed;
                }
            }
            npc->ai_cd_seeded = 1;
        }

        float spawn_dist = dist2d(npc->pos_x, npc->pos_y, npc->spawn_x, npc->spawn_y);

        // =============================================================
        // State: CASTING — NPC is locked, waiting for telegraph to resolve
        // =============================================================
        if (npc->ai_state == NPC_AI_CASTING && npc->ai_is_casting) {
            int abi = npc->ai_cast_ability_idx;
            if (abi < 0 || abi >= prof->ability_count) {
                // Invalid — cancel
                npc->ai_is_casting = 0;
                npc->ai_state = NPC_AI_AGGRO;
                continue;
            }

            const NPCAbilityDef* ab = &prof->abilities[abi];
            double elapsed = now - npc->ai_cast_start;

            if (elapsed >= ab->cast_time) {
                // Cast complete — resolve the telegraph (deal damage)
                npc->ai_is_casting = 0;
                float v = (float)(rand() % 40 - 20) / 100.0f;
                npc->ai_ability_cooldowns[abi] = now - ab->cooldown * v;
                npc->ai_state = NPC_AI_AGGRO;

                // Teleport to end of line (e.g., Kingdom Slime charge)
                if (ab->teleport_on_resolve) {
                    npc->pos_x = npc->ai_cast_pos_x + npc->ai_cast_dir_x * ab->telegraph_length;
                    npc->pos_y = npc->ai_cast_pos_y + npc->ai_cast_dir_y * ab->telegraph_length;
                    printf("[NPC_AI] NPC %u (%s) teleported to (%.1f, %.1f)\n",
                           npc->id, npc->name, npc->pos_x, npc->pos_y);
                }

                DeferredAction da = {0};
                da.type = DSEND_TELEGRAPH_RESOLVE;
                da.tresolve.npc_id     = npc->id;
                da.tresolve.ability_id = ab->ability_id;
                da.tresolve.shape      = ab->telegraph_shape;
                da.tresolve.pos_x      = npc->ai_cast_pos_x;
                da.tresolve.pos_y      = npc->ai_cast_pos_y;
                da.tresolve.dir_x      = npc->ai_cast_dir_x;
                da.tresolve.dir_y      = npc->ai_cast_dir_y;
                da.tresolve.radius     = ab->telegraph_radius;
                da.tresolve.angle      = ab->telegraph_angle;
                da.tresolve.width      = ab->telegraph_width;
                da.tresolve.length     = ab->telegraph_length;
                da.tresolve.damage     = ab->damage;
                da.tresolve.npc_x      = npc->pos_x;
                da.tresolve.npc_y      = npc->pos_y;
                dq_push(&q, &da);

                printf("[NPC_AI] NPC %u (%s) telegraph resolved — ability %u\n",
                       npc->id, npc->name, ab->ability_id);
            }
            // While casting, NPC is locked — no movement, no other abilities
            continue;
        }

        // =============================================================
        // State: RETURNING — walk back to spawn
        // =============================================================
        if (npc->ai_state == NPC_AI_RETURNING) {
            if (spawn_dist < 5.0f) {
                npc->pos_x = npc->spawn_x;
                npc->pos_y = npc->spawn_y;
                npc->ai_state = NPC_AI_IDLE;
                npc->ai_target_id = 0;
                npc->health = npc->max_health;
                continue;
            }
            float dx = npc->spawn_x - npc->pos_x;
            float dy = npc->spawn_y - npc->pos_y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.001f) {
                float move = prof->move_speed * 1.5f * dt;
                if (move > len) move = len;
                npc->pos_x += (dx / len) * move;
                npc->pos_y += (dy / len) * move;
            }
            continue;
        }

        // =============================================================
        // Find best target
        // =============================================================
        float best_dist = 1e9f;
        int best_target = -1;

        // Once aggroed, keep chasing until the target is more than
        // NPC_HOLD_AGGRO_RANGE units away from the NPC itself.
        // This is intentionally larger than the initial aggro_range so the
        // enemy feels relentless once it locks on.
#define NPC_HOLD_AGGRO_RANGE 250.0f

        if (npc->ai_target_id > 0) {
            for (int t = 0; t < MAX_PLAYERS; t++) {
                if (!targets[t].valid) continue;
                if (targets[t].character_id == npc->ai_target_id) {
                    if (!targets[t].is_dead) {
                        float d = dist2d(npc->pos_x, npc->pos_y,
                                         targets[t].pos_x, targets[t].pos_y);
                        if (spawn_dist < prof->leash_range &&
                            d < NPC_HOLD_AGGRO_RANGE) {
                            best_target = t;
                            best_dist = d;
                        }
                    }
                    break;
                }
            }
        }

        if (best_target == -1) {
            for (int t = 0; t < MAX_PLAYERS; t++) {
                if (!targets[t].valid || targets[t].is_dead) continue;
                float d = dist2d(npc->pos_x, npc->pos_y,
                                 targets[t].pos_x, targets[t].pos_y);
                if (d <= prof->aggro_range && d < best_dist) {
                    best_dist = d;
                    best_target = t;
                }
            }
        }

        if (best_target == -1) {
            if (npc->ai_state == NPC_AI_AGGRO) {
                npc->ai_state = NPC_AI_RETURNING;
                npc->ai_target_id = 0;
            }
            continue;
        }

        if (spawn_dist >= prof->leash_range) {
            npc->ai_state = NPC_AI_RETURNING;
            npc->ai_target_id = 0;
            continue;
        }

        // =============================================================
        // We have a valid target — movement + abilities
        // =============================================================
        npc->ai_state = NPC_AI_AGGRO;
        npc->ai_target_id = targets[best_target].character_id;

        float tx = targets[best_target].pos_x;
        float ty = targets[best_target].pos_y;
        float target_dist = best_dist;

        // ----- Movement -----
        switch (prof->movement_type) {
            case NPC_MOVE_FOLLOW: {
                float stop_range = 40.0f;
                for (int a = 0; a < prof->ability_count; a++) {
                    if (prof->abilities[a].delivery == NPC_DELIVERY_PROJECTILE &&
                        prof->abilities[a].projectile_speed <= 0.0f) {
                        stop_range = prof->abilities[a].range * 0.8f;
                        break;
                    }
                }
                if (target_dist > stop_range) {
                    float dx = tx - npc->pos_x;
                    float dy = ty - npc->pos_y;
                    float len = sqrtf(dx * dx + dy * dy);
                    if (len > 0.001f) {
                        float move = prof->move_speed * dt;
                        if (move > len - stop_range) move = len - stop_range;
                        if (move > 0.0f) {
                            npc->pos_x += (dx / len) * move;
                            npc->pos_y += (dy / len) * move;
                        }
                    }
                }
                break;
            }
            case NPC_MOVE_MAINTAIN_RANGE: {
                float desired = prof->preferred_range;
                float tolerance = 30.0f;

                if (target_dist < desired - tolerance) {
                    float dx = npc->pos_x - tx;
                    float dy = npc->pos_y - ty;
                    float len = sqrtf(dx * dx + dy * dy);
                    if (len > 0.001f) {
                        float move = prof->move_speed * dt;
                        npc->pos_x += (dx / len) * move;
                        npc->pos_y += (dy / len) * move;
                    }
                } else if (target_dist > desired + tolerance) {
                    float dx = tx - npc->pos_x;
                    float dy = ty - npc->pos_y;
                    float len = sqrtf(dx * dx + dy * dy);
                    if (len > 0.001f) {
                        float move = prof->move_speed * dt;
                        if (move > len - desired) move = len - desired;
                        if (move > 0.0f) {
                            npc->pos_x += (dx / len) * move;
                            npc->pos_y += (dy / len) * move;
                        }
                    }
                }
                break;
            }
            case NPC_MOVE_STATIONARY:
            default:
                break;
        }

        // ----- Fire abilities: collect ready pool, pick randomly -----
        {
            float cur_dist = dist2d(npc->pos_x, npc->pos_y, tx, ty);

            // Build list of ability indices that are off cooldown and in range
            int ready[MAX_NPC_ABILITIES_RT];
            int ready_count = 0;
            for (int a = 0; a < prof->ability_count && a < MAX_NPC_ABILITIES_RT; a++) {
                const NPCAbilityDef* ab = &prof->abilities[a];
                if (cur_dist > ab->range) continue;
                if ((now - npc->ai_ability_cooldowns[a]) < ab->cooldown) continue;
                ready[ready_count++] = a;
            }

            if (ready_count > 0) {
                // Pick a random ability from the ready pool so firing order
                // is never fixed — abilities feel unpredictable but each one
                // is still clearly telegraphed when it fires.
                int a = ready[rand() % ready_count];
                const NPCAbilityDef* ab = &prof->abilities[a];

                // Direction from NPC to target
                float dx = tx - npc->pos_x;
                float dy = ty - npc->pos_y;
                float len = sqrtf(dx * dx + dy * dy);
                float dir_x = (len > 0.001f) ? dx / len : 1.0f;
                float dir_y = (len > 0.001f) ? dy / len : 0.0f;

                // Aim jitter: rotate direction by a small random angle (±8°).
                // The telegraph still points roughly at the player, but not
                // with pixel-perfect precision — standing still is punished,
                // small sidesteps can dodge.
                float jitter = ((float)(rand() % 17) - 8.0f) * (3.14159f / 180.0f);
                float cj = cosf(jitter), sj = sinf(jitter);
                float jdir_x = dir_x * cj - dir_y * sj;
                float jdir_y = dir_x * sj + dir_y * cj;
                dir_x = jdir_x;
                dir_y = jdir_y;

                if (ab->delivery == NPC_DELIVERY_TELEGRAPH) {
                    // ----- Start telegraph cast -----
                    float tele_x, tele_y;
                    if (ab->telegraph_at_target) {
                        tele_x = tx;
                        tele_y = ty;
                    } else {
                        tele_x = npc->pos_x;
                        tele_y = npc->pos_y;
                    }

                    // Lock NPC into casting state
                    npc->ai_state = NPC_AI_CASTING;
                    npc->ai_is_casting = 1;
                    npc->ai_cast_ability_idx = a;
                    npc->ai_cast_start = now;
                    npc->ai_cast_pos_x = tele_x;
                    npc->ai_cast_pos_y = tele_y;
                    npc->ai_cast_dir_x = dir_x;
                    npc->ai_cast_dir_y = dir_y;

                    // Cooldown variance ±20%: next use of this ability won't
                    // land on an exact fixed cadence.
                    float v = (float)(rand() % 40 - 20) / 100.0f; // [-0.2, +0.2)
                    npc->ai_ability_cooldowns[a] = now - ab->cooldown * v;

                    // Queue telegraph start broadcast
                    DeferredAction da = {0};
                    da.type = DSEND_TELEGRAPH_START;
                    da.tstart.npc_id     = npc->id;
                    da.tstart.ability_id = ab->ability_id;
                    da.tstart.shape      = ab->telegraph_shape;
                    da.tstart.pos_x      = tele_x;
                    da.tstart.pos_y      = tele_y;
                    da.tstart.dir_x      = dir_x;
                    da.tstart.dir_y      = dir_y;
                    da.tstart.radius     = ab->telegraph_radius;
                    da.tstart.angle      = ab->telegraph_angle;
                    da.tstart.width      = ab->telegraph_width;
                    da.tstart.length     = ab->telegraph_length;
                    da.tstart.cast_time  = ab->cast_time;
                    da.tstart.npc_x      = npc->pos_x;
                    da.tstart.npc_y      = npc->pos_y;
                    dq_push(&q, &da);

                    printf("[NPC_AI] NPC %u (%s) started telegraph — ability %u, shape=%d, cast=%.1fs\n",
                           npc->id, npc->name, ab->ability_id, ab->telegraph_shape, ab->cast_time);

                } else if (ab->delivery == NPC_DELIVERY_PROJECTILE && ab->projectile_speed > 0.0f) {
                    // ----- Fire projectile -----
                    float v = (float)(rand() % 40 - 20) / 100.0f;
                    npc->ai_ability_cooldowns[a] = now - ab->cooldown * v;

                    DeferredAction da = {0};
                    da.type = DSEND_PROJECTILE_SHOT;
                    da.proj.npc_id     = npc->id;
                    da.proj.ability_id = ab->ability_id;
                    da.proj.origin_x   = npc->pos_x;
                    da.proj.origin_y   = npc->pos_y;
                    da.proj.target_x   = tx;
                    da.proj.target_y   = ty;
                    da.proj.damage     = ab->damage;
                    da.proj.speed      = ab->projectile_speed;
                    da.proj.width      = ab->projectile_width;
                    da.proj.range      = ab->range;
                    dq_push(&q, &da);
                }
            }
        }
    }

    pthread_mutex_unlock(&world->lock);

    // -------------------------------------------------------------------
    // 3. Flush all deferred actions OUTSIDE locks
    // -------------------------------------------------------------------
    dq_flush(&q);
}

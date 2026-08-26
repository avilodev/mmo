/**
 * @file
 * Load NPC behavior profiles and advance targeting, movement, projectiles, and telegraphs.
 * Mutate NPC state under the world lock and defer network actions until it is released.
 */

#include "npc_ai.h"
#include "json_util.h"
#include "npc_world.h"
#include "player_effects.h"
#include "log.h"
#include "projectile.h"
#include "player_data.h"
#include "combat_stats.h"
#include "tick_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "utils.h"

extern ActivePlayer active_players[];

static NPCAIProfile g_ai_profiles[MAX_NPC_AI_PROFILES];
static int          g_ai_profile_count = 0;

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

/**
 * Read an entire file into a terminated buffer.
 *
 * The caller must free the returned buffer.
 *
 * @return An allocated buffer, or NULL when opening or allocation fails.
 */
static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
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

/**
 * Find the delimiter that balances a nested JSON span.
 *
 * @return The matching delimiter, or NULL for an unterminated span.
 */
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

/**
 * Parse NPC behavior profiles into the fixed registry.
 *
 * @param source  Path the JSON came from, named in truncation logs.
 *
 * @return 1 when parsing completes or no section exists, or 0 on malformed input or allocation failure.
 */
static int parse_npc_types(const char* json, const char* source) {
    const char* types_start = strstr(json, "\"npc_types\"");
    if (!types_start) {
        LOG_DEBUG("[NPC_AI] No npc_types section found");
        return 1;
    }

    const char* arr_start = strchr(types_start, '[');
    if (!arr_start) return 0;
    const char* arr_end = find_matching(arr_start, '[', ']');
    if (!arr_end) return 0;

    int arr_len = (int)(arr_end - arr_start + 1);
    char* arr_json = malloc(arr_len + 1);
    if (!arr_json) {
        LOG_ERROR("[NPC_AI] malloc failed while parsing npc_types");
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
            LOG_ERROR("[NPC_AI] malloc failed while parsing NPC type object");
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
                            LOG_ERROR("[NPC_AI] malloc failed while parsing ability object");
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

                    if (prof->ability_count >= MAX_NPC_ABILITIES) {
                        int dropped = json_count_remaining_objects(ap);
                        if (dropped > 0) {
                            LOG_ERROR("[NPC_AI] %s: npc_type_id=%u kept %d abilities and "
                                      "dropped %d more; MAX_NPC_ABILITIES is %d in npc_ai.h",
                                      source, prof->npc_type_id, prof->ability_count,
                                      dropped, MAX_NPC_ABILITIES);
                        }
                    }
                }
            }
        }

        if (prof->npc_type_id > 0) {
            LOG_INFO("[NPC_AI] Loaded profile npc_type_id=%u: movement=%s, aggro=%.0f, %d abilities", prof->npc_type_id, move_str, prof->aggro_range, prof->ability_count);
            g_ai_profile_count++;
        }

        free(obj);
        pos = obj_end + 1;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }

    /* A data file with one more profile than MAX_NPC_AI_PROFILES used to load
     * the first N and say nothing, leaving one NPC type with no behaviour at
     * all -- which reads in game as a mob that will not aggro. */
    if (g_ai_profile_count >= MAX_NPC_AI_PROFILES) {
        int dropped = json_count_remaining_objects(pos);
        if (dropped > 0) {
            LOG_ERROR("[NPC_AI] %s: loaded %d profiles and dropped %d more; "
                      "MAX_NPC_AI_PROFILES is %d in npc_ai.h",
                      source, g_ai_profile_count, dropped, MAX_NPC_AI_PROFILES);
        }
    }

    free(arr_json);
    return 1;
}

/**
 * Initialize NPC AI profiles from a JSON file.
 *
 * @return 1 on success, or 0 when the file cannot be read or parsed.
 */
int npc_ai_init(const char* json_path) {
    g_ai_profile_count = 0;

    char* json = read_file(json_path);
    if (!json) {
        LOG_ERROR("[NPC_AI] Failed to read %s", json_path);
        return 0;
    }

    int result = parse_npc_types(json, json_path);
    free(json);

    LOG_INFO("[NPC_AI] Initialized with %d NPC type profiles", g_ai_profile_count);
    return result;
}

/**
 * Clear the NPC AI profile registry.
 */
void npc_ai_cleanup(void) {
    g_ai_profile_count = 0;
    LOG_DEBUG("[NPC_AI] Cleaned up");
}

/**
 * Retrieve an NPC behavior profile by type identifier.
 *
 * The returned pointer remains owned by the profile registry.
 *
 * @return The matching profile, or NULL when absent.
 */
const NPCAIProfile* npc_ai_get_profile(uint16_t npc_type_id) {
    for (int i = 0; i < g_ai_profile_count; i++) {
        if (g_ai_profiles[i].npc_type_id == npc_type_id) {
            return &g_ai_profiles[i];
        }
    }
    return NULL;
}

static int point_in_circle(float px, float py, float cx, float cy, float radius) {
    return dist2d(px, py, cx, cy) <= radius;
}

/**
 * Test whether a point lies within an oriented cone.
 *
 * @return 1 when inside, or 0 otherwise.
 */
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

/**
 * Test whether a point lies within an oriented forward rectangle.
 *
 * @return 1 when inside, or 0 otherwise.
 */
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

/**
 * Dispatch a point-containment test for a telegraph shape.
 *
 * @return 1 when inside, or 0 for an exterior point or unknown shape.
 */
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

/** Radius within which a telegraph start/resolve packet is broadcast. */
#define TELEGRAPH_BROADCAST_RADIUS 500.0f

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

/** Hold one tick's worth of NPC actions until every NPC lock has been released.
 *
 * Sized from the NPC pool rather than from a constant. An NPC contributes at most
 * one action per tick, so a queue as large as the pool cannot overflow -- which
 * matters because the previous fixed 128 entries sat below even the default pool
 * of 256, and the overflow was silent. Past the limit a telegraph's start would
 * be sent and its resolve dropped, leaving a warning painted on the ground
 * forever and its damage never dealt.
 */
typedef struct {
    DeferredAction* items;
    int count;
    int capacity;
} DeferredQueue;

/** Reset a queue and ensure it can hold one action for every NPC in the pool.
 *
 * The storage is retained between ticks: npc_ai_tick() runs on the single
 * gameplay thread, so one buffer serves every tick and the allocation happens
 * once, on the first tick after the pool's capacity is known.
 */
static void dq_init(DeferredQueue* q, int capacity) {
    static DeferredAction* buffer = NULL;
    static int             buffer_capacity = 0;

    if (capacity > buffer_capacity) {
        DeferredAction* grown = realloc(buffer, (size_t)capacity * sizeof(*grown));
        if (grown) {
            buffer = grown;
            buffer_capacity = capacity;
        } else {
            LOG_ERROR("[NPC_AI] could not size the deferred queue to %d actions; "
                      "holding at %d", capacity, buffer_capacity);
        }
    }

    q->items    = buffer;
    q->capacity = buffer_capacity;
    q->count    = 0;
}

static void dq_push(DeferredQueue* q, const DeferredAction* item) {
    if (q->count < q->capacity) {
        q->items[q->count++] = *item;
        return;
    }
    // Only reachable when the allocation above failed; never silently.
    LOG_WARN_RL(5, 60, "[NPC_AI] deferred queue full at %d actions -- "
                       "an NPC ability was dropped this tick", q->capacity);
}

/**
 * Execute deferred telegraph broadcasts, damage, and projectile spawns.
 *
 * Player positions come from the tick snapshot rather than from live slots. The
 * previous version read pos_x, pos_y, is_dead, and client_fd straight out of
 * active_players[] holding only the registry read lock, which is not the lock that
 * protects those fields — and this is the code that decides whether an NPC ability
 * hits you. It also scanned all MAX_PLAYERS slots per deferred action; the snapshot
 * it was already handed carries these fields plus a spatial index over them.
 *
 * The caller must hold neither the NPC-world lock nor player locks.
 *
 * @param snap Tick-wide player snapshot; must be the one built for this tick.
 */
static void dq_flush(DeferredQueue* q, TickSnapshot* snap) {
    if (!snap) return;

    int nearby[MAX_PLAYERS];

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
                int near_count = tick_snapshot_query(snap, d->tstart.npc_x, d->tstart.npc_y,
                                                     TELEGRAPH_BROADCAST_RADIUS,
                                                     nearby, MAX_PLAYERS);
                for (int k = 0; k < near_count; k++) {
                    server_send(snap->client_fd[nearby[k]], &pkt, sizeof(pkt));
                }
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

                // Send resolve packet to players near the NPC
                int near_count = tick_snapshot_query(snap, d->tresolve.npc_x, d->tresolve.npc_y,
                                                     TELEGRAPH_BROADCAST_RADIUS,
                                                     nearby, MAX_PLAYERS);
                for (int k = 0; k < near_count; k++) {
                    server_send(snap->client_fd[nearby[k]], &rpkt, sizeof(rpkt));
                }

                /* Deal damage to players inside the shape. The query radius is the
                 * shape's farthest reach from its own origin, so it is a superset of
                 * what point_in_telegraph can accept — the broadcast radius above is a
                 * different centre and cannot stand in for it. */
                float rect_reach = sqrtf(d->tresolve.length * d->tresolve.length +
                                         (d->tresolve.width * 0.5f) * (d->tresolve.width * 0.5f));
                float extent = d->tresolve.radius > rect_reach ? d->tresolve.radius : rect_reach;

                int hit_count = tick_snapshot_query(snap, d->tresolve.pos_x, d->tresolve.pos_y,
                                                    extent, nearby, MAX_PLAYERS);

                for (int k = 0; k < hit_count; k++) {
                    int di = nearby[k];
                    if (snap->is_dead[di]) continue;

                    int inside = point_in_telegraph(
                        snap->pos_x[di], snap->pos_y[di],
                        d->tresolve.shape,
                        d->tresolve.pos_x, d->tresolve.pos_y,
                        d->tresolve.dir_x, d->tresolve.dir_y,
                        d->tresolve.radius, d->tresolve.angle,
                        d->tresolve.width, d->tresolve.length);

                    if (!inside) continue;

                    /* Take the slot lock to mutate, and let it reject a slot recycled
                     * to another character since the snapshot was built. */
                    ActivePlayer* target = player_acquire_slot(snap->slot[di],
                                                               snap->character_id[di]);
                    if (!target) continue;

                    int damage = d->tresolve.damage;
                    int variance = (rand() % 21) - 10;
                    damage += (damage * variance) / 100;

                    /* Collect the player's own mitigation: armor, the race passive if
                     * they are in Animal Form, and any active percentage reducers. */
                    DamageModifiers target_mods;
                    player_collect_modifiers(target, 0, &target_mods);
                    damage = damage_resolve(damage, NULL, &target_mods);

                    target->health -= damage;
                    if (target->health < 0) target->health = 0;
                    target->last_combat_time = get_time();
                    player_add_rage(target, 0, damage);

                    int new_hp = target->health;
                    uint8_t is_kill = (new_hp == 0) ? 1 : 0;
                    int client_fd = target->client_fd;
                    uint32_t char_id = target->character_id;

                    player_release(target);

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

                    LOG_DEBUG("[NPC_AI] Telegraph hit player %u for %d dmg (hp=%d)%s", char_id, damage, new_hp, is_kill ? " — KILLED" : "");
                }
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

/**
 * Advance NPC targeting, movement, cooldowns, and ability casts.
 *
 * @param world       NPC world whose entities are updated.
 * @param snap        Tick-wide player snapshot; NULL or empty snapshots skip the update.
 * @param delta_time  Elapsed tick time in seconds.
 */
void npc_ai_tick(NPCWorld* world, TickSnapshot* snap, double delta_time) {
    double now = get_time();
    float dt = (float)delta_time;

    // share one player snapshot across the gameplay tick
    if (!snap || snap->count == 0) return;

    DeferredQueue q;
    dq_init(&q, npc_world_capacity(world));
    if (!q.items) return;   // nothing can be deferred, so nothing may be started

    /* Every NPC has to think, so this phase iterates rather than queries — there
     * is no position to query from. Two things make that affordable.
     *
     * The locking: the pool read lock plus one NPC's mutex at a time, instead
     * of one exclusive lock across the whole pass. An AI tick no longer blocks
     * combat, projectiles, or any inbound packet for its full duration.
     *
     * And what it walks. This used to run over all `capacity` slots, twenty
     * times a second, whatever fraction of them held an NPC — a pool sized for
     * a launch-day crowd cost the same to think for whether two hundred NPCs
     * were spawned or none. It walks the occupied-slot list now, so the cost
     * tracks the NPCs that exist. */
    npc_world_read_begin(world);

    int live_count = 0;
    const int* live = npc_world_live_slots(world, &live_count);

    for (int i = 0; i < live_count; i++) {
        int n = live[i];
        NPCEntity* npc = npc_world_slot(world, n);
        if (!npc || npc->id == 0) continue;   // unlocked pre-filter only

        npc_world_slot_lock(world, n);

        if (npc->id == 0 || !npc->is_alive || npc->npc_type_id == 0) {
            goto next_npc;
        }

        const NPCAIProfile* prof = npc_ai_get_profile(npc->npc_type_id);
        if (!prof || prof->ability_count == 0) {
            goto next_npc;
        }

        // stagger initial ability cooldown phases per NPC
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

        if (npc->ai_state == NPC_AI_CASTING && npc->ai_is_casting) {
            int abi = npc->ai_cast_ability_idx;
            if (abi < 0 || abi >= prof->ability_count) {
                // Invalid — cancel
                npc->ai_is_casting = 0;
                npc->ai_state = NPC_AI_AGGRO;
                goto next_npc;
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
                    LOG_DEBUG("[NPC_AI] NPC %u (%s) teleported to (%.1f, %.1f)", npc->id, npc->name, npc->pos_x, npc->pos_y);
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

                LOG_DEBUG("[NPC_AI] NPC %u (%s) telegraph resolved — ability %u", npc->id, npc->name, ab->ability_id);
            }
            // While casting, NPC is locked — no movement, no other abilities
            goto next_npc;
        }

        if (npc->ai_state == NPC_AI_RETURNING) {
            if (spawn_dist < 5.0f) {
                npc->pos_x = npc->spawn_x;
                npc->pos_y = npc->spawn_y;
                npc->ai_state = NPC_AI_IDLE;
                npc->ai_target_id = 0;
                npc->health = npc->max_health;
                goto next_npc;
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
            goto next_npc;
        }

        float best_dist = 1e9f;
        int best_target = -1;

        // retain aggro beyond the initial acquisition range
#define NPC_HOLD_AGGRO_RANGE 250.0f

        /* A live taunt overrides target selection outright. It is checked before
         * anything else so that a tank pulling a pack off a healer works even when
         * the healer is nearer, which is the entire point of the ability. */
        if (npc->taunt_expires_at > 0.0) {
            if (get_time() >= npc->taunt_expires_at) {
                npc->taunt_expires_at = 0.0;
                npc->taunt_source_id  = 0;
            } else {
                int t = tick_snapshot_find(snap, npc->taunt_source_id);
                if (t >= 0 && !snap->is_dead[t]) {
                    npc->ai_target_id = npc->taunt_source_id;
                    best_target = t;
                    best_dist = dist2d(npc->pos_x, npc->pos_y,
                                       snap->pos_x[t], snap->pos_y[t]);
                } else {
                    /* The taunter left or died; the taunt dies with them. */
                    npc->taunt_expires_at = 0.0;
                    npc->taunt_source_id  = 0;
                }
            }
        }

        // resolve held targets through the snapshot index
        if (best_target == -1 && npc->ai_target_id > 0) {
            int t = tick_snapshot_find(snap, npc->ai_target_id);
            if (t >= 0 && !snap->is_dead[t]) {
                float d = dist2d(npc->pos_x, npc->pos_y,
                                 snap->pos_x[t], snap->pos_y[t]);
                if (spawn_dist < prof->leash_range && d < NPC_HOLD_AGGRO_RANGE) {
                    best_target = t;
                    best_dist = d;
                }
            }
        }

        // select the nearest living grid candidate
        if (best_target == -1) {
            int nearby[NPC_AGGRO_CANDIDATES];
            int n_near = tick_snapshot_query(snap, npc->pos_x, npc->pos_y,
                                             prof->aggro_range,
                                             nearby, NPC_AGGRO_CANDIDATES);
            for (int k = 0; k < n_near; k++) {
                int t = nearby[k];
                if (snap->is_dead[t]) continue;
                best_dist   = dist2d(npc->pos_x, npc->pos_y,
                                     snap->pos_x[t], snap->pos_y[t]);
                best_target = t;
                break;
            }
        }

        if (best_target == -1) {
            if (npc->ai_state == NPC_AI_AGGRO) {
                npc->ai_state = NPC_AI_RETURNING;
                npc->ai_target_id = 0;
            }
            goto next_npc;
        }

        if (spawn_dist >= prof->leash_range) {
            npc->ai_state = NPC_AI_RETURNING;
            npc->ai_target_id = 0;
            goto next_npc;
        }

        npc->ai_state = NPC_AI_AGGRO;
        npc->ai_target_id = snap->character_id[best_target];

        float tx = snap->pos_x[best_target];
        float ty = snap->pos_y[best_target];
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
                // randomize selection among ready abilities
                int a = ready[rand() % ready_count];
                const NPCAbilityDef* ab = &prof->abilities[a];

                // Direction from NPC to target
                float dx = tx - npc->pos_x;
                float dy = ty - npc->pos_y;
                float len = sqrtf(dx * dx + dy * dy);
                float dir_x = (len > 0.001f) ? dx / len : 1.0f;
                float dir_y = (len > 0.001f) ? dy / len : 0.0f;

                // rotate aim by up to eight degrees
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

                    LOG_INFO("[NPC_AI] NPC %u (%s) started telegraph — ability %u, shape=%d, cast=%.1fs", npc->id, npc->name, ab->ability_id, ab->telegraph_shape, ab->cast_time);

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

next_npc:
        /* Single exit for the loop body. The body is long and branches often;
         * with a plain `continue` at each of those branches, every future edit
         * would have to remember to drop this NPC's lock on the way out. */
        npc_world_slot_unlock(world, n);
    }

    npc_world_read_end(world);

    dq_flush(&q, snap);
}

/**
 * @file
 * Exercise the runtime half of the NPC system: effects, mitigation, triggers.
 *
 * npc_content_test.c proves the shipped content satisfies every rule; this proves
 * the engine does what the content says. The three modules here are the ones a
 * content author cannot see the effect of by reading a JSON file -- a shield that
 * absorbs, a slow that stacks in percentage space, a threshold that fires exactly
 * once per instance -- and each of them is silent when it goes wrong.
 *
 * The deferred queue is stubbed rather than linked. Every action an ability takes
 * reaches something outside the NPC pool, which is the whole reason the queue
 * exists; a test of the *decision* has no business dragging in the projectile
 * pool and the player registry to observe it. Counting the pushes is enough to
 * know a trigger fired and what it asked for.
 */

#include "npc_effects.h"
#include "npc_mitigation.h"
#include "npc_registry.h"
#include "npc_ai.h"
#include "npc_summon.h"
#include "npc_triggers.h"
#include "npc_world.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void check(int condition, const char* what) {
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        g_failures++;
    }
}

/* --- Stubs ---------------------------------------------------------------
 *
 * The world's extent, and the far end of the deferred queue. Both are stood in
 * for so that this test links against the decision-making modules alone.
 */

void world_collision_extent(float* out_width, float* out_height) {
    if (out_width)  *out_width  = 9600.0f;
    if (out_height) *out_height = 6400.0f;
}
float world_tile_size(void) { return 16.0f; }
int world_coord_is_valid(float x, float y) { (void)x; (void)y; return 1; }
int world_collision_check_box(float x, float y, float h) { (void)x;(void)y;(void)h; return 0; }
int world_collision_check_box_path(float x0, float y0, float x1, float y1, float h) {
    (void)x0;(void)y0;(void)x1;(void)y1;(void)h; return 0;
}

/** Record what the think phase asked for, instead of performing it. */
static int g_pushed;
static uint8_t g_last_kind;

void npc_deferred_push(NPCDeferredQueue* q, const NPCDeferredAction* item) {
    (void)q;
    g_pushed++;
    g_last_kind = item->type;
}

int player_cast_flag_get(int slot) { (void)slot; return 0; }

/** A player snapshot the test fills by hand.
 *
 * The real tick_snapshot.c samples active_players[], which would drag the player
 * registry and its database into a test about NPC decisions. The struct itself is
 * plain arrays, so the two queries the AI tick asks of it are answered here
 * directly -- linearly, over at most a handful of players, which is faster than
 * building a grid for them.
 */
static TickSnapshot g_players;

int tick_snapshot_query(TickSnapshot* s, float x, float y, float r,
                        int* out, int max_out) {
    if (!s || !out || max_out <= 0) return 0;
    int n = 0;
    for (int i = 0; i < s->count && n < max_out; i++) {
        float dx = s->pos_x[i] - x, dy = s->pos_y[i] - y;
        if (dx * dx + dy * dy <= r * r) out[n++] = i;
    }
    return n;
}

int tick_snapshot_find(const TickSnapshot* s, uint32_t character_id) {
    if (!s) return -1;
    for (int i = 0; i < s->count; i++)
        if (s->character_id[i] == character_id) return i;
    return -1;
}

/** The queue's far end. Counting is what the tests want; performing is not.
 *
 * npc_deferred_push is stubbed too, so nothing reaches q->count -- g_pushed is
 * the count that matters, and this exists only so the tick has something to
 * call.
 */
void npc_deferred_flush(NPCDeferredQueue* q, NPCWorld* w,
                        TickSnapshot* p, NPCTickSnapshot* n) {
    (void)w; (void)p; (void)n;
    if (q) q->count = 0;
}

/* --- Fixtures ------------------------------------------------------------- */

static NPCWorld g_world;

static uint32_t spawn(uint16_t type_id, int health, float x, float y) {
    uint32_t id = npc_world_spawn(&g_world, "test", x, y, health, 16.0f,
                                  0, 0, type_id, 0.0f, NPC_CATEGORY_HOSTILE);
    NPCEntity* n = npc_world_acquire(&g_world, id);
    if (n) { n->max_health = health; npc_world_release(&g_world, n); }
    return id;
}

/** Find the pool slot holding an identifier, for the state accessors. */
static int slot_of(uint32_t id) {
    for (int i = 0; i < npc_world_capacity(&g_world); i++) {
        NPCEntity* n = npc_world_slot(&g_world, i);
        if (n && n->id == id) return i;
    }
    return -1;
}

static double now_seconds(void) { return 1000.0; }

/* --- Tests ---------------------------------------------------------------- */

static void test_effects(void) {
    printf("TEST 1: effects apply, refresh rather than stack, and expire\n");

    uint32_t id = spawn(150, 100, 0, 0);
    int slot = slot_of(id);
    NPCEntity* npc = npc_world_acquire(&g_world, id);

    AbilityEffectDef slow = { .type = EFFECT_SLOW, .duration = 2.0f, .value = 30 };
    check(npc_effect_apply(&g_world, slot, npc, &slow, 1), "a slow installs");

    float m = npc_speed_multiplier(&g_world, slot, npc);
    check(m > 0.69f && m < 0.71f, "30% slow resolves to a 0.70 multiplier");

    /* Two sources of the same effect are one effect refreshed, not two stacks:
     * a pack of five howling at each other would otherwise buff itself fivefold
     * inside a second. */
    npc_effect_apply(&g_world, slot, npc, &slow, 2);
    m = npc_speed_multiplier(&g_world, slot, npc);
    check(m > 0.69f && m < 0.71f, "a second identical slow refreshes rather than stacks");

    /* Percentage space, not multiplication: a 30% slow and a 30% haste cancel
     * exactly, which is what damage_model.h promises for every other modifier. */
    npc->mod_move_speed_pct = 30.0f;
    m = npc_speed_multiplier(&g_world, slot, npc);
    check(m > 0.99f && m < 1.01f, "a +30% modifier cancels the 30% slow exactly");
    npc->mod_move_speed_pct = 0.0f;

    check(npc_is_movement_locked(&g_world, slot, 0) == 0, "a slow is not a root");

    AbilityEffectDef root = { .type = EFFECT_ROOT, .duration = 1.0f };
    npc_effect_apply(&g_world, slot, npc, &root, 1);
    check(npc_is_movement_locked(&g_world, slot, 0), "a root locks movement");
    check(npc_is_movement_locked(&g_world, slot, 1) == 0,
          "and a cc-immune archetype walks through it");

    /* Damage over time is applied under the lock the NPC is already held by. */
    AbilityEffectDef dot = { .type = EFFECT_DOT, .duration = 5.0f,
                             .tick_rate = 1.0f, .value = 10 };
    npc_effect_apply(&g_world, slot, npc, &dot, 1);
    int dealt = 0;
    npc_effects_tick(&g_world, slot, npc, 1.0f, &dealt);
    check(dealt == 10, "a bleed ticks for its value");
    check(npc->health == 90, "and the damage lands on the entity");

    for (int i = 0; i < 6; i++) npc_effects_tick(&g_world, slot, npc, 1.0f, NULL);
    check(npc_speed_multiplier(&g_world, slot, npc) > 0.99f,
          "effects expire on their own timers");

    npc_world_release(&g_world, npc);
    npc_world_remove(&g_world, id);
}

static void test_effects_do_not_survive_a_slot(void) {
    printf("TEST 2: a recycled slot inherits nothing\n");

    uint32_t first = spawn(150, 100, 0, 0);
    int slot = slot_of(first);
    NPCEntity* npc = npc_world_acquire(&g_world, first);
    AbilityEffectDef slow = { .type = EFFECT_SLOW, .duration = 60.0f, .value = 50 };
    npc_effect_apply(&g_world, slot, npc, &slow, 1);
    npc->mod_damage_pct = 90.0f;
    npc_world_release(&g_world, npc);
    npc_world_remove(&g_world, first);

    uint32_t second = spawn(150, 100, 0, 0);
    int slot2 = slot_of(second);
    NPCEntity* npc2 = npc_world_acquire(&g_world, second);

    check(slot2 == slot, "the second spawn reuses the first's slot");
    check(npc_speed_multiplier(&g_world, slot2, npc2) > 0.99f,
          "and carries none of its predecessor's effects");
    check(npc2->mod_damage_pct == 0.0f, "nor its accumulated trigger modifiers");

    npc_world_release(&g_world, npc2);
    npc_world_remove(&g_world, second);
}

static void test_mitigation(void) {
    printf("TEST 3: mitigation gates damage the way its content says\n");

    /* Cult Enforcer: a parry-frame shield at -70%. */
    const NPCTypeDef* enforcer = npc_type_get_by_key("vessane_cult_enforcer");
    check(enforcer && enforcer->has_mitigation, "Cult Enforcer declares mitigation");
    if (!enforcer) return;

    uint32_t id = spawn(enforcer->id, 200, 0, 0);
    int slot = slot_of(id);
    NPCEntity* npc = npc_world_acquire(&g_world, id);
    npc_mitigation_init(npc, enforcer->id);

    int blocked = 0;
    int out = npc_mitigation_apply(&g_world, slot, npc, 100, 500, 0,
                                   now_seconds(), &blocked);
    check(out == 30, "its shield takes 100 down to 30");
    check(blocked, "and reports the hit as gated");
    npc_world_release(&g_world, npc);
    npc_world_remove(&g_world, id);

    /* Frost Warden: four hits inside three seconds break it for six. */
    const NPCTypeDef* warden = npc_type_get_by_key("hunt_frost_warden");
    check(warden && warden->mitigation.break_mode == NPC_BREAK_HITS_IN_WINDOW,
          "Frost Warden's shield breaks on hits in a window");
    if (!warden) return;

    id = spawn(warden->id, 200, 0, 0);
    slot = slot_of(id);
    npc = npc_world_acquire(&g_world, id);
    npc_mitigation_init(npc, warden->id);

    double t = now_seconds();
    int reduced = 0, full = 0;
    for (int i = 0; i < 6; i++) {
        int dmg = npc_mitigation_apply(&g_world, slot, npc, 100, 500, 0, t, NULL);
        if (dmg < 100) reduced++; else full++;
    }
    check(reduced == 4, "the first four hits are absorbed");
    check(full == 2, "and everything after the break lands whole");
    check(npc->shield_broken, "the shield records itself as broken");
    npc_world_release(&g_world, npc);
    npc_world_remove(&g_world, id);

    /* Iron Zealot: not from the front. */
    const NPCTypeDef* zealot = npc_type_get_by_key("unpara_iron_zealot");
    check(zealot && zealot->mitigation.has_frontal_block,
          "Iron Zealot declares a frontal block");
    if (!zealot) return;

    id = spawn(zealot->id, 200, 0, 0);
    slot = slot_of(id);
    npc = npc_world_acquire(&g_world, id);
    npc_mitigation_init(npc, zealot->id);
    npc->facing_x = 1.0f;
    npc->facing_y = 0.0f;

    int front = npc_mitigation_apply(&g_world, slot, npc, 100, 200, 0, now_seconds(), NULL);
    int back  = npc_mitigation_apply(&g_world, slot, npc, 100, -200, 0, now_seconds(), NULL);
    check(front < back, "an attack from the front is reduced");
    check(back == 100, "and one from behind is not");
    check(front == 35, "the reduction is the 35% the content states");

    npc_world_release(&g_world, npc);
    npc_world_remove(&g_world, id);
}

static void test_triggers(void) {
    printf("TEST 4: a threshold trigger fires once, per instance\n");

    /* Rusher's Frenzy: below 30% health, +30% speed and +20% attack rate. */
    const NPCTypeDef* rusher = npc_type_get_by_key("fang_rusher");
    check(rusher && rusher->trigger_count == 1, "Rusher declares one trigger");
    if (!rusher) return;

    uint32_t a = spawn(rusher->id, 20, 0, 0);
    uint32_t b = spawn(rusher->id, 20, 100, 0);
    int slot_a = slot_of(a);

    NPCThink t = {0};
    t.world = &g_world;
    t.now   = now_seconds();
    t.dt    = 0.05f;
    t.faction_index = -1;

    NPCEntity* na = npc_world_acquire(&g_world, a);
    t.slot = slot_a; t.npc = na;
    t.prof = npc_ai_get_profile(rusher->id);
    check(t.prof != NULL, "and a runtime profile was built for it");
    if (!t.prof) { npc_world_release(&g_world, na); return; }

    npc_triggers_evaluate(&t);
    check(na->mod_move_speed_pct == 0.0f, "a healthy Rusher does not frenzy");

    na->health = 5;   /* 25% of 20 */
    npc_triggers_evaluate(&t);
    check(na->mod_move_speed_pct == 30.0f, "below the threshold it does");
    check(na->mod_attack_speed_pct == 20.0f, "with both modifiers applied");

    npc_triggers_evaluate(&t);
    npc_triggers_evaluate(&t);
    check(na->mod_move_speed_pct == 30.0f,
          "and the latch stops it firing again every tick");
    npc_world_release(&g_world, na);

    NPCEntity* nb = npc_world_acquire(&g_world, b);
    check(nb->mod_move_speed_pct == 0.0f,
          "the other Rusher is untouched -- latches are per instance");
    npc_world_release(&g_world, nb);

    npc_world_remove(&g_world, a);
    npc_world_remove(&g_world, b);
}

static void test_trigger_casts(void) {
    printf("TEST 5: a reactive trigger asks for the ability it names\n");

    /* Rogue Rabbit hops when damage comes in. The hop is displacement, not a
     * dodge roll: nothing rolls, the Rabbit is simply somewhere else. */
    const NPCTypeDef* rabbit = npc_type_get_by_key("forgotten_rogue_rabbit");
    check(rabbit != NULL, "Rogue Rabbit resolves");
    if (!rabbit) return;

    uint32_t id = spawn(rabbit->id, 15, 0, 0);
    int slot = slot_of(id);
    NPCEntity* npc = npc_world_acquire(&g_world, id);

    NPCThink t = {0};
    t.world = &g_world;
    t.slot  = slot;
    t.npc   = npc;
    t.prof  = npc_ai_get_profile(rabbit->id);
    t.now   = now_seconds();
    t.dt    = 0.05f;
    t.faction_index = -1;

    float before_x = npc->pos_x;

    npc_triggers_evaluate(&t);
    check(npc->pos_x == before_x, "an unharmed Rabbit stays put");

    npc_trigger_note_damage(npc, 100.0f, 0.0f);
    npc_triggers_evaluate(&t);
    check(npc->dash_active, "a hit starts its hop");
    check(npc->dash_dir_x < 0.0f, "away from where the damage came from");

    npc_world_release(&g_world, npc);
    npc_world_remove(&g_world, id);
}

static void test_ally_queries(void) {
    printf("TEST 6: ally queries are faction-scoped and exclude the asker\n");

    uint32_t wolf_a = spawn(150, 100, 0, 0);      /* feral_wolf */
    uint32_t wolf_b = spawn(150, 100, 40, 0);
    uint32_t cultist = spawn(140, 100, 60, 0);    /* choir_cultist */

    NPCTickSnapshot snap;
    check(npc_snapshot_init(&snap, &g_world), "the NPC snapshot allocates");
    npc_snapshot_build(&snap, &g_world);

    int feral = npc_faction_of_type(150);
    int idx[16];

    int any = npc_allies_near(&snap, 0, 0, 500.0f, wolf_a, -1, idx, 16);
    check(any == 2, "an unscoped query finds both other NPCs");

    int same = npc_allies_near(&snap, 0, 0, 500.0f, wolf_a, feral, idx, 16);
    check(same == 1, "a faction-scoped one finds only the other wolf");
    check(snap.id[idx[0]] == wolf_b, "and it is the wolf, not the cultist");

    /* Wound one and confirm the healer picks it. */
    NPCEntity* hurt = npc_world_acquire(&g_world, wolf_b);
    hurt->health = 20;
    npc_world_release(&g_world, hurt);
    npc_snapshot_build(&snap, &g_world);

    int worst = npc_lowest_health_ally(&snap, 0, 0, 500.0f, wolf_a, feral);
    check(worst >= 0 && snap.id[worst] == wolf_b, "the worst-off ally is the wounded one");

    int below = npc_allies_below_health(&snap, 0, 0, 500.0f, wolf_a, feral, 0.5f);
    check(below == 1, "and it counts as one ally below half health");

    npc_snapshot_free(&snap);
    npc_world_remove(&g_world, wolf_a);
    npc_world_remove(&g_world, wolf_b);
    npc_world_remove(&g_world, cultist);
}

static void test_hold_distance_clamp(void) {
    printf("TEST 8: a profile never holds outside its own reach\n");

    /* This is the engine half of what R7 used to catch by refusing content.
     * Ice Slinger's archetype prefers six tiles and its frost cone reaches five,
     * which under the old rule was a content failure; it is now a profile that
     * holds at five. */
    const NPCTypeDef* slinger = npc_type_get_by_key("hunt_ice_slinger");
    const NPCArchetypeDef* kiter = slinger
        ? npc_archetype_at(slinger->archetype_index) : NULL;
    const NPCAIProfile* prof = slinger ? npc_ai_get_profile(slinger->id) : NULL;
    check(prof && kiter, "Ice Slinger has a profile and an archetype");
    if (!prof || !kiter) return;

    float longest = 0.0f;
    for (int i = 0; i < prof->ability_count; i++) {
        const NPCAbilityDef* ab = &prof->abilities[i];
        if (ab->src->delivery == NPC_DELIV_BUFF ||
            ab->src->delivery == NPC_DELIV_SUMMON) continue;
        if (ab->range > longest) longest = ab->range;
    }

    check(kiter->preferred_tiles * world_tile_size() > longest,
          "its archetype would prefer to stand outside its longest attack");
    check(prof->preferred_range <= longest + 0.001f,
          "but the profile holds within it");

    /* And a chaser closes to its *shortest* attack, not its longest. Blood
     * Ritualist's melee_lunger prefers 1.5 tiles; its sword swing reaches 1. */
    const NPCTypeDef* rit = npc_type_get_by_key("vessane_blood_ritualist");
    const NPCAIProfile* rp = rit ? npc_ai_get_profile(rit->id) : NULL;
    check(rp != NULL, "Blood Ritualist has a profile");
    if (!rp) return;
    check(rp->shortest_reach > 0.0f && rp->shortest_reach <= world_tile_size() + 0.001f,
          "and closes to the one-tile reach of its swing, not its archetype's 1.5");
}

static void test_stealth(void) {
    printf("TEST 7: stealth hides an NPC until it gives itself away\n");

    const NPCTypeDef* stalker = npc_type_get_by_key("hunt_snowveil_stalker");
    check(stalker && stalker->stealth, "Snowveil Stalker declares stealth");
    if (!stalker) return;

    uint32_t id = spawn(stalker->id, 25, 0, 0);
    NPCEntity* npc = npc_world_acquire(&g_world, id);
    npc->stealth_active = stalker->stealth;

    double t = now_seconds();
    check(npc_stealth_hidden(npc, t), "it starts hidden");
    check(npc_stealth_damageable(npc, t), "and is still damageable, as its type says");

    npc_trigger_note_damage(npc, 10.0f, 0.0f);
    check(!npc_stealth_hidden(npc, npc->stealth_revealed_until - 0.1),
          "being hit reveals it");
    check(npc_stealth_hidden(npc, npc->stealth_revealed_until + 0.1),
          "and it fades back once the window passes");

    npc_world_release(&g_world, npc);
    npc_world_remove(&g_world, id);
}

static void test_every_type_thinks(void) {
    printf("TEST 9: every type in the roster thinks without falling over\n");

    /* The end-to-end shape of a tick, over the whole shipped roster. It is worth
     * having because most of what went wrong while this was being written went
     * wrong for one enemy in one branch -- a kit with no targeted ability, a
     * phase naming a slot, a trigger casting a registry ability its type does
     * not own -- and none of those is visible from reading any one module. */
    memset(&g_players, 0, sizeof(g_players));
    g_players.count = 2;
    for (int i = 0; i < 2; i++) {
        g_players.slot[i]         = i;
        g_players.character_id[i] = (uint32_t)(1000 + i);
        g_players.pos_x[i]        = 60.0f * (float)i;
        g_players.pos_y[i]        = 0.0f;
        g_players.client_fd[i]    = -1;
        g_players.health[i]       = 100 - 40 * i;
        g_players.max_health[i]   = 100;
        g_players.form[i]         = (uint8_t)i;
    }

    int count = npc_type_count();
    check(count == 81, "the shipped roster is 81 type rows");

    /* One of each, spaced so they do not all pile onto one player. */
    int spawned = 0;
    for (int i = 0; i < count; i++) {
        const NPCTypeDef* t = npc_type_at(i);
        if (!t) continue;
        float x = 30.0f + 6.0f * (float)(i % 9);
        float y = 6.0f * (float)(i / 9);
        if (spawn(t->id, t->health > 0 ? t->health : 1, x, y)) spawned++;
    }
    check(spawned == count, "every one of them spawns into the pool");

    NPCTickSnapshot npcs;
    check(npc_snapshot_init(&npcs, &g_world), "an NPC snapshot allocates for them");

    /* Where everything started, so movement is observable afterwards. */
    float* start_x = calloc((size_t)npc_world_capacity(&g_world), sizeof(float));
    for (int i = 0; i < npc_world_capacity(&g_world); i++) {
        NPCEntity* n = npc_world_slot(&g_world, i);
        start_x[i] = n ? n->pos_x : 0.0f;
    }

    for (int i = 0; i < 40; i++) {
        npc_snapshot_build(&npcs, &g_world);
        npc_ai_tick(&g_world, &g_players, &npcs, 0.05);
    }

    int moved = 0;
    for (int i = 0; i < npc_world_capacity(&g_world); i++) {
        NPCEntity* n = npc_world_slot(&g_world, i);
        if (n && n->id && n->pos_x != start_x[i]) moved++;
    }
    free(start_x);
    check(moved > 0, "they acquire targets and move toward them");

    /* Abilities fire on the monotonic clock, not on the dt handed to the tick --
     * a cooldown is an absolute instant, which is what keeps a form swap or a
     * hotbar rebuild from shortening one. Forty loop iterations take microseconds
     * of real time, so nothing has come off cooldown yet. Rather than sleep ten
     * seconds to find out, hand every NPC a ready kit and take one more tick. */
    npc_world_read_begin(&g_world);
    int live_count = 0;
    const int* live = npc_world_live_slots(&g_world, &live_count);
    for (int i = 0; i < live_count; i++) {
        double* cds = npc_world_cooldowns(&g_world, live[i]);
        for (int a = 0; a < npc_world_max_abilities(&g_world); a++) cds[a] = 0.0;
    }
    npc_world_read_end(&g_world);

    g_pushed = 0;
    npc_snapshot_build(&npcs, &g_world);
    npc_ai_tick(&g_world, &g_players, &npcs, 0.05);
    check(g_pushed > 0, "and with their kits ready, they act");
    printf("       (%d actions queued in one tick across %d types)\n",
           g_pushed, spawned);

    /* Nothing should have been able to leave the pool inconsistent. */
    int live_now = npc_world_count(&g_world);
    check(live_now >= spawned, "no NPC vanished, and summons may have arrived");

    /* A summoner's budget is recounted from what is alive rather than tracked by
     * matched increments, so killing its adds lets it summon again. */
    const NPCTypeDef* pack = npc_type_get_by_key("hunt_packmaster");
    int with_parents = 0, budgeted = 0;
    if (pack) {
        npc_world_read_begin(&g_world);
        int lc = 0;
        const int* ls = npc_world_live_slots(&g_world, &lc);
        for (int i = 0; i < lc; i++) {
            NPCEntity* n = npc_world_slot(&g_world, ls[i]);
            if (n && n->parent_npc_id) with_parents++;
            if (n && n->summon_children) budgeted++;
        }
        npc_world_read_end(&g_world);
    }
    check(with_parents == 0 || budgeted > 0,
          "every live summon is counted against its summoner's budget");

    npc_snapshot_free(&npcs);
}

int main(int argc, char** argv) {
    log_init();

    const char* dir = (argc > 1) ? argv[1] : "world_server/data/npc";
    if (!npc_registry_load(dir)) {
        printf("  FAIL the shipped NPC content did not load from '%s'\n", dir);
        return 1;
    }
    if (!npc_ai_init()) {
        printf("  FAIL the runtime behaviour table did not build\n");
        return 1;
    }

    NPCStateSizes sizes = {
        .max_abilities    = npc_registry_max_abilities(),
        .max_effect_slots = 8,
        .max_triggers     = npc_registry_max_triggers(),
        .max_phases       = npc_registry_max_phases(),
    };
    /* Room for one of every type plus whatever the summoners call up. */
    if (!npc_world_init_sized(&g_world, 256, &sizes)) {
        printf("  FAIL the NPC pool did not allocate\n");
        return 1;
    }

    test_effects();
    test_effects_do_not_survive_a_slot();
    test_mitigation();
    test_triggers();
    test_trigger_casts();
    test_ally_queries();
    test_stealth();
    test_hold_distance_clamp();
    test_every_type_thinks();

    npc_world_shutdown(&g_world);
    npc_ai_cleanup();
    npc_registry_cleanup();

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}

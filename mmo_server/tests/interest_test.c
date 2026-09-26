/**
 * @file
 * Check that event broadcasts address the players in range and nobody else.
 *
 * The bug this exists to prevent is not a crash. A broadcast that ignores its
 * radius still delivers to everyone who should get it — it just also delivers to
 * everyone who should not, which looks identical in a two-player test and costs
 * a packet per player per event once the world is populated. So the assertions
 * below are as interested in who is *absent* from the recipient list as in who
 * is present.
 */

#include "types.h"
#include "log.h"
#include "interest.h"
#include "player_data.h"
#include "players_database.h"
#include "ability_def.h"
#include "quest_system.h"
#include "utils.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern ActivePlayer active_players[MAX_PLAYERS];

/** Stub successful database initialization. */
int character_database_init(const char* c) { (void)c; return 1; }
/** Stub database shutdown. */
void character_database_close(void) {}
/** Stub successful character persistence. */
int character_update_full_data(const CharacterInfo* d) { (void)d; return 1; }

/** Stub loading empty inventory and equipment arrays. */
int character_items_load(uint32_t c, ItemInstance* inv, int n_inv,
                         ItemInstance* eq, int n_eq) {
    (void)c;
    if (inv) memset(inv, 0, (size_t)n_inv * sizeof(*inv));
    if (eq)  memset(eq,  0, (size_t)n_eq  * sizeof(*eq));
    return 1;
}
/** Stub successful inventory persistence. */
int character_items_save(uint32_t c, const ItemInstance* inv, int n_inv,
                         const ItemInstance* eq, int n_eq) {
    (void)c; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
/** Stub the whole-character save that commits scalars, currency and items together. */
int character_save_all(const CharacterInfo* d, const ItemInstance* inv, int n_inv,
                       const ItemInstance* eq, int n_eq) {
    (void)d; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
/** Stub recording that a character is live in a world. */
int world_session_mark(uint32_t character_id, uint32_t world_id) {
    (void)character_id; (void)world_id; return 1;
}
/** Stub clearing that record. */
void world_session_clear(uint32_t character_id) { (void)character_id; }
/** Stub an empty persisted instance-id range. */
uint64_t character_items_max_instance_id(void) { return 0; }

/** Stub a populated level-one character record. */
int character_get_full_data(uint32_t character_id, CharacterInfo* out) {
    memset(out, 0, sizeof(*out));
    out->character_id = character_id;
    snprintf(out->name, sizeof(out->name), "char%u", character_id);
    out->level = 1; out->health = 100; out->max_health = 100;
    out->resource = 50; out->max_resource = 50;
    return 1;
}

/** Stub class-stat application. */
void player_apply_class_stats(ActivePlayer* p) { (void)p; }
/** Stub the stat recompute. */
void player_recompute_stats(ActivePlayer* p) { (void)p; }
/** Stub the hotbar rebuild. */
void ability_refresh_hotbars(ActivePlayer* p) { (void)p; }
/** Stub equipment-stat application. */
void player_apply_equipment_bonuses(ActivePlayer* p) { (void)p; }
/** Stub an empty class ability list. */
int  ability_get_class_abilities(uint8_t c, uint16_t* o, int m) { (void)c;(void)o;(void)m; return 0; }
/** Stub an absent ability definition. */
const AbilityDef* ability_get(uint16_t id) { (void)id; return NULL; }
/** Stub an empty persisted quest list. */
int  quest_player_load(uint32_t c, PlayerQuestState* s) { (void)c;(void)s; return 0; }
/** Stub successful quest persistence. */
int  quest_player_save(uint32_t c, const PlayerQuestState* s) { (void)c;(void)s; return 1; }
/** Stub quest storage teardown; these tests never allocate any. */
void quest_state_release(PlayerQuestState* s) { (void)s; }
int  quest_state_copy(PlayerQuestState* out, const PlayerQuestState* src) {
    (void)src; if (out) memset(out, 0, sizeof(*out)); return 1;
}
/** Stub the completion set; these tests never record one. */
int  quest_history_add(QuestHistory* h, uint32_t id) { (void)h; (void)id; return 1; }
/** Stub successful packet transmission. */
ssize_t server_send(int fd, void* d, size_t n) { (void)fd;(void)d;(void)n; return (ssize_t)n; }

/** Return the synthetic collision-map extent. */
void world_collision_extent(float* w, float* h) { *w = 8192.0f; *h = 8192.0f; }

/** Activate a character at a fixture position and return its descriptor. */
static int spawn_at(uint32_t character_id, int fd, float x, float y) {
    int slot = -1;
    if (!player_add_active(character_id, fd, &slot)) return -1;
    ActivePlayer* p = player_acquire_slot(slot, character_id);
    assert(p != NULL);
    p->pos_x = x;
    p->pos_y = y;
    player_release(p);
    return fd;
}

/** Report whether a descriptor appears in a recipient list. */
static int contains(const int* fds, int count, int fd) {
    for (int i = 0; i < count; i++) if (fds[i] == fd) return 1;
    return 0;
}

/** Count the players inside a radius the slow, obvious way. */
static int brute_force_count(float x, float y, float r) {
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        float dx = active_players[i].pos_x - x;
        float dy = active_players[i].pos_y - y;
        if (sqrtf(dx * dx + dy * dy) <= r) n++;
    }
    return n;
}

/**
 * Run interest-radius, boundary, capacity, and rejection assertions.
 *
 * @return Zero after all assertions pass.
 */
int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);
    assert(playerdata_init("stub") == 1);

    int fds[MAX_PLAYERS];

    printf("TEST 1: only players inside the radius are addressed\n");
    // A line of players at x = 0, 100, ... 900, all on y = 0.
    enum { N = 10 };
    int fd_of[N];
    for (int i = 0; i < N; i++) {
        fd_of[i] = spawn_at((uint32_t)(700 + i), 40 + i, 100.0f * (float)i, 0.0f);
        assert(fd_of[i] > 0);
    }

    int count = interest_collect_fds(0.0f, 0.0f, 250.0f, fds, MAX_PLAYERS);
    assert(count == 3);                                  // x = 0, 100, 200
    for (int i = 0; i < 3; i++) assert(contains(fds, count, fd_of[i]));
    for (int i = 3; i < N; i++) assert(!contains(fds, count, fd_of[i]));
    printf("  radius 250 over 10 players spaced 100 apart selects exactly 3\n");

    printf("\nTEST 2: the whole world is not the default\n");
    // The regression this file exists for: a broadcast that reached every
    // online player regardless of where the event happened.
    count = interest_collect_fds(100000.0f, 100000.0f, 250.0f, fds, MAX_PLAYERS);
    assert(count == 0);
    printf("  an event nobody is near addresses nobody, not everybody\n");

    printf("\nTEST 3: the boundary is inclusive and agrees with brute force\n");
    const float radii[] = { 0.0f, 99.9f, 100.0f, 100.1f, 450.0f, 1e6f };
    for (unsigned r = 0; r < sizeof(radii) / sizeof(radii[0]); r++) {
        for (int i = 0; i < N; i++) {
            float ox = 100.0f * (float)i;
            count = interest_collect_fds(ox, 0.0f, radii[r], fds, MAX_PLAYERS);
            assert(count == brute_force_count(ox, 0.0f, radii[r]));
        }
    }
    // A player exactly on the circle counts as inside.
    count = interest_collect_fds(0.0f, 0.0f, 100.0f, fds, MAX_PLAYERS);
    assert(count == 2 && contains(fds, count, fd_of[1]));
    printf("  6 radii x %d origins agree exactly; distance == radius is inside\n", N);

    printf("\nTEST 4: the output capacity is respected\n");
    count = interest_collect_fds(450.0f, 0.0f, 1e6f, fds, 4);
    assert(count == 4);
    count = interest_collect_fds(450.0f, 0.0f, 1e6f, fds, 1);
    assert(count == 1);
    printf("  a caller asking for 4 of 10 gets 4; asking for 1 gets 1\n");

    printf("\nTEST 5: a departed player stops receiving events\n");
    assert(player_remove_active_if_fd(700, fd_of[0]) == 1);
    count = interest_collect_fds(0.0f, 0.0f, 250.0f, fds, MAX_PLAYERS);
    assert(count == 2);
    assert(!contains(fds, count, fd_of[0]));
    printf("  count 3 -> 2 after a logout, and the old descriptor is gone\n");

    printf("\nTEST 6: bad input is refused rather than guessed at\n");
    assert(interest_collect_fds(0.0f, 0.0f, 250.0f, NULL, MAX_PLAYERS) == 0);
    assert(interest_collect_fds(0.0f, 0.0f, 250.0f, fds, 0) == 0);
    assert(interest_collect_fds(0.0f, 0.0f, -1.0f, fds, MAX_PLAYERS) == 0);
    assert(interest_collect_fds(NAN, 0.0f, 250.0f, fds, MAX_PLAYERS) == 0);
    assert(interest_collect_fds(0.0f, INFINITY, 250.0f, fds, MAX_PLAYERS) == 0);
    printf("  NULL output, zero capacity, negative and non-finite inputs all return 0\n");

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}

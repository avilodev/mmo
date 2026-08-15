// Concurrency test for the real player_add_active() in world_server/src/player_data.c.
// The database layer is stubbed with a deliberately slow load so we can measure
// whether the global table lock is held across it.

#include "types.h"
#include "log.h"
#include "player_data.h"
#include "players_database.h"
#include "ability_def.h"
#include "quest_system.h"
#include "utils.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOAD_DELAY_US 200000   // 200ms — stands in for a slow Postgres query

extern ActivePlayer active_players[MAX_PLAYERS];
extern pthread_mutex_t active_players_lock;

static atomic_int g_loads_started = 0;

// ---- stubs -----------------------------------------------------------------
int character_database_init(const char* c) { (void)c; return 1; }
void character_database_close(void) {}
int character_update_full_data(const CharacterInfo* d) { (void)d; return 1; }

int character_get_full_data(uint32_t character_id, CharacterInfo* out) {
    atomic_fetch_add(&g_loads_started, 1);
    usleep(LOAD_DELAY_US);                 // the slow part we must not hold locks across
    memset(out, 0, sizeof(*out));
    out->character_id = character_id;
    snprintf(out->name, sizeof(out->name), "char%u", character_id);
    out->level = 1; out->health = 100; out->max_health = 100;
    out->mana = 50; out->max_mana = 50; out->pos_x = 100.0f; out->pos_y = 100.0f;
    return 1;
}

void player_apply_class_stats(ActivePlayer* p) { (void)p; }
void player_apply_equipment_bonuses(ActivePlayer* p) { (void)p; }
int  ability_get_class_abilities(uint8_t c, uint16_t* out, int max) { (void)c; (void)out; (void)max; return 0; }
const AbilityDef* ability_get(uint16_t id) { (void)id; return NULL; }
int  quest_player_load(uint32_t c, PlayerQuestEntry* q, int max) { (void)c; (void)q; (void)max; return 0; }
int  quest_player_save(uint32_t c, const PlayerQuestEntry* q, int n) { (void)c; (void)q; (void)n; return 1; }
ssize_t server_send(int fd, void* d, size_t n) { (void)fd; (void)d; (void)n; return (ssize_t)n; }

// ---- test 1: concurrent logins of distinct characters ----------------------
typedef struct { uint32_t cid; int fd; int result; } LoginArg;

static void* do_login(void* a) {
    LoginArg* arg = (LoginArg*)a;
    arg->result = player_add_active(arg->cid, arg->fd);
    return NULL;
}

// ---- scanner: proves the global lock is free during the load ---------------
static atomic_int g_scan_count = 0;
static atomic_int g_scanner_run = 1;

static void* scanner(void* a) {
    (void)a;
    while (atomic_load(&g_scanner_run)) {
        pthread_mutex_lock(&active_players_lock);
        int n = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) if (active_players[i].is_loaded) n++;
        pthread_mutex_unlock(&active_players_lock);
        (void)n;
        atomic_fetch_add(&g_scan_count, 1);
        usleep(1000);   // 1ms between scans, like a 60Hz broadcast tick
    }
    return NULL;
}

int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);   // keep the test output readable
    assert(playerdata_init("stub") == 1);

    // -------------------------------------------------------------------
    printf("TEST 1: 16 concurrent logins of distinct characters\n");
    enum { N = 16 };
    pthread_t th[N]; LoginArg args[N];
    for (int i = 0; i < N; i++) { args[i].cid = (uint32_t)(1000 + i); args[i].fd = 500 + i; }

    atomic_store(&g_scanner_run, 1);
    atomic_store(&g_scan_count, 0);
    pthread_t scan_th;
    pthread_create(&scan_th, NULL, scanner, NULL);

    for (int i = 0; i < N; i++) pthread_create(&th[i], NULL, do_login, &args[i]);
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);

    atomic_store(&g_scanner_run, 0);
    pthread_join(scan_th, NULL);

    int ok = 0;
    for (int i = 0; i < N; i++) ok += (args[i].result == 1);
    printf("  logins succeeded : %d/%d\n", ok, N);
    assert(ok == N);

    // Every character must occupy exactly one distinct slot.
    int loaded = 0, dupes = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        loaded++;
        for (int j = i + 1; j < MAX_PLAYERS; j++)
            if (active_players[j].is_loaded &&
                active_players[j].character_id == active_players[i].character_id) dupes++;
    }
    printf("  slots occupied   : %d (expect %d)\n", loaded, N);
    printf("  duplicate slots  : %d (expect 0)\n", dupes);
    assert(loaded == N && dupes == 0);

    // The decisive measurement: with a 200ms load, a lock held across it would
    // let the scanner run only a handful of times.
    int scans = atomic_load(&g_scan_count);
    printf("  scanner acquired global lock %d times during the loads\n", scans);
    printf("  -> lock free during DB load: %s\n", scans > 50 ? "YES" : "NO (still serialized)");
    assert(scans > 50);

    // -------------------------------------------------------------------
    printf("\nTEST 2: 8 concurrent logins of the SAME character (reconnect race)\n");
    int before = atomic_load(&g_loads_started);
    enum { M = 8 };
    pthread_t th2[M]; LoginArg args2[M];
    for (int i = 0; i < M; i++) { args2[i].cid = 7777; args2[i].fd = 900 + i; }
    for (int i = 0; i < M; i++) pthread_create(&th2[i], NULL, do_login, &args2[i]);
    for (int i = 0; i < M; i++) pthread_join(th2[i], NULL);

    int slots_for_7777 = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (active_players[i].is_loaded && active_players[i].character_id == 7777) slots_for_7777++;
    int loads = atomic_load(&g_loads_started) - before;

    printf("  slots for char 7777 : %d (expect 1)\n", slots_for_7777);
    printf("  DB loads issued     : %d (expect 1 — reconnects must rebind)\n", loads);
    assert(slots_for_7777 == 1);
    assert(loads == 1);

    // -------------------------------------------------------------------
    printf("\nTEST 3: reserved slot is invisible to gameplay until committed\n");
    int reserved_and_loaded = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (active_players[i].is_reserved && active_players[i].is_loaded) reserved_and_loaded++;
    printf("  slots both reserved and loaded: %d (expect 0)\n", reserved_and_loaded);
    assert(reserved_and_loaded == 0);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}

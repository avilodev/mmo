/**
 * @file
 * Stand-ins for everything the client's network sources call out to.
 *
 * The four packet dispatchers and the connection state machine are what the
 * host tests exercise; the renderer, the audio mixer and the quest log are
 * not, and linking them would drag in OpenGL and the whole game. These are the
 * boundary.
 *
 * Shared by every host test rather than copied into each, so a signature that
 * changes is a compile error in one place.
 */

#include "net_internal.h"
#include "inventory.h"
#include "ability_bar.h"
#include "combat_system.h"
#include "ui/quest_log.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/** The one client network context, which network.c defines in a real build. */
#ifdef NET_STUBS_OWN_CONTEXT
NetContext g_net;
#endif

/** The game the dispatchers write into; each test points this at its own. */
GameState* g_current_game = NULL;

/** GLFW's clock, which network.c reads through net_now(). */
double glfwGetTime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#ifndef NET_STUBS_LINK_NETWORK_C
/* network.c defines these; a test that does not link it needs them here. */
double net_now(void) { return glfwGetTime(); }
void   net_log_player_stats(const char* prefix) { (void)prefix; }

/* The transport shim. Every packet the client sends goes through this rather
 * than through send(), because one socket serves both server links and only the
 * realm one is encrypted -- see net_internal.h.
 *
 * A dispatcher test drives handlers with bytes it chose and never has a live
 * socket, so what a send does here does not matter; what matters is that it is
 * one symbol to stub instead of thirty-six call sites to intercept. */
int net_send(const void* buf, int len) { (void)buf; return len; }
int net_recv(void* buf, int len)       { (void)buf; (void)len; return -1; }
#endif

void ability_bar_on_cast_start(AbilityBarState* bar, uint16_t id, float t) {
    (void)bar; (void)id; (void)t;
}
void ability_bar_on_cast_resolve(AbilityBarState* bar, uint16_t id) { (void)bar; (void)id; }
void ability_bar_on_cast_cancel(AbilityBarState* bar, uint16_t id) { (void)bar; (void)id; }
void ability_bar_on_effect_apply(AbilityBarState* bar, uint8_t type, int value,
                                 float duration, uint32_t source_id) {
    (void)bar; (void)type; (void)value; (void)duration; (void)source_id;
}
void ability_bar_on_effect_remove(AbilityBarState* bar, uint8_t type) { (void)bar; (void)type; }
void ability_bar_on_resource_update(AbilityBarState* bar, int32_t cur, int32_t max) {
    (void)bar; (void)cur; (void)max;
}
void ability_bar_set_form_abilities(AbilityBarState* bar, uint8_t form,
                                    uint16_t* ability_ids, const char** names,
                                    float* cooldowns, float* cast_times,
                                    int* costs, const char** images, int count) {
    (void)bar; (void)form; (void)ability_ids; (void)names; (void)cooldowns;
    (void)cast_times; (void)costs; (void)images; (void)count;
}

void audio_event_death(void) {}
void audio_event_level_up(void) {}
void audio_event_pickup(void) {}

void combat_on_attack_result(CombatState* c, uint8_t code, float cooldown) {
    (void)c; (void)code; (void)cooldown;
}
void combat_on_cast_start(CombatState* c, uint8_t attack_type, float cast_time,
                          float ox, float oy, float ax, float ay,
                          uint32_t* target_ids, uint8_t target_count) {
    (void)c; (void)attack_type; (void)cast_time; (void)ox; (void)oy;
    (void)ax; (void)ay; (void)target_ids; (void)target_count;
}
void combat_on_cast_cancel(CombatState* c) { (void)c; }
void combat_on_damage(CombatState* c, uint32_t target_id, int damage, int is_crit,
                      int is_kill, int is_heal, float tx, float ty) {
    (void)c; (void)target_id; (void)damage; (void)is_crit; (void)is_kill;
    (void)is_heal; (void)tx; (void)ty;
}

/* Stubbed only when the real inventory.c is not linked in.
 *
 * net_dispatch_test links it for real, to exercise the optimistic-move
 * rollback; net_connect_test does not, and would not link without this. A
 * duplicate definition is a link error rather than a silent shadow, so the
 * guard has to be explicit -- define NET_STUBS_REAL_INVENTORY when the real
 * module is on the command line. */
#ifndef NET_STUBS_REAL_INVENTORY
uint16_t inventory_remove_item(InventoryState* inv, int slot, uint16_t qty) {
    (void)inv; (void)slot; return qty;
}
int inventory_revert_move(InventoryState* inv, int from_slot, int to_slot) {
    (void)inv; (void)from_slot; (void)to_slot; return 0;
}
#endif

const char* npc_type_get_name(uint16_t type_id) { (void)type_id; return "stub"; }

/* network_request_player_stats() and network_request_player_data_refresh() are
 * defined in net_combat.c itself and send on g_net.socket, which is -1 here.
 * They are linked as-is rather than stubbed: a send() on a closed descriptor
 * is a failure the handlers must already tolerate. */

void quest_log_add(QuestLogState* ql, uint32_t id, const char* title,
                   uint8_t obj_count, const QuestObjectiveInfo* objectives) {
    (void)ql; (void)id; (void)title; (void)obj_count; (void)objectives;
}
void quest_log_remove(QuestLogState* ql, uint32_t id) { (void)ql; (void)id; }
void quest_log_complete(QuestLogState* ql, uint32_t id) { (void)ql; (void)id; }
void quest_log_update_progress(QuestLogState* ql, uint32_t quest_id, uint8_t obj_index,
                               int32_t current, int32_t required) {
    (void)ql; (void)quest_id; (void)obj_index; (void)current; (void)required;
}


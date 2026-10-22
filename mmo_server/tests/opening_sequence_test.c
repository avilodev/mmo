/**
 * @file
 * Check that the shipped content actually connects end to end.
 *
 * Every link in it is an identifier written in one data file and read in
 * another: a spawn names a dialogue, a dialogue option names a quest, a quest
 * objective names an NPC type or an item, a reward names a coin, and a race
 * condition names a race. Any one of those pointing at nothing produces content
 * that looks completely correct and cannot be finished, which is not something
 * a compiler or a startup log will tell you about -- it is something a player
 * tells you about, which is the most expensive way to find out.
 *
 * So this walks the real data directory and follows every reference, and it
 * does it by linking the real loaders rather than by re-reading the JSON. That
 * is the point: a validator with its own idea of how "repeat" or "quest_active"
 * is spelled drifts from the loader the first time either changes, and then
 * passes on data the server will not accept.
 *
 * Two kinds of check live here. Reference checks -- does this identifier
 * resolve -- and reachability checks, which matter just as much because their
 * failures look like working data: a quest nothing offers is invisible, and a
 * quest nothing takes back can be finished but never handed in.
 *
 * The player and socket calls that quest_system.c makes are stubbed: nothing
 * here grants a reward or sends a packet, and linking the real ones would pull
 * in the database. The item database is real, because rewards point into it.
 */

#include "quest_system.h"
#include "dialogue_system.h"
#include "npc_world.h"
#include "npc_spawns.h"
#include "race_registry.h"
#include "item_instance.h"
#include "item_registry.h"
#include "world_regions.h"
#include "world_motion.h"
#include "world_seats.h"
#include "world_spans.h"
#include "shop.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Stubs --------------------------------------------------------------- */

NPCWorld g_npc_world;

ActivePlayer* player_acquire(uint32_t character_id) { (void)character_id; return NULL; }
void player_release(ActivePlayer* player) { (void)player; }
void player_send_slot_updates(int client_fd, uint32_t character_id,
                              const uint16_t* slot_ids, int count) {
    (void)client_fd; (void)character_id; (void)slot_ids; (void)count;
}
/** The bag is full here: rewards are refused, which is not this file's subject. */
uint16_t inventory_add_tracked(ItemInstance* slots, uint32_t item_id,
                               uint16_t quantity, uint16_t max_stack,
                               uint8_t bind_on_pickup,
                               uint16_t* changed, int max_changed,
                               int* changed_count) {
    (void)slots; (void)item_id; (void)max_stack; (void)bind_on_pickup;
    (void)changed; (void)max_changed;
    if (changed_count) *changed_count = 0;
    return quantity;
}
uint16_t inventory_add(ItemInstance* slots, uint32_t item_id, uint16_t quantity,
                       uint16_t max_stack, uint8_t bind_on_pickup) {
    return inventory_add_tracked(slots, item_id, quantity, max_stack,
                                 bind_on_pickup, NULL, 0, NULL);
}
int inventory_first_free(const ItemInstance* slots) { (void)slots; return -1; }
ssize_t outbox_send(int fd, void* buf, size_t len) { (void)fd; (void)buf; return (ssize_t)len; }

/* --- Helpers ------------------------------------------------------------- */

static int passed = 0;

static void check(int condition, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void check(int condition, const char* fmt, ...) {
    if (!condition) {
        va_list args;
        printf("  FAIL: ");
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
        exit(1);
    }
    passed++;
}

/** Bound the type-id sweep. NPC type ids travel as one byte. */
#define NPC_TYPE_ID_LIMIT 256

/** Bound the identifier sweeps in this test.
 *
 * Not a limit the registry has -- it grows to any identifier. It is how far
 * this test looks for one, and it must stay above the largest id the shipped
 * data uses. */
#define QUEST_ID_LIMIT 4096

/** Record which NPC types the world actually spawns. */
static int g_spawned_type[NPC_TYPE_ID_LIMIT];

/** Record which dialogue each spawned NPC type speaks (the first one found). */
static uint32_t g_type_dialogue[NPC_TYPE_ID_LIMIT];

static int g_spawn_count;

/**
 * Walk the NPC pool and note every type and dialogue it holds.
 */
static void survey_spawns(void) {
    memset(g_spawned_type, 0, sizeof(g_spawned_type));
    memset(g_type_dialogue, 0, sizeof(g_type_dialogue));
    g_spawn_count = 0;

    npc_world_read_begin(&g_npc_world);
    int capacity = npc_world_capacity(&g_npc_world);

    for (int slot = 0; slot < capacity; slot++) {
        NPCEntity* npc = npc_world_slot(&g_npc_world, slot);
        if (!npc || npc->id == 0) continue;

        npc_world_slot_lock(&g_npc_world, slot);
        if (npc->id != 0) {
            g_spawn_count++;
            if (npc->npc_type_id < NPC_TYPE_ID_LIMIT) {
                g_spawned_type[npc->npc_type_id] = 1;
                if (g_type_dialogue[npc->npc_type_id] == 0)
                    g_type_dialogue[npc->npc_type_id] = npc->dialogue_id;
            }

            /* An interactable NPC with no dialogue is a right-click that does
             * nothing, which reads in game as a broken NPC. */
            if (npc->is_interactable) {
                check(npc->dialogue_id != 0,
                      "interactable NPC '%s' has no dialogue", npc->name);
                check(dialogue_get(npc->dialogue_id) != NULL,
                      "NPC '%s' points at dialogue %u, which does not exist",
                      npc->name, npc->dialogue_id);
            }
        }
        npc_world_slot_unlock(&g_npc_world, slot);
    }

    npc_world_read_end(&g_npc_world);
}

/**
 * Follow every quest reference a dialogue makes.
 */
static void test_dialogue_quest_references(void) {
    printf("every quest a dialogue offers or takes back exists\n");

    int references = 0;

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const DialogueDef* d = dialogue_get(id);
        if (!d) continue;

        for (int p = 0; p < d->page_count; p++) {
            const DialoguePageDef* page = &d->pages[p];
            for (int o = 0; o < page->option_count; o++) {
                const DialogueOptionDef* option = &page->options[o];
                if (option->action != DIALOGUE_ACTION_QUEST_ACCEPT &&
                    option->action != DIALOGUE_ACTION_QUEST_TURNIN) continue;

                check(quest_get(option->action_value) != NULL,
                      "dialogue %u page %d option %u names quest %u, which does not exist",
                      id, page->page_num, option->option_id, option->action_value);
                references++;
            }
        }
    }

    check(references > 0, "the data contains at least one quest reference");
}

/**
 * Follow every quest reference a dialogue condition makes.
 */
static void test_condition_quest_references(void) {
    printf("every quest a dialogue condition tests exists\n");

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const DialogueDef* d = dialogue_get(id);
        if (!d) continue;

        for (int p = 0; p < d->page_count; p++) {
            const DialoguePageDef* page = &d->pages[p];
            for (int o = 0; o < page->option_count; o++) {
                const DialogueOptionDef* option = &page->options[o];
                for (int c = 0; c < option->condition_count; c++) {
                    const DialogueCondition* cond = &option->conditions[c];
                    switch ((DialogueConditionKind)cond->kind) {
                        case DIALOGUE_COND_QUEST_NOT_STARTED:
                        case DIALOGUE_COND_QUEST_ACTIVE:
                        case DIALOGUE_COND_QUEST_COMPLETE:
                        case DIALOGUE_COND_QUEST_TURNED_IN:
                            check(quest_get(cond->value) != NULL,
                                  "dialogue %u page %d option %u tests quest %u, "
                                  "which does not exist",
                                  id, page->page_num, option->option_id, cond->value);
                            break;
                        case DIALOGUE_COND_QUEST_STEP: {
                            const QuestDef* stepped = quest_get(cond->value);

                            check(stepped != NULL,
                                  "dialogue %u page %d option %u tests a step of quest "
                                  "%u, which does not exist",
                                  id, page->page_num, option->option_id, cond->value);
                            check(cond->step >= 1 && cond->step <= stepped->step_count,
                                  "dialogue %u page %d option %u tests step %u of quest "
                                  "%u, which has %u",
                                  id, page->page_num, option->option_id, cond->step,
                                  cond->value, stepped->step_count);
                            break;
                        }
                        case DIALOGUE_COND_RACE:
                        case DIALOGUE_COND_NOT_RACE:
                            check(race_get(cond->value) != NULL,
                                  "dialogue %u page %d option %u tests race %u, "
                                  "which does not exist",
                                  id, page->page_num, option->option_id, cond->value);
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
}

/**
 * Confirm every objective has something in the world to advance it.
 */
static uint32_t shop_of(const DialogueDef* d);

/** Whether a shop stocks an item. */
static int shop_stocks(uint32_t shop_id, uint32_t item_id) {
    ShopDef* shop = shop_find(shop_id);

    for (int i = 0; shop && i < shop->item_count; i++)
        if (shop->items[i].item_id == item_id) return 1;

    return 0;
}

/**
 * Follow a buy or sell objective to the merchant it points at.
 *
 * The item has no position, so the objective names an NPC to mark. That NPC has
 * to be spawned and has to run a shop -- and for a purchase the shop has to
 * stock the item, or the objective can never be completed.
 */
static void check_trade_objective(const QuestDef* q, int index, const QuestObjectiveDef* obj) {
    const char* verb = obj->type == QUEST_OBJECTIVE_BUY ? "buys" : "sells";

    check(item_exists(obj->target_id),
          "quest %u '%s' objective %d %s item %u, which is not in items.json",
          q->quest_id, q->title, index, verb, obj->target_id);
    check(obj->marker_npc != 0 && obj->marker_npc < NPC_TYPE_ID_LIMIT &&
          g_spawned_type[obj->marker_npc] != 0,
          "quest %u '%s' objective %d %s an item but names no spawned merchant to mark",
          q->quest_id, q->title, index, verb);

    uint32_t shop_id = shop_of(dialogue_get(g_type_dialogue[obj->marker_npc]));

    check(shop_id != 0, "quest %u '%s' objective %d marks NPC type %u, which opens no shop",
          q->quest_id, q->title, index, obj->marker_npc);
    if (obj->type == QUEST_OBJECTIVE_BUY)
        check(shop_stocks(shop_id, obj->target_id),
              "quest %u '%s' objective %d buys item %u, which shop %u does not stock",
              q->quest_id, q->title, index, obj->target_id, shop_id);
}

static void test_quest_targets_are_spawned(void) {
    printf("every objective points at something that exists in the world\n");

    int checked = 0;

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const QuestDef* q = quest_get(id);
        if (!q) continue;

        check(q->step_count > 0, "quest %u '%s' has no steps", id, q->title);

        for (int step_index = 0; step_index < q->step_count; step_index++) {
        const QuestStepDef* step = quest_step_get(q, step_index);

        check(step->obj_count > 0, "quest %u '%s' step %d has no objectives",
              id, q->title, step_index + 1);
        if (q->step_count > 1)
            check(step->title[0] != '\0', "quest %u '%s' step %d has no name",
                  id, q->title, step_index + 1);

        for (int i = 0; i < step->obj_count; i++) {
            const QuestObjectiveDef* obj = &step->objectives[i];
            check(obj->required_count > 0,
                  "quest %u objective %d requires nothing", id, i);
            check(obj->description[0] != '\0',
                  "quest %u objective %d has no description", id, i);

            /* An objective's target means different things per type, so each
             * type is followed into the file that actually defines it. */
            if (obj->type == QUEST_OBJECTIVE_TALK || obj->type == QUEST_OBJECTIVE_KILL) {
                const char* verb = obj->type == QUEST_OBJECTIVE_TALK
                                 ? "a conversation with" : "something to kill of";

                check(obj->target_id < NPC_TYPE_ID_LIMIT,
                      "quest %u objective %d targets NPC type %u, past the byte the "
                      "wire carries", id, i, obj->target_id);

                /* Spawned, not merely declared: an unspawned type is an
                 * objective that cannot advance, and fill_objective_info()
                 * silently sends no map marker for it either. */
                check(g_spawned_type[obj->target_id] != 0,
                      "quest %u '%s' objective %d wants %s NPC type %u, "
                      "which nothing in spawns.json spawns",
                      id, q->title, i, verb, obj->target_id);
                checked++;
            } else if (obj->type == QUEST_OBJECTIVE_BUY || obj->type == QUEST_OBJECTIVE_SELL) {
                check_trade_objective(q, i, obj);
                checked++;
            } else if (obj->type == QUEST_OBJECTIVE_COLLECT) {
                check(item_exists(obj->target_id),
                      "quest %u '%s' objective %d collects item %u, which is not "
                      "in items.json", id, q->title, i, obj->target_id);
                checked++;
            } else {
                check(0, "quest %u objective %d has type %u, which this build does "
                         "not know how to point at", id, i, obj->type);
            }
        }
        }
    }

    check(checked > 0, "at least one objective target was checked");
}

/**
 * Confirm every quest can be both taken and handed in.
 *
 * Both failures look exactly like working data from the outside. A quest no
 * dialogue offers is authored content no player will ever see. A quest no
 * dialogue takes back can be accepted and completed and then sits in the log
 * forever -- since abandon shipped the player can at least drop it, which is
 * not the same as being able to finish it.
 */
static void test_every_quest_is_reachable_from_a_conversation(void) {
    printf("every quest is offered by some NPC and taken back by some NPC\n");

    static int offered[QUEST_ID_LIMIT];
    static int taken_back[QUEST_ID_LIMIT];
    memset(offered, 0, sizeof(offered));
    memset(taken_back, 0, sizeof(taken_back));

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const DialogueDef* d = dialogue_get(id);
        if (!d) continue;

        for (int p = 0; p < d->page_count; p++) {
            const DialoguePageDef* page = &d->pages[p];
            for (int o = 0; o < page->option_count; o++) {
                const DialogueOptionDef* option = &page->options[o];
                uint32_t named = option->action_value;
                if (named >= QUEST_ID_LIMIT) continue;

                if (option->action == DIALOGUE_ACTION_QUEST_ACCEPT) offered[named] = 1;
                if (option->action == DIALOGUE_ACTION_QUEST_TURNIN) taken_back[named] = 1;
            }
        }
    }

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const QuestDef* q = quest_get(id);
        if (!q) continue;

        check(offered[id],
              "quest %u '%s' exists but no dialogue option offers it; no player "
              "can ever take it", id, q->title);
        check(taken_back[id],
              "quest %u '%s' can be taken but no dialogue option hands it in; a "
              "player who accepts it can finish the objectives and never complete it",
              id, q->title);
    }
}

/**
 * Confirm every page an option leads to exists.
 *
 * A next_page naming a page that is not there is a conversation that stops
 * mid-sentence, and it is invisible until someone picks that exact option.
 */
static void test_dialogue_pages_resolve(void) {
    printf("every option leads to a page that exists, or deliberately closes\n");

    int followed = 0;

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const DialogueDef* d = dialogue_get(id);
        if (!d) continue;

        for (int p = 0; p < d->page_count; p++) {
            const DialoguePageDef* page = &d->pages[p];
            for (int o = 0; o < page->option_count; o++) {
                const DialogueOptionDef* option = &page->options[o];

                if (option->next_page != DIALOGUE_PAGE_CLOSE) {
                    check(dialogue_find_page(d, option->next_page) != NULL,
                          "dialogue %u page %d option %u leads to page %d, which "
                          "does not exist in that conversation",
                          id, page->page_num, option->option_id, option->next_page);
                    followed++;
                }

                /* INHERIT means "reuse next_page", which was just checked. */
                if (option->fail_page != DIALOGUE_PAGE_CLOSE &&
                    option->fail_page != DIALOGUE_PAGE_INHERIT) {
                    check(dialogue_find_page(d, option->fail_page) != NULL,
                          "dialogue %u page %d option %u falls back to page %d, "
                          "which does not exist in that conversation",
                          id, page->page_num, option->option_id, option->fail_page);
                    followed++;
                }

                /* An action that can fail and has nowhere to go leaves the
                 * player looking at an option that silently does nothing. */
                if (option->action == DIALOGUE_ACTION_QUEST_TURNIN)
                    check(option->fail_page != DIALOGUE_PAGE_CLOSE ||
                          option->next_page != DIALOGUE_PAGE_CLOSE,
                          "dialogue %u page %d option %u hands in a quest but "
                          "closes on both paths", id, page->page_num, option->option_id);
            }
        }
    }

    check(followed > 0, "at least one page transition was followed");
}

/**
 * Confirm every reward names something that exists.
 *
 * A reward pointing at a missing item grants nothing and says nothing. A reward
 * naming a coin no kingdom mints pays nothing at all. And a quest gated to a
 * race that does not exist is authored content no character can reach.
 */
static void test_rewards_resolve(void) {
    printf("every reward names an item, a coin and a race that exist\n");

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const QuestDef* q = quest_get(id);
        if (!q) continue;

        check(world_currency_valid(q->currency_id),
              "quest %u '%s' pays in currency %u, which no kingdom mints",
              id, q->title, q->currency_id);

        /* UINT32_MAX is what the loader leaves behind when "require_race" names
         * a race that is not in races.json. The quest loads, and no character
         * alive can ever match it. */
        check(q->require_race_id != UINT32_MAX,
              "quest %u '%s' requires a race that is not in races.json, so no "
              "character can take it", id, q->title);
        if (q->require_race_id != 0)
            check(race_get(q->require_race_id) != NULL,
                  "quest %u '%s' requires race %u, which does not exist",
                  id, q->title, q->require_race_id);

        for (int step_index = 0; step_index < q->step_count; step_index++) {
            const QuestStepDef* step = quest_step_get(q, step_index);

            for (int i = 0; i < step->item_reward_count; i++) {
                const QuestItemReward* reward = &step->item_rewards[i];
                check(reward->item_id != 0,
                      "quest %u '%s' step %d reward %d names item id zero",
                      id, q->title, step_index + 1, i);
                check(item_exists(reward->item_id),
                      "quest %u '%s' rewards item %u, which is not in items.json",
                      id, q->title, reward->item_id);
                check(reward->quantity > 0,
                      "quest %u '%s' rewards zero of item %u",
                      id, q->title, reward->item_id);
            }
        }
    }
}

/**
 * Confirm a repeat declaration says something the loader can act on.
 *
 * Both of these are accepted by the loader with a log line and then behave as
 * once-only, which is the quietest possible way for authored intent to be lost.
 */
static void test_repeat_declarations_are_coherent(void) {
    printf("a cap on turn-ins is only ever set on a quest that repeats\n");

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const QuestDef* q = quest_get(id);
        if (!q) continue;

        check(q->repeat_mode < QUEST_REPEAT_MODE_COUNT,
              "quest %u '%s' has repeat mode %d, which is not a mode",
              id, q->title, (int)q->repeat_mode);

        check(!(q->max_count > 0 && q->repeat_mode == QUEST_REPEAT_ONCE),
              "quest %u '%s' caps itself at %u turn-ins but does not repeat, so "
              "the cap does nothing", id, q->title, q->max_count);
    }
}

/**
 * Report and validate the quest dependency graph.
 *
 * A chain that cannot be finished is invisible until a player walks into it, so
 * the two ways it can be broken are checked here: a prerequisite that names a
 * quest which does not exist, and a cycle, where two quests each wait on the
 * other and neither is ever reachable.
 */
static void test_quest_chain(void) {
    printf("the quest chain is complete, acyclic, and every quest is reachable\n");

    /* Depth-first colouring: 0 unvisited, 1 on the current path, 2 settled. */
    static uint8_t colour[QUEST_ID_LIMIT];
    memset(colour, 0, sizeof(colour));

    int quests_seen = 0;

    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const QuestDef* q = quest_get(id);
        if (!q) continue;
        quests_seen++;

        /* Every prerequisite must name a quest that exists. */
        for (int i = 0; i < q->require_all_count; i++)
            check(quest_get(q->require_all[i]) != NULL,
                  "quest %u '%s' requires quest %u, which does not exist",
                  id, q->title, q->require_all[i]);
        for (int i = 0; i < q->require_any_count; i++)
            check(quest_get(q->require_any[i]) != NULL,
                  "quest %u '%s' accepts quest %u, which does not exist",
                  id, q->title, q->require_any[i]);
    }

    check(quests_seen > 0, "quests are loaded");

    /* Walk each quest's prerequisites, refusing to re-enter the current path. */
    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        if (!quest_get(id) || colour[id] != 0) continue;

        uint32_t stack[QUEST_ID_LIMIT];
        int depth = 0;
        stack[depth++] = id;

        while (depth > 0) {
            uint32_t current = stack[depth - 1];
            const QuestDef* q = quest_get(current);

            if (colour[current] == 0) colour[current] = 1;

            uint32_t next = 0;
            for (int i = 0; i < q->require_all_count && !next; i++)
                if (colour[q->require_all[i]] == 0) next = q->require_all[i];
            for (int i = 0; i < q->require_any_count && !next; i++)
                if (colour[q->require_any[i]] == 0) next = q->require_any[i];

            /* A prerequisite still on the current path closes a loop. */
            for (int i = 0; i < q->require_all_count; i++)
                check(colour[q->require_all[i]] != 1 || q->require_all[i] == current,
                      "quest %u '%s' is in a prerequisite cycle through quest %u",
                      current, q->title, q->require_all[i]);
            for (int i = 0; i < q->require_any_count; i++)
                check(colour[q->require_any[i]] != 1 || q->require_any[i] == current,
                      "quest %u '%s' is in a prerequisite cycle through quest %u",
                      current, q->title, q->require_any[i]);

            if (next) { stack[depth++] = next; continue; }

            colour[current] = 2;
            depth--;
        }
    }

    /* Print the graph, so what unlocks what can be read in one place. */
    printf("\n  quest chain:\n");
    for (uint32_t id = 1; id < QUEST_ID_LIMIT; id++) {
        const QuestDef* q = quest_get(id);
        if (!q) continue;

        printf("    %-5u %-28s", id, q->title);
        if (q->require_race_id) {
            const RaceDef* race = race_get(q->require_race_id);
            printf(" race=%s", race ? race->key : "?");
        }
        if (q->require_level > 0) printf(" level>=%d", q->require_level);
        for (int i = 0; i < q->require_all_count; i++)
            printf(" after=%u", q->require_all[i]);
        if (q->require_any_count > 0) {
            printf(" after any of {");
            for (int i = 0; i < q->require_any_count; i++)
                printf("%s%u", i ? "," : "", q->require_any[i]);
            printf("}");
        }
        printf("\n");
    }
    printf("\n");
}

/** Whether some conversation offers a quest. */
static int quest_is_offered(uint32_t quest_id) {
    for (uint32_t d = 1; d < QUEST_ID_LIMIT; d++) {
        const DialogueDef* dialogue = dialogue_get(d);

        for (int p = 0; dialogue && p < dialogue->page_count; p++)
            for (int o = 0; o < dialogue->pages[p].option_count; o++) {
                const DialogueOptionDef* option = &dialogue->pages[p].options[o];

                if (option->action == DIALOGUE_ACTION_QUEST_ACCEPT &&
                    option->action_value == quest_id)
                    return 1;
            }
    }

    return 0;
}

/**
 * Confirm every playable race can start the opening line.
 *
 * Race-driven rather than id-driven: it fails when a race is made playable
 * and the line is gated away from it, which is the mistake most likely to be
 * made next. A starting quest is one with no prerequisites that some
 * conversation offers.
 */
static void test_every_playable_race_can_start(void) {
    printf("every playable race can be offered a first quest\n");

    int races = race_registry_count();
    check(races > 0, "races are loaded");

    int covered = 0;

    for (int i = 0; i < races; i++) {
        const RaceDef* race = race_at(i);
        if (!race || !race->playable) continue;

        int startable = 0;

        for (uint32_t id = 1; id < QUEST_ID_LIMIT && !startable; id++) {
            const QuestDef* q = quest_get(id);

            if (!q || q->require_all_count > 0 || q->require_any_count > 0) continue;
            if (q->require_race_id != 0 && q->require_race_id != race->id) continue;
            startable = quest_is_offered(id);
        }
        check(startable, "nothing offers race '%s' a first quest", race->key);
        covered++;
    }

    check(covered > 0, "at least one playable race was covered");
}

/**
 * Check the shopkeepers: each sits on a keeper seat, can be talked to, opens a
 * stocked shop, and stands at a height a customer in front of the counter can
 * reach. The spans under a seat read the counter's top, which is out of reach.
 */
#define WORLD_TILES_X 5133
#define WORLD_TILES_Y 2567
#define CUSTOMER_STEP_UNITS 60.0f

// The first option anywhere in the dialogue that opens a shop, or 0.
static uint32_t shop_of(const DialogueDef* d) {
	for(int p = 0; d && p < d->page_count; p++)
		for(int o = 0; o < d->pages[p].option_count; o++)
			if(d->pages[p].options[o].action == DIALOGUE_ACTION_OPEN_SHOP)
				return d->pages[p].options[o].action_value;

	return 0;
}

static void check_keeper(const NPCEntity* npc, int seat_index, int spans_ok) {
	const world_seat* seat = world_seats_get(seat_index);
	const DialogueDef* d = dialogue_get(npc->dialogue_id);
	ShopDef* shop = shop_find(shop_of(d));

	check(npc->is_interactable, "shopkeeper '%s' is not interactable", npc->name);
	check(npc->category == NPC_CATEGORY_PASSIVE, "shopkeeper '%s' is not passive", npc->name);
	check(shop != NULL && shop->item_count > 0,
	      "shopkeeper '%s' opens no stocked shop", npc->name);
	check(npc->pos_z == seat->z - WORLD_SEAT_HEIGHT_UNITS && npc->spawn_z == npc->pos_z,
	      "shopkeeper '%s' height is not its seat's", npc->name);

	if(spans_ok) {
		float floor_z = 0.0f;
		float step_x = seat->x + sinf(seat->heading) * CUSTOMER_STEP_UNITS;
		float step_y = seat->y + cosf(seat->heading) * CUSTOMER_STEP_UNITS;

		check(world_spans_snap(step_x, step_y, 0.0f, &floor_z) &&
		      world_vertical_ok(floor_z, npc->pos_z),
		      "shopkeeper '%s' is out of vertical reach of a customer", npc->name);
	}
}

static void test_shopkeepers(void) {
	int reserved = 0;
	int keepers = 0;
	int spans_ok;

	check(world_seats_load("world_server/data/world_seats.bin"), "seats load");
	world_spans_init(WORLD_TILES_X, WORLD_TILES_Y, 16);
	spans_ok = world_spans_load("world_server/data/world_spans.bin");
	if(!spans_ok)
		printf("no matching spans file; shopkeeper reach is not checked\n");

	// spawned before the seats were known, so respawn them now
	npc_world_shutdown(&g_npc_world);
	check(npc_world_init(&g_npc_world, NPC_CAPACITY_DEFAULT), "NPC pool ready again");
	check(npc_spawns_load("world_server/data/spawns.json", &g_npc_world) > 0, "spawns reload");

	for(int i = 0; i < world_seats_count(); i++)
		reserved += world_seat_is_reserved(world_seats_get(i));

	npc_world_read_begin(&g_npc_world);
	for(int slot = 0; slot < npc_world_capacity(&g_npc_world); slot++) {
		NPCEntity* npc = npc_world_slot(&g_npc_world, slot);
		int seat_index;

		if(!npc || npc->id == 0)
			continue;
		seat_index = world_seats_keeper_near(npc->pos_x, npc->pos_y,
		                                     WORLD_SEAT_KEEPER_MATCH_UNITS);
		if(seat_index < 0)
			continue;
		keepers++;
		check_keeper(npc, seat_index, spans_ok);
	}
	npc_world_read_end(&g_npc_world);

	check(reserved > 0, "the shipped seats hold a keeper seat");
	check(keepers == reserved, "%d keeper seats but %d shopkeepers", reserved, keepers);
	shop_cleanup();
	world_seats_free();
}

int main(void) {
    printf("=== opening sequence content ===\n");

    check(race_registry_init("world_server/data/races.json") > 0, "races load");
    check(dialogue_system_init("world_server/data/dialogues") == 1, "dialogues load");
    check(quest_registry_load("world_server/data/quests.json") == 1, "quests load");
    /* Nonfatal when absent, exactly as the server treats it: an item file with
     * nothing in it is a world with no items, not a broken one. Rewards and
     * collect objectives are checked against whatever it does hold. */
    items_init("world_server/data/items.json");
    check(shop_init("world_server/data/shops.json") == 1, "shops load");

    check(npc_world_init(&g_npc_world, NPC_CAPACITY_DEFAULT), "NPC pool ready");
    check(npc_spawns_load("world_server/data/spawns.json", &g_npc_world) > 0,
          "spawns load");

    survey_spawns();
    printf("%d NPCs spawned from the courtyard data\n", g_spawn_count);

    test_dialogue_quest_references();
    test_condition_quest_references();
    test_dialogue_pages_resolve();
    test_every_quest_is_reachable_from_a_conversation();
    test_quest_targets_are_spawned();
    test_rewards_resolve();
    test_repeat_declarations_are_coherent();
    test_quest_chain();
    test_every_playable_race_can_start();
    test_shopkeepers();

    npc_world_shutdown(&g_npc_world);
    items_cleanup();
    quest_registry_clear();
    dialogue_system_cleanup();
    race_registry_cleanup();

    printf("\n%d checks passed\n", passed);
    return 0;
}

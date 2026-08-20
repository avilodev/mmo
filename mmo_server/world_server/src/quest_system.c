/**
 * @file
 * Load quest definitions, persist player quest state, and update objectives and rewards.
 */

#include "quest_system.h"
#include "log.h"
#include "player_data.h"
#include "items_database.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

static QuestDef* g_quest_table[MAX_QUESTS];  // indexed by quest_id
static int        g_quest_count = 0;

static const char* qs_skip_ws(const char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static const char* qs_find_value(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    return qs_skip_ws(pos + 1);
}

static int qs_parse_string(const char* val, char* out, int out_size) {
    val = qs_skip_ws(val);
    if (*val != '"') return 0;
    val++;
    int i = 0;
    while (*val && *val != '"' && i < out_size - 1) {
        if (*val == '\\' && *(val+1)) { val++; out[i++] = *val++; }
        else out[i++] = *val++;
    }
    out[i] = '\0';
    return 1;
}

static int qs_parse_int(const char* val) {
    return atoi(qs_skip_ws(val));
}

static const char* qs_find_array(const char* json, const char* key) {
    const char* val = qs_find_value(json, key);
    if (!val) return NULL;
    val = qs_skip_ws(val);
    if (*val != '[') return NULL;
    return val + 1;
}

static const char* qs_first_element(const char* pos) {
    pos = qs_skip_ws(pos);
    return (*pos == '{') ? pos : NULL;
}

/**
 * Advance from one object to the next object in a JSON array.
 *
 * @return The next opening brace, or NULL at the array end or for a non-object element.
 */
static const char* qs_next_element(const char* pos) {
    pos = qs_skip_ws(pos);
    if (*pos == '{') {
        int depth = 1; pos++;
        while (*pos && depth > 0) {
            if (*pos == '{') depth++;
            else if (*pos == '}') depth--;
            pos++;
        }
    }
    pos = qs_skip_ws(pos);
    if (*pos == ',') pos = qs_skip_ws(pos + 1);
    if (*pos == ']' || *pos == '\0') return NULL;
    return (*pos == '{') ? pos : NULL;
}

/**
 * Initialize quest definitions from a JSON file.
 *
 * A missing file or quests array is treated as a nonfatal empty quest registry.
 *
 * @return 1 after loading or a nonfatal absence, or 0 on allocation failure.
 */
int quest_system_init(const char* json_path) {
    memset(g_quest_table, 0, sizeof(g_quest_table));
    g_quest_count = 0;

    FILE* f = fopen(json_path, "r");
    if (!f) {
        LOG_ERROR("[QUEST] Cannot open %s: %s", json_path, strerror(errno));
        return 1;  // non-fatal — server can run without quests
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char* buf = malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    const char* quests_arr = qs_find_array(buf, "quests");
    if (!quests_arr) {
        LOG_ERROR("[QUEST] No 'quests' array in JSON");
        free(buf);
        return 1;
    }

    const char* obj = qs_first_element(quests_arr);
    for (; obj != NULL; obj = qs_next_element(obj)) {
        QuestDef* q = calloc(1, sizeof(QuestDef));
        if (!q) break;

        const char* v;
        v = qs_find_value(obj, "quest_id"); if (v) q->quest_id = qs_parse_int(v);
        v = qs_find_value(obj, "title");    if (v) qs_parse_string(v, q->title, sizeof(q->title));
        v = qs_find_value(obj, "xp");       if (v) q->xp_reward   = qs_parse_int(v);
        /* "currency" names the paying kingdom by CurrencyId; quests that omit
         * it pay in Ennara's coin, the starting city's. */
        v = qs_find_value(obj, "currency_reward");
        if (v) q->currency_reward = qs_parse_int(v);
        v = qs_find_value(obj, "currency");
        q->currency_id = (uint8_t)((v && world_currency_valid(qs_parse_int(v)))
                                   ? qs_parse_int(v) : (int)CURRENCY_ENNARA);

        // Objectives array
        const char* objs = qs_find_array(obj, "objectives");
        if (objs) {
            const char* o = qs_first_element(objs);
            for (; o != NULL && q->obj_count < MAX_QUEST_OBJECTIVES; o = qs_next_element(o)) {
                QuestObjectiveDef* od = &q->objectives[q->obj_count];
                char type_str[16] = {0};
                v = qs_find_value(o, "type");  if (v) qs_parse_string(v, type_str, sizeof(type_str));
                od->type = (strcmp(type_str, "collect") == 0) ? QUEST_OBJ_COLLECT :
                           (strcmp(type_str, "talk")    == 0) ? QUEST_OBJ_TALK    :
                                                                QUEST_OBJ_KILL;
                v = qs_find_value(o, "target_id");   if (v) od->target_id      = qs_parse_int(v);
                v = qs_find_value(o, "required");    if (v) od->required_count  = qs_parse_int(v);
                v = qs_find_value(o, "description"); if (v) qs_parse_string(v, od->description, sizeof(od->description));
                q->obj_count++;
            }
        }

        // Item rewards array
        const char* rewards = qs_find_array(obj, "item_rewards");
        if (rewards) {
            const char* r = qs_first_element(rewards);
            for (; r != NULL && q->item_reward_count < MAX_QUEST_OBJECTIVES; r = qs_next_element(r)) {
                QuestItemReward* ir = &q->item_rewards[q->item_reward_count];
                v = qs_find_value(r, "item_id");  if (v) ir->item_id  = qs_parse_int(v);
                v = qs_find_value(r, "quantity"); if (v) ir->quantity  = qs_parse_int(v);
                q->item_reward_count++;
            }
        }

        if (q->quest_id > 0 && q->quest_id < MAX_QUESTS) {
            g_quest_table[q->quest_id] = q;
            g_quest_count++;
            LOG_INFO("[QUEST] Loaded quest %u '%s' (%u objectives)", q->quest_id, q->title, q->obj_count);
        } else {
            free(q);
        }
    }

    free(buf);
    LOG_INFO("[QUEST] %d quests loaded from %s", g_quest_count, json_path);
    return 1;
}

/**
 * Release all loaded quest definitions.
 */
void quest_system_cleanup(void) {
    for (int i = 0; i < MAX_QUESTS; i++) {
        if (g_quest_table[i]) { free(g_quest_table[i]); g_quest_table[i] = NULL; }
    }
}

/**
 * Retrieve a quest definition by identifier.
 *
 * The returned pointer remains owned by the quest registry.
 *
 * @return The definition, or NULL when absent or out of range.
 */
const QuestDef* quest_get(uint32_t quest_id) {
    if (quest_id == 0 || quest_id >= MAX_QUESTS) return NULL;
    return g_quest_table[quest_id];
}

static char g_quest_dir[512] = {0};

/**
 * Set and create the directory used for binary quest-state files.
 */
void quest_system_set_dir(const char* dir) {
    snprintf(g_quest_dir, sizeof(g_quest_dir), "%s", dir);
    // Create directory if it doesn't exist
    mkdir(g_quest_dir, 0755);
}

/**
 * Atomically write a character's quest entries through a temporary file.
 *
 * The file stores native integer and structure representations.
 *
 * @param quests  Array containing count quest entries.
 * @param count   Number of entries; must be between zero and MAX_PLAYER_QUESTS.
 * @return        1 when the temporary file is written and renamed, or 0 on failure.
 */
int quest_player_save(uint32_t character_id, const PlayerQuestEntry* quests, int count) {
    if (g_quest_dir[0] == '\0' || count < 0 || count > MAX_PLAYER_QUESTS) return 0;
    char path[600], temp_path[640];
    snprintf(path, sizeof(path), "%s/%u.bin", g_quest_dir, character_id);
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    FILE* f = fopen(temp_path, "wb");
    if (!f) return 0;
    int ok = fwrite(&count, sizeof(int), 1, f) == 1;
    if (ok && count > 0) {
        ok = fwrite(quests, sizeof(PlayerQuestEntry), (size_t)count, f) == (size_t)count;
    }
    if (ok) ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) ok = rename(temp_path, path) == 0;
    if (!ok) remove(temp_path);
    return ok;
}

/**
 * Load a character's native-format quest-state file.
 *
 * @param quests     Output array with room for max_count entries.
 * @param max_count  Maximum accepted entry count.
 * @return           The loaded count, or 0 when absent, invalid, or unreadable.
 */
int quest_player_load(uint32_t character_id, PlayerQuestEntry* quests, int max_count) {
    if (g_quest_dir[0] == '\0') return 0;
    char path[600];
    snprintf(path, sizeof(path), "%s/%u.bin", g_quest_dir, character_id);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    int count = 0;
    if (fread(&count, sizeof(int), 1, f) != 1 || count < 0 || count > max_count) {
        fclose(f);
        return 0;
    }
    if (count > 0 &&
        fread(quests, sizeof(PlayerQuestEntry), (size_t)count, f) != (size_t)count) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return count;
}

static struct PlayerQuestSlot* find_player_quest(ActivePlayer* p, uint32_t quest_id) {
    for (int i = 0; i < p->quest_count; i++)
        if (p->quests[i].quest_id == quest_id) return &p->quests[i];
    return NULL;
}

/**
 * Send a quest definition and its current objective progress.
 */
static void send_quest_accept_packet(int client_fd, uint32_t character_id,
                                     const QuestDef* q, const PlayerQuestEntry* pq) {
    QuestAcceptPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_QUEST_ACCEPT;
    pkt.header.player_id = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.quest_id = htonl(q->quest_id);
    strncpy(pkt.title, q->title, sizeof(pkt.title) - 1);
    pkt.obj_count = q->obj_count;
    for (int i = 0; i < q->obj_count && i < MAX_QUEST_OBJECTIVES; i++) {
        strncpy(pkt.objectives[i].description,
                q->objectives[i].description,
                sizeof(pkt.objectives[i].description) - 1);
        pkt.objectives[i].required = htonl(q->objectives[i].required_count);
    }
    server_send(client_fd, &pkt, sizeof(pkt));

    // Also send current progress for each objective
    for (int i = 0; i < q->obj_count && i < MAX_QUEST_OBJECTIVES; i++) {
        QuestProgressPacket pp;
        memset(&pp, 0, sizeof(pp));
        pp.header.type = PACKET_QUEST_PROGRESS;
        pp.header.player_id = htonl(character_id);
        pp.header.payload_size = htons(sizeof(pp) - sizeof(PacketHeader));
        pp.quest_id   = htonl(q->quest_id);
        pp.obj_index  = (uint8_t)i;
        pp.current    = htonl(pq->progress[i]);
        pp.required   = htonl(q->objectives[i].required_count);
        server_send(client_fd, &pp, sizeof(pp));
    }
}

/**
 * Add a quest to a character and send its initial state.
 *
 * @return 1 when accepted, or 0 when unavailable, duplicated, or the quest log is full.
 */
int quest_player_accept(uint32_t character_id, int client_fd, uint32_t quest_id) {
    const QuestDef* q = quest_get(quest_id);
    if (!q) {
        LOG_DEBUG("[QUEST] quest_player_accept: unknown quest %u", quest_id);
        return 0;
    }

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    // Already accepted?
    if (find_player_quest(p, quest_id)) {
        player_release(p);
        LOG_WARN_RL(5, 60, "[QUEST] Player %u already has quest %u", character_id, quest_id);
        return 0;
    }

    if (p->quest_count >= MAX_PLAYER_QUESTS) {
        player_release(p);
        return 0;
    }

    struct PlayerQuestSlot* pq = &p->quests[p->quest_count++];
    memset(pq, 0, sizeof(*pq));
    pq->quest_id   = quest_id;
    pq->is_active  = 1;
    p->is_dirty    = 1;

    // Copy before sending so we can release lock first
    struct PlayerQuestSlot pq_copy = *pq;
    player_release(p);

    send_quest_accept_packet(client_fd, character_id, q, (const PlayerQuestEntry*)&pq_copy);
    LOG_DEBUG("[QUEST] Player %u accepted quest %u '%s'", character_id, quest_id, q->title);
    return 1;
}

/**
 * Complete an eligible quest and grant its configured rewards.
 *
 * @return 1 when turned in, or 0 when absent, inactive, or incomplete.
 */
int quest_player_turnin(uint32_t character_id, int client_fd, uint32_t quest_id) {
    const QuestDef* q = quest_get(quest_id);
    if (!q) return 0;

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    struct PlayerQuestSlot* pq = find_player_quest(p, quest_id);
    if (!pq || !pq->is_active || !pq->is_complete) {
        player_release(p);
        return 0;  // caller uses fail_page
    }

    // Mark done and give rewards
    pq->is_active   = 0;
    pq->is_complete = 0;  // remove from active tracking
    p->is_dirty     = 1;

    // XP
    if (q->xp_reward > 0) {
        p->experience += q->xp_reward;
    }
    // Coin, in the kingdom currency the quest pays
    if (q->currency_reward > 0) {
        world_currency_credit(p->currency, q->currency_id, q->currency_reward);
    }

    // Item rewards — find empty inventory slots
    QuestCompletePacket cpkt;
    memset(&cpkt, 0, sizeof(cpkt));
    cpkt.header.type = PACKET_QUEST_COMPLETE;
    cpkt.header.player_id = htonl(character_id);
    cpkt.header.payload_size = htons(sizeof(cpkt) - sizeof(PacketHeader));
    cpkt.quest_id    = htonl(quest_id);
    cpkt.xp_reward       = htonl(q->xp_reward);
    cpkt.currency_reward = htonl(q->currency_reward);
    cpkt.currency_id     = q->currency_id;

    for (int i = 0; i < q->item_reward_count && i < MAX_QUEST_OBJECTIVES; i++) {
        uint32_t reward_id = q->item_rewards[i].item_id;
        uint16_t want      = q->item_rewards[i].quantity ? q->item_rewards[i].quantity : 1;

        const ItemDefinition* def = item_get(reward_id);
        int slot = inventory_first_free(p->inventory);

        // retain unplaced reward quantities for warning output
        uint16_t left   = inventory_add(p->inventory, reward_id, want,
                                        def ? def->max_stack : 1,
                                        def ? def->bind_on_pickup : 0);
        uint16_t stored = (uint16_t)(want - left);

        if (stored > 0) {
            cpkt.items[cpkt.item_count].item_id        = htonl(reward_id);
            cpkt.items[cpkt.item_count].quantity       = (uint8_t)stored;
            cpkt.items[cpkt.item_count].inventory_slot = (uint8_t)(slot < 0 ? 0 : slot);
            cpkt.item_count++;
        }
        if (left > 0)
            LOG_WARN("[QUEST] character %u had no room for %u x item %u",
                     character_id, left, reward_id);
    }

    player_release(p);

    server_send(client_fd, &cpkt, sizeof(cpkt));

    // report current slots after stack merges
    if (cpkt.item_count > 0) {
        uint16_t changed[MAX_SLOT_UPDATES];
        int n = 0;
        for (int i = 0; i < cpkt.item_count && n < MAX_SLOT_UPDATES; i++)
            changed[n++] = cpkt.items[i].inventory_slot;
        player_send_slot_updates(client_fd, character_id, changed, n);
    }

    LOG_DEBUG("[QUEST] Player %u completed quest %u '%s'", character_id, quest_id, q->title);
    return 1;
}

/**
 * Advance matching kill objectives for a character.
 */
void quest_on_npc_kill(uint32_t character_id, int client_fd, uint16_t npc_type_id) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    for (int qi = 0; qi < p->quest_count; qi++) {
        struct PlayerQuestSlot* pq = &p->quests[qi];
        if (!pq->is_active || pq->is_complete) continue;

        const QuestDef* q = quest_get(pq->quest_id);
        if (!q) continue;

        int any_progress = 0;
        for (int oi = 0; oi < q->obj_count; oi++) {
            if (q->objectives[oi].type != QUEST_OBJ_KILL) continue;
            if (q->objectives[oi].target_id != (uint32_t)npc_type_id) continue;
            if (pq->progress[oi] >= q->objectives[oi].required_count) continue;

            pq->progress[oi]++;
            any_progress = 1;

            // Capture for packet (release lock before send)
            int32_t cur = pq->progress[oi];
            int32_t req = q->objectives[oi].required_count;
            uint32_t qid = pq->quest_id;
            uint8_t  oidx = (uint8_t)oi;

            // Check if all objectives complete
            int all_done = 1;
            for (int k = 0; k < q->obj_count; k++) {
                if (pq->progress[k] < q->objectives[k].required_count) { all_done = 0; break; }
            }
            if (all_done) pq->is_complete = 1;

            p->is_dirty = 1;
            player_release(p);

            // Send progress packet
            QuestProgressPacket pp;
            memset(&pp, 0, sizeof(pp));
            pp.header.type = PACKET_QUEST_PROGRESS;
            pp.header.player_id = htonl(character_id);
            pp.header.payload_size = htons(sizeof(pp) - sizeof(PacketHeader));
            pp.quest_id  = htonl(qid);
            pp.obj_index = oidx;
            pp.current   = htonl(cur);
            pp.required  = htonl(req);
            server_send(client_fd, &pp, sizeof(pp));

            p = player_acquire(character_id);
            if (!p) return;
        }
        (void)any_progress;
    }

    player_release(p);
}

/**
 * Advance matching item-collection objectives for a character.
 */
void quest_on_item_collect(uint32_t character_id, int client_fd, uint32_t item_id) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    for (int qi = 0; qi < p->quest_count; qi++) {
        struct PlayerQuestSlot* pq = &p->quests[qi];
        if (!pq->is_active || pq->is_complete) continue;

        const QuestDef* q = quest_get(pq->quest_id);
        if (!q) continue;

        for (int oi = 0; oi < q->obj_count; oi++) {
            if (q->objectives[oi].type != QUEST_OBJ_COLLECT) continue;
            if (q->objectives[oi].target_id != item_id) continue;
            if (pq->progress[oi] >= q->objectives[oi].required_count) continue;

            pq->progress[oi]++;

            int32_t  cur  = pq->progress[oi];
            int32_t  req  = q->objectives[oi].required_count;
            uint32_t qid  = pq->quest_id;
            uint8_t  oidx = (uint8_t)oi;

            int all_done = 1;
            for (int k = 0; k < q->obj_count; k++) {
                if (pq->progress[k] < q->objectives[k].required_count) { all_done = 0; break; }
            }
            if (all_done) pq->is_complete = 1;

            p->is_dirty = 1;
            player_release(p);

            QuestProgressPacket pp;
            memset(&pp, 0, sizeof(pp));
            pp.header.type = PACKET_QUEST_PROGRESS;
            pp.header.player_id = htonl(character_id);
            pp.header.payload_size = htons(sizeof(pp) - sizeof(PacketHeader));
            pp.quest_id  = htonl(qid);
            pp.obj_index = oidx;
            pp.current   = htonl(cur);
            pp.required  = htonl(req);
            server_send(client_fd, &pp, sizeof(pp));

            p = player_acquire(character_id);
            if (!p) return;
        }
    }

    player_release(p);
}

/**
 * Advance matching NPC-conversation objectives for a character.
 */
void quest_on_npc_talk(uint32_t character_id, int client_fd, uint16_t npc_type_id) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    for (int qi = 0; qi < p->quest_count; qi++) {
        struct PlayerQuestSlot* pq = &p->quests[qi];
        if (!pq->is_active || pq->is_complete) continue;

        const QuestDef* q = quest_get(pq->quest_id);
        if (!q) continue;

        for (int oi = 0; oi < q->obj_count; oi++) {
            if (q->objectives[oi].type != QUEST_OBJ_TALK) continue;
            if (q->objectives[oi].target_id != (uint32_t)npc_type_id) continue;
            if (pq->progress[oi] >= q->objectives[oi].required_count) continue;

            pq->progress[oi]++;

            int32_t  cur  = pq->progress[oi];
            int32_t  req  = q->objectives[oi].required_count;
            uint32_t qid  = pq->quest_id;
            uint8_t  oidx = (uint8_t)oi;

            int all_done = 1;
            for (int k = 0; k < q->obj_count; k++) {
                if (pq->progress[k] < q->objectives[k].required_count) { all_done = 0; break; }
            }
            if (all_done) pq->is_complete = 1;

            p->is_dirty = 1;
            player_release(p);

            QuestProgressPacket pp;
            memset(&pp, 0, sizeof(pp));
            pp.header.type = PACKET_QUEST_PROGRESS;
            pp.header.player_id = htonl(character_id);
            pp.header.payload_size = htons(sizeof(pp) - sizeof(PacketHeader));
            pp.quest_id  = htonl(qid);
            pp.obj_index = oidx;
            pp.current   = htonl(cur);
            pp.required  = htonl(req);
            server_send(client_fd, &pp, sizeof(pp));

            p = player_acquire(character_id);
            if (!p) return;
        }
    }

    player_release(p);
}

/**
 * Send every active quest and its progress to a character's client.
 */
void quest_send_all(uint32_t character_id, int client_fd) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    // Copy quest state to send after releasing lock
    PlayerQuestEntry local_quests[MAX_PLAYER_QUESTS];
    int count = p->quest_count;
    memcpy(local_quests, p->quests, count * sizeof(PlayerQuestEntry));
    player_release(p);

    for (int i = 0; i < count; i++) {
        if (!local_quests[i].is_active) continue;
        const QuestDef* q = quest_get(local_quests[i].quest_id);
        if (!q) continue;
        send_quest_accept_packet(client_fd, character_id, q, &local_quests[i]);
    }
}

// ============================================================================
// quest_system.c — Server-side quest tracking
// ============================================================================

#include "quest_system.h"
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

// ---------------------------------------------------------------------------
// Quest table (loaded from quests.json)
// ---------------------------------------------------------------------------

static QuestDef* g_quest_table[MAX_QUESTS];  // indexed by quest_id
static int        g_quest_count = 0;

// ---------------------------------------------------------------------------
// Simple JSON helpers (same pattern as dialogue_loader.c)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

int quest_system_init(const char* json_path) {
    memset(g_quest_table, 0, sizeof(g_quest_table));
    g_quest_count = 0;

    FILE* f = fopen(json_path, "r");
    if (!f) {
        fprintf(stderr, "[QUEST] Cannot open %s: %s\n", json_path, strerror(errno));
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
        fprintf(stderr, "[QUEST] No 'quests' array in JSON\n");
        free(buf);
        return 1;
    }

    const char* obj = quests_arr;
    while ((obj = qs_next_element(obj)) != NULL) {
        QuestDef* q = calloc(1, sizeof(QuestDef));
        if (!q) break;

        const char* v;
        v = qs_find_value(obj, "quest_id"); if (v) q->quest_id = qs_parse_int(v);
        v = qs_find_value(obj, "title");    if (v) qs_parse_string(v, q->title, sizeof(q->title));
        v = qs_find_value(obj, "xp");       if (v) q->xp_reward   = qs_parse_int(v);
        v = qs_find_value(obj, "gold");     if (v) q->gold_reward  = qs_parse_int(v);

        // Objectives array
        const char* objs = qs_find_array(obj, "objectives");
        if (objs) {
            const char* o = objs;
            while ((o = qs_next_element(o)) != NULL && q->obj_count < MAX_QUEST_OBJECTIVES) {
                QuestObjectiveDef* od = &q->objectives[q->obj_count];
                char type_str[16] = {0};
                v = qs_find_value(o, "type");  if (v) qs_parse_string(v, type_str, sizeof(type_str));
                od->type = (strcmp(type_str, "collect") == 0) ? QUEST_OBJ_COLLECT : QUEST_OBJ_KILL;
                v = qs_find_value(o, "target_id");   if (v) od->target_id      = qs_parse_int(v);
                v = qs_find_value(o, "required");    if (v) od->required_count  = qs_parse_int(v);
                v = qs_find_value(o, "description"); if (v) qs_parse_string(v, od->description, sizeof(od->description));
                q->obj_count++;
            }
        }

        // Item rewards array
        const char* rewards = qs_find_array(obj, "item_rewards");
        if (rewards) {
            const char* r = rewards;
            while ((r = qs_next_element(r)) != NULL && q->item_reward_count < MAX_QUEST_OBJECTIVES) {
                QuestItemReward* ir = &q->item_rewards[q->item_reward_count];
                v = qs_find_value(r, "item_id");  if (v) ir->item_id  = qs_parse_int(v);
                v = qs_find_value(r, "quantity"); if (v) ir->quantity  = qs_parse_int(v);
                q->item_reward_count++;
            }
        }

        if (q->quest_id > 0 && q->quest_id < MAX_QUESTS) {
            g_quest_table[q->quest_id] = q;
            g_quest_count++;
            printf("[QUEST] Loaded quest %u '%s' (%u objectives)\n",
                   q->quest_id, q->title, q->obj_count);
        } else {
            free(q);
        }
    }

    free(buf);
    printf("[QUEST] %d quests loaded from %s\n", g_quest_count, json_path);
    return 1;
}

void quest_system_cleanup(void) {
    for (int i = 0; i < MAX_QUESTS; i++) {
        if (g_quest_table[i]) { free(g_quest_table[i]); g_quest_table[i] = NULL; }
    }
}

const QuestDef* quest_get(uint32_t quest_id) {
    if (quest_id == 0 || quest_id >= MAX_QUESTS) return NULL;
    return g_quest_table[quest_id];
}

// ---------------------------------------------------------------------------
// File-based persistence  (<exe_dir>/data/quests/<character_id>.bin)
// ---------------------------------------------------------------------------

static char g_quest_dir[512] = {0};

void quest_system_set_dir(const char* dir) {
    snprintf(g_quest_dir, sizeof(g_quest_dir), "%s", dir);
    // Create directory if it doesn't exist
    mkdir(g_quest_dir, 0755);
}

void quest_player_save(uint32_t character_id, const PlayerQuestEntry* quests, int count) {
    if (g_quest_dir[0] == '\0') return;
    char path[600];
    snprintf(path, sizeof(path), "%s/%u.bin", g_quest_dir, character_id);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(&count, sizeof(int), 1, f);
    fwrite(quests, sizeof(PlayerQuestEntry), count, f);
    fclose(f);
}

int quest_player_load(uint32_t character_id, PlayerQuestEntry* quests, int max_count) {
    if (g_quest_dir[0] == '\0') return 0;
    char path[600];
    snprintf(path, sizeof(path), "%s/%u.bin", g_quest_dir, character_id);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    int count = 0;
    fread(&count, sizeof(int), 1, f);
    if (count > max_count) count = max_count;
    fread(quests, sizeof(PlayerQuestEntry), count, f);
    fclose(f);
    return count;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static struct PlayerQuestSlot* find_player_quest(ActivePlayer* p, uint32_t quest_id) {
    for (int i = 0; i < p->quest_count; i++)
        if (p->quests[i].quest_id == quest_id) return &p->quests[i];
    return NULL;
}

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

// ---------------------------------------------------------------------------
// Player operations
// ---------------------------------------------------------------------------

int quest_player_accept(uint32_t character_id, int client_fd, uint32_t quest_id) {
    const QuestDef* q = quest_get(quest_id);
    if (!q) {
        printf("[QUEST] quest_player_accept: unknown quest %u\n", quest_id);
        return 0;
    }

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    // Already accepted?
    if (find_player_quest(p, quest_id)) {
        player_release(p);
        printf("[QUEST] Player %u already has quest %u\n", character_id, quest_id);
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
    printf("[QUEST] Player %u accepted quest %u '%s'\n", character_id, quest_id, q->title);
    return 1;
}

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
    // Gold
    if (q->gold_reward > 0) {
        p->gold += q->gold_reward;
    }

    // Item rewards — find empty inventory slots
    QuestCompletePacket cpkt;
    memset(&cpkt, 0, sizeof(cpkt));
    cpkt.header.type = PACKET_QUEST_COMPLETE;
    cpkt.header.player_id = htonl(character_id);
    cpkt.header.payload_size = htons(sizeof(cpkt) - sizeof(PacketHeader));
    cpkt.quest_id    = htonl(quest_id);
    cpkt.xp_reward   = htonl(q->xp_reward);
    cpkt.gold_reward = htonl(q->gold_reward);

    for (int i = 0; i < q->item_reward_count && i < MAX_QUEST_OBJECTIVES; i++) {
        // Find empty slot
        int slot = -1;
        for (int s = 0; s < 150; s++) {
            if (p->inventory[s] == 0) { slot = s; break; }
        }
        if (slot >= 0) {
            p->inventory[slot] = q->item_rewards[i].item_id;
            cpkt.items[cpkt.item_count].item_id       = htonl(q->item_rewards[i].item_id);
            cpkt.items[cpkt.item_count].quantity      = q->item_rewards[i].quantity;
            cpkt.items[cpkt.item_count].inventory_slot = (uint8_t)slot;
            cpkt.item_count++;
        }
    }

    player_release(p);

    server_send(client_fd, &cpkt, sizeof(cpkt));
    printf("[QUEST] Player %u completed quest %u '%s'\n", character_id, quest_id, q->title);
    return 1;
}

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

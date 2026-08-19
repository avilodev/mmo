/**
 * @file
 * Validate and dispatch client packets for NPC dialogue interactions.
 */

#include "dialogue_handler.h"
#include "dialogue_system.h"
#include "combat.h"
#include "player_data.h"
#include "quest_system.h"
#include "shop.h"
#include "headers.h"
#include "types.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "utils.h"

extern NPCWorld g_npc_world;

static float dist2d(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    return sqrtf(dx * dx + dy * dy);
}

/** Snapshot the NPC fields required after releasing the world lock. */
typedef struct {
    uint32_t id;
    float    pos_x, pos_y;
    uint32_t dialogue_id;
    uint8_t  is_interactable;
    uint16_t npc_type_id;
    char     name[32];
} NPCSnapshot;

/**
 * Snapshot an alive NPC while holding the world lock.
 *
 * @param out  Receives the copied NPC fields.
 * @return     1 when found and alive, or 0 otherwise.
 */
static int find_npc_snapshot(uint32_t npc_id, NPCSnapshot* out) {
    pthread_mutex_lock(&g_npc_world.lock);

    for (int i = 0; i < MAX_NPCS; i++) {
        const NPCEntity* npc = &g_npc_world.npcs[i];
        if (npc->id == npc_id && npc->is_alive) {
            out->id              = npc->id;
            out->pos_x           = npc->pos_x;
            out->pos_y           = npc->pos_y;
            out->dialogue_id     = npc->dialogue_id;
            out->is_interactable = npc->is_interactable;
            out->npc_type_id     = npc->npc_type_id;
            strncpy(out->name, npc->name, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = '\0';
            pthread_mutex_unlock(&g_npc_world.lock);
            return 1;
        }
    }

    pthread_mutex_unlock(&g_npc_world.lock);
    return 0;
}

static void send_packet(int client_fd, void* packet, size_t size) {
    server_send(client_fd, packet, size);
}

/**
 * Validate an NPC interaction request and open its initial dialogue page.
 *
 * @param client_fd     Socket that receives the dialogue response.
 * @param character_id Character initiating the interaction.
 * @param buffer        Packet buffer containing NPCInteractRequestPacket.
 * @param bytes         Available packet bytes; negative values are rejected.
 */
void handle_npc_interact_request(int client_fd, uint32_t character_id,
                                 uint8_t* buffer, ssize_t bytes) {
    if (bytes < 0 || (size_t)bytes < sizeof(NPCInteractRequestPacket)) {
        printf("[DIALOGUE] Malformed NPC interact request (size: %zd)\n", bytes);
        return;
    }

    NPCInteractRequestPacket* pkt = (NPCInteractRequestPacket*)buffer;
    uint32_t npc_id = ntohl(pkt->npc_id);

    printf("[DIALOGUE] Player %u interacting with NPC %u\n", character_id, npc_id);

    // Find player — snapshot position under lock
    float player_x = 0, player_y = 0;
    {
        ActivePlayer* player = player_acquire(character_id);
        if (!player) {
            printf("[DIALOGUE] Player %u not found\n", character_id);
            return;
        }
        player_x = player->pos_x;
        player_y = player->pos_y;
        player_release(player);
    }

    // Snapshot NPC data under world lock — safe to use after the call
    NPCSnapshot npc;
    if (!find_npc_snapshot(npc_id, &npc)) {
        printf("[DIALOGUE] NPC %u not found\n", npc_id);
        return;
    }

    // Validate NPC has dialogue
    if (npc.dialogue_id == 0 || !npc.is_interactable) {
        printf("[DIALOGUE] NPC %u is not interactable\n", npc_id);
        return;
    }

    // Calculate distance
    float distance = dist2d(player_x, player_y, npc.pos_x, npc.pos_y);
    if (distance > 100.0f) {
        printf("[DIALOGUE] Player %u too far from NPC %u (%.1f units)\n",
               character_id, npc_id, distance);
        return;
    }

    // Get dialogue definition
    const DialogueDef* dialogue = dialogue_get(npc.dialogue_id);
    if (!dialogue) {
        printf("[DIALOGUE] Dialogue %u not found\n", npc.dialogue_id);
        return;
    }

    if (dialogue->page_count == 0) {
        printf("[DIALOGUE] Dialogue %u has no pages\n", npc.dialogue_id);
        return;
    }

    // Create dialogue session
    DialogueSession* session = dialogue_session_create(character_id, npc_id, npc.dialogue_id);
    if (!session) {
        printf("[DIALOGUE] Failed to create session\n");
        return;
    }

    // Notify quest system that the player talked to this NPC type
    quest_on_npc_talk(character_id, client_fd, npc.npc_type_id);

    // Build response packet with page 0
    // Client will look up text from local dialogues.json
    const DialoguePageDef* page = &dialogue->pages[0];

    NPCInteractResponsePacket response;
    memset(&response, 0, sizeof(response));

    response.header.type = PACKET_NPC_INTERACT_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));

    response.npc_id = htonl(npc_id);
    response.dialogue_id = htonl(npc.dialogue_id);
    response.page_num = 0;
    response.option_count = page->option_count;

    strncpy(response.npc_name, npc.name, sizeof(response.npc_name) - 1);

    // Send only option IDs - client looks up text locally
    for (int i = 0; i < page->option_count && i < MAX_DIALOGUE_OPTIONS; i++) {
        response.option_ids[i] = page->options[i].option_id;
    }

    send_packet(client_fd, &response, sizeof(response));

    printf("[DIALOGUE] Sent dialogue response to player %u (dialogue %u, page 0)\n",
           character_id, npc.dialogue_id);
}

/**
 * Validate a dialogue option selection and execute its configured action.
 *
 * @param client_fd     Socket that receives navigation or close packets.
 * @param character_id Character selecting the option.
 * @param buffer        Packet buffer containing DialogueOptionSelectPacket.
 * @param bytes         Available packet bytes; negative values are rejected.
 */
void handle_dialogue_option_select(int client_fd, uint32_t character_id,
                                   uint8_t* buffer, ssize_t bytes) {
    if (bytes < 0 || (size_t)bytes < sizeof(DialogueOptionSelectPacket)) {
        printf("[DIALOGUE] Malformed option select packet (size: %zd)\n", bytes);
        return;
    }

    DialogueOptionSelectPacket* pkt = (DialogueOptionSelectPacket*)buffer;
    uint32_t npc_id = ntohl(pkt->npc_id);
    uint32_t dialogue_id = ntohl(pkt->dialogue_id);
    uint8_t current_page = pkt->current_page;
    uint8_t option_selected = pkt->option_selected;

    printf("[DIALOGUE] Player %u selected option %u on page %u\n",
           character_id, option_selected, current_page);

    // Get active session
    DialogueSession* session = dialogue_session_get(character_id);
    if (!session) {
        printf("[DIALOGUE] No active session for player %u\n", character_id);
        return;
    }

    // Validate session matches packet
    if (session->npc_id != npc_id || session->dialogue_id != dialogue_id ||
        session->current_page != current_page) {
        printf("[DIALOGUE] Session mismatch for player %u\n", character_id);
        return;
    }

    // Find player and NPC for distance check — snapshot position under lock
    float player_x = 0, player_y = 0;
    {
        ActivePlayer* player = player_acquire(character_id);
        if (!player) {
            dialogue_session_close(character_id);
            return;
        }
        player_x = player->pos_x;
        player_y = player->pos_y;
        player_release(player);
    }

    NPCSnapshot npc;
    if (!find_npc_snapshot(npc_id, &npc)) {
        dialogue_session_close(character_id);
        return;
    }

    // Re-check distance (anti-exploit)
    float distance = dist2d(player_x, player_y, npc.pos_x, npc.pos_y);
    if (distance > 100.0f) {
        printf("[DIALOGUE] Player %u moved too far from NPC %u\n", character_id, npc_id);
        dialogue_session_close(character_id);

        // Send close packet
        DialogueClosePacket close_pkt;
        close_pkt.header.type = PACKET_DIALOGUE_CLOSE;
        close_pkt.header.player_id = htonl(character_id);
        close_pkt.header.payload_size = htons(sizeof(close_pkt) - sizeof(PacketHeader));
        close_pkt.npc_id = htonl(npc_id);
        send_packet(client_fd, &close_pkt, sizeof(close_pkt));
        return;
    }

    // Get dialogue and current page
    const DialogueDef* dialogue = dialogue_get(dialogue_id);
    if (!dialogue || current_page >= dialogue->page_count) {
        printf("[DIALOGUE] Invalid dialogue or page\n");
        dialogue_session_close(character_id);
        return;
    }

    const DialoguePageDef* page = &dialogue->pages[current_page];

    // Validate option index
    if (option_selected >= page->option_count) {
        printf("[DIALOGUE] Invalid option %u (max %u)\n", option_selected, page->option_count);
        return;
    }

    const DialogueOptionDef* option = &page->options[option_selected];
    int8_t next_page = option->next_page;

    // Dispatch action before page navigation
    if (option->action == DIALOGUE_ACTION_OPEN_SHOP) {
        shop_open(character_id, client_fd, option->action_value);
        // Close dialogue after opening shop
        dialogue_session_close(character_id);
        DialogueClosePacket close_pkt;
        close_pkt.header.type = PACKET_DIALOGUE_CLOSE;
        close_pkt.header.player_id = htonl(character_id);
        close_pkt.header.payload_size = htons(sizeof(close_pkt) - sizeof(PacketHeader));
        close_pkt.npc_id = htonl(npc_id);
        send_packet(client_fd, &close_pkt, sizeof(close_pkt));
        return;
    } else if (option->action == DIALOGUE_ACTION_QUEST_ACCEPT) {
        quest_player_accept(character_id, client_fd, option->action_value);
        // Continue to next_page normally (NPC can respond "Quest accepted!")
    } else if (option->action == DIALOGUE_ACTION_QUEST_TURNIN) {
        int turned_in = quest_player_turnin(character_id, client_fd, option->action_value);
        if (!turned_in) {
            // Quest not complete — use fail_page if set
            int8_t fp = option->fail_page;
            if (fp != -2) next_page = fp;
        }
    }

    // If next_page == -1, close dialogue
    if (next_page == -1) {
        printf("[DIALOGUE] Closing dialogue for player %u\n", character_id);
        dialogue_session_close(character_id);

        DialogueClosePacket close_pkt;
        close_pkt.header.type = PACKET_DIALOGUE_CLOSE;
        close_pkt.header.player_id = htonl(character_id);
        close_pkt.header.payload_size = htons(sizeof(close_pkt) - sizeof(PacketHeader));
        close_pkt.npc_id = htonl(npc_id);
        send_packet(client_fd, &close_pkt, sizeof(close_pkt));
        return;
    }

    // Validate next_page
    if (next_page < 0 || next_page >= dialogue->page_count) {
        printf("[DIALOGUE] Invalid next_page %d\n", next_page);
        dialogue_session_close(character_id);
        return;
    }

    // Update session to new page
    dialogue_session_update_page(character_id, next_page);

    // Send dialogue update with new page
    // Client will look up text from local dialogues.json
    const DialoguePageDef* new_page = &dialogue->pages[next_page];

    DialogueUpdatePacket update;
    memset(&update, 0, sizeof(update));

    update.header.type = PACKET_DIALOGUE_UPDATE;
    update.header.player_id = htonl(character_id);
    update.header.payload_size = htons(sizeof(update) - sizeof(PacketHeader));

    update.npc_id = htonl(npc_id);
    update.dialogue_id = htonl(dialogue_id);
    update.page_num = next_page;
    update.option_count = new_page->option_count;

    // Send only option IDs - client looks up text locally
    for (int i = 0; i < new_page->option_count && i < MAX_DIALOGUE_OPTIONS; i++) {
        update.option_ids[i] = new_page->options[i].option_id;
    }

    send_packet(client_fd, &update, sizeof(update));

    printf("[DIALOGUE] Sent page %d to player %u\n", next_page, character_id);
}

/**
 * Close a character's dialogue session.
 */
void handle_dialogue_close(uint32_t character_id) {
    printf("[DIALOGUE] Closing dialogue for player %u\n", character_id);
    dialogue_session_close(character_id);
}

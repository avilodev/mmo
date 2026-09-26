/**
 * @file
 * Validate and dispatch client packets for NPC dialogue interactions.
 *
 * Two rules shape everything here. Every inbound packet is re-validated against
 * the world rather than trusted -- distance is rechecked on each choice, and the
 * chosen option is rechecked against its conditions, because the option list a
 * client holds was true when it was sent and need not be true now. And the
 * client identifies its choice by option_id, never by row: the server hides
 * options whose conditions fail, so row numbers differ between two players
 * reading the same page.
 */

#include "dialogue_handler.h"
#include "npc_world.h"
#include "str_fixed.h"
#include "dialogue_system.h"
#include "combat.h"
#include "player_data.h"
#include "quest_system.h"
#include "shop.h"
#include "headers.h"
#include "types.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "utils.h"

extern NPCWorld g_npc_world;

/** Refuse interaction past this range, in world pixels. */
#define DIALOGUE_INTERACT_RANGE 100.0f

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
 * Snapshot an alive NPC while holding its slot lock.
 *
 * @param out  Receives the copied NPC fields.
 * @return     1 when found and alive, or 0 otherwise.
 */
static int find_npc_snapshot(uint32_t npc_id, NPCSnapshot* out) {
    NPCEntity* npc = npc_world_acquire(&g_npc_world, npc_id);
    if (!npc) return 0;

    if (!npc->is_alive) {
        npc_world_release(&g_npc_world, npc);
        return 0;
    }

    out->id              = npc->id;
    out->pos_x           = npc->pos_x;
    out->pos_y           = npc->pos_y;
    out->dialogue_id     = npc->dialogue_id;
    out->is_interactable = npc->is_interactable;
    out->npc_type_id     = npc->npc_type_id;
    STR_COPY_FIELD(out->name, npc->name);

    npc_world_release(&g_npc_world, npc);
    return 1;
}

/**
 * Read a character's position.
 *
 * @return 1 when the character is loaded, or 0 otherwise.
 */
static int player_position(uint32_t character_id, float* out_x, float* out_y) {
    ActivePlayer* player = player_acquire(character_id);
    if (!player) return 0;

    *out_x = player->pos_x;
    *out_y = player->pos_y;
    player_release(player);
    return 1;
}

/**
 * Tell the client a conversation has ended, and forget it server-side.
 */
static void end_conversation(int client_fd, uint32_t character_id, uint32_t npc_id) {
    dialogue_session_close(character_id);

    DialogueClosePacket close_pkt;
    memset(&close_pkt, 0, sizeof(close_pkt));
    close_pkt.header.type         = PACKET_DIALOGUE_CLOSE;
    close_pkt.header.player_id    = htonl(character_id);
    close_pkt.header.payload_size = htons(sizeof(close_pkt) - sizeof(PacketHeader));
    close_pkt.npc_id              = htonl(npc_id);
    server_send(client_fd, &close_pkt, sizeof(close_pkt));
}

/**
 * Report whether a page number survives the wire's byte-wide page field.
 *
 * Numbering pages beyond this is an authoring mistake rather than a limit worth
 * routing around: the packet field is one byte, so page 256 has no way to reach
 * the client at all.
 */
static int page_fits_on_wire(int32_t page_num, uint32_t dialogue_id) {
    if (page_num >= 0 && page_num <= 255) return 1;
    LOG_ERROR("[DIALOGUE] dialogue %u names page %d, which does not fit the "
              "packet's one-byte page number (0..255)", dialogue_id, page_num);
    return 0;
}

/**
 * Send one page of a conversation, filtered to the options this character may pick.
 *
 * @param opening  Nonzero to send the opening response, which carries the NPC's name.
 */
static void send_page(int client_fd, uint32_t character_id, const NPCSnapshot* npc,
                      uint32_t dialogue_id, const DialoguePageDef* page, int opening) {
    uint8_t option_ids[MAX_DIALOGUE_OPTIONS];
    int option_count = dialogue_page_visible_options(page, character_id,
                                                     option_ids, MAX_DIALOGUE_OPTIONS);

    /* Warn once here rather than letting the player read a sentence that stops
     * mid-word: the packet field is fixed, and prose is easy to overrun. */
    if (strlen(page->text) >= MAX_DIALOGUE_TEXT)
        LOG_ERROR("[DIALOGUE] dialogue %u page %d is %zu bytes and will be cut to %d",
                  dialogue_id, page->page_num, strlen(page->text), MAX_DIALOGUE_TEXT - 1);

    /* Both packets carry the same page; only the opening one names the NPC. */
    char  text[MAX_DIALOGUE_TEXT];
    char  option_text[MAX_DIALOGUE_OPTIONS][MAX_OPTION_TEXT];
    memset(text, 0, sizeof(text));
    memset(option_text, 0, sizeof(option_text));

    snprintf(text, sizeof(text), "%s", page->text);
    for (int i = 0; i < option_count; i++) {
        const DialogueOptionDef* option = dialogue_page_find_option(page, option_ids[i]);
        snprintf(option_text[i], MAX_OPTION_TEXT, "%s", option ? option->text : "");
    }

    if (opening) {
        NPCInteractResponsePacket response;
        memset(&response, 0, sizeof(response));
        response.header.type         = PACKET_NPC_INTERACT_RESPONSE;
        response.header.player_id    = htonl(character_id);
        response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
        response.npc_id              = htonl(npc->id);
        response.dialogue_id         = htonl(dialogue_id);
        response.page_num            = (uint8_t)page->page_num;
        response.option_count        = (uint8_t)option_count;
        STR_COPY_FIELD(response.npc_name, npc->name);
        memcpy(response.option_ids, option_ids, (size_t)option_count);
        memcpy(response.text, text, sizeof(response.text));
        memcpy(response.option_text, option_text, sizeof(response.option_text));
        server_send(client_fd, &response, sizeof(response));
    } else {
        DialogueUpdatePacket update;
        memset(&update, 0, sizeof(update));
        update.header.type         = PACKET_DIALOGUE_UPDATE;
        update.header.player_id    = htonl(character_id);
        update.header.payload_size = htons(sizeof(update) - sizeof(PacketHeader));
        update.npc_id              = htonl(npc->id);
        update.dialogue_id         = htonl(dialogue_id);
        update.page_num            = (uint8_t)page->page_num;
        update.option_count        = (uint8_t)option_count;
        memcpy(update.option_ids, option_ids, (size_t)option_count);
        memcpy(update.text, text, sizeof(update.text));
        memcpy(update.option_text, option_text, sizeof(update.option_text));
        server_send(client_fd, &update, sizeof(update));
    }
}

/**
 * Validate an NPC interaction request and open its first page.
 *
 * @param client_fd     Socket that receives the dialogue response.
 * @param character_id  Character initiating the interaction.
 * @param buffer        Packet buffer containing NPCInteractRequestPacket.
 * @param bytes         Available packet bytes; negative values are rejected.
 */
void handle_npc_interact_request(int client_fd, uint32_t character_id,
                                 uint8_t* buffer, ssize_t bytes) {
    if (bytes < 0 || (size_t)bytes < sizeof(NPCInteractRequestPacket)) {
        /* Size is whatever the peer chose to send, so this is one line per
         * bad packet until the limiter kicks them -- rate limited so a single
         * client cannot roll the world's log history out of the ring. */
        LOG_WARN_RL(10, 60, "[DIALOGUE] Malformed NPC interact request (size: %zd)", bytes);
        return;
    }

    NPCInteractRequestPacket* pkt = (NPCInteractRequestPacket*)buffer;
    uint32_t npc_id = ntohl(pkt->npc_id);

    float player_x = 0.0f, player_y = 0.0f;
    if (!player_position(character_id, &player_x, &player_y)) {
        LOG_DEBUG("[DIALOGUE] Character %u not loaded", character_id);
        return;
    }

    NPCSnapshot npc;
    if (!find_npc_snapshot(npc_id, &npc)) {
        LOG_DEBUG("[DIALOGUE] NPC %u not found", npc_id);
        return;
    }

    if (npc.dialogue_id == 0 || !npc.is_interactable) {
        LOG_DEBUG("[DIALOGUE] NPC %u is not interactable", npc_id);
        return;
    }

    if (dist2d(player_x, player_y, npc.pos_x, npc.pos_y) > DIALOGUE_INTERACT_RANGE) {
        LOG_DEBUG("[DIALOGUE] Character %u is too far from NPC %u", character_id, npc_id);
        return;
    }

    const DialogueDef* dialogue = dialogue_get(npc.dialogue_id);
    if (!dialogue || dialogue->page_count == 0) {
        LOG_ERROR("[DIALOGUE] NPC '%s' points at dialogue %u, which has no pages",
                  npc.name, npc.dialogue_id);
        return;
    }

    /* The opening page is the first one authored, whatever it is numbered. */
    const DialoguePageDef* page = &dialogue->pages[0];
    if (!page_fits_on_wire(page->page_num, npc.dialogue_id)) return;

    if (!dialogue_session_open(character_id, npc_id, npc.dialogue_id)) return;
    dialogue_session_update_page(character_id, page->page_num);

    /* Talking is itself a quest objective, so it advances before the page goes
     * out: the page the player then reads can be gated on the new state. */
    quest_on_npc_talk(character_id, client_fd, npc.npc_type_id);

    send_page(client_fd, character_id, &npc, npc.dialogue_id, page, 1);
}

/**
 * Validate a dialogue choice and execute its configured action.
 *
 * @param client_fd     Socket that receives navigation or close packets.
 * @param character_id  Character selecting the option.
 * @param buffer        Packet buffer containing DialogueOptionSelectPacket.
 * @param bytes         Available packet bytes; negative values are rejected.
 */
void handle_dialogue_option_select(int client_fd, uint32_t character_id,
                                   uint8_t* buffer, ssize_t bytes) {
    if (bytes < 0 || (size_t)bytes < sizeof(DialogueOptionSelectPacket)) {
        LOG_WARN_RL(10, 60, "[DIALOGUE] Malformed option select packet (size: %zd)", bytes);
        return;
    }

    DialogueOptionSelectPacket* pkt = (DialogueOptionSelectPacket*)buffer;
    uint32_t npc_id       = ntohl(pkt->npc_id);
    uint32_t dialogue_id  = ntohl(pkt->dialogue_id);
    uint8_t  current_page = pkt->current_page;
    uint8_t  option_id    = pkt->option_id;

    DialogueSession session;
    if (!dialogue_session_snapshot(character_id, &session)) {
        LOG_DEBUG("[DIALOGUE] No conversation open for character %u", character_id);
        return;
    }

    if (session.npc_id != npc_id || session.dialogue_id != dialogue_id ||
        session.current_page != (int32_t)current_page) {
        LOG_DEBUG("[DIALOGUE] Choice does not match the open conversation for character %u",
                  character_id);
        return;
    }

    float player_x = 0.0f, player_y = 0.0f;
    if (!player_position(character_id, &player_x, &player_y)) {
        dialogue_session_close(character_id);
        return;
    }

    NPCSnapshot npc;
    if (!find_npc_snapshot(npc_id, &npc)) {
        dialogue_session_close(character_id);
        return;
    }

    if (dist2d(player_x, player_y, npc.pos_x, npc.pos_y) > DIALOGUE_INTERACT_RANGE) {
        LOG_DEBUG("[DIALOGUE] Character %u walked away from NPC %u", character_id, npc_id);
        end_conversation(client_fd, character_id, npc_id);
        return;
    }

    const DialogueDef* dialogue = dialogue_get(dialogue_id);
    const DialoguePageDef* page = dialogue_find_page(dialogue, (int32_t)current_page);
    if (!page) {
        LOG_ERROR("[DIALOGUE] dialogue %u has no page %u", dialogue_id, current_page);
        dialogue_session_close(character_id);
        return;
    }

    const DialogueOptionDef* option = dialogue_page_find_option(page, option_id);
    if (!option) {
        LOG_WARN_RL(10, 60,
                    "[DIALOGUE] Character %u chose option %u, which page %u does not define",
                    character_id, option_id, current_page);
        return;
    }

    /* Re-checked rather than assumed: the option list the client holds was
     * filtered when it was sent, and the state behind it can have moved since. */
    if (!dialogue_option_available(option, character_id)) {
        LOG_WARN_RL(10, 60,
                    "[DIALOGUE] Character %u chose option %u, which they no longer qualify for",
                    character_id, option_id);
        return;
    }

    int32_t next_page = option->next_page;

    switch (option->action) {
        case DIALOGUE_ACTION_OPEN_SHOP:
            shop_open(character_id, client_fd, option->action_value,
                      npc_id, npc.pos_x, npc.pos_y);
            end_conversation(client_fd, character_id, npc_id);
            return;

        case DIALOGUE_ACTION_QUEST_ACCEPT:
            quest_player_accept(character_id, client_fd, option->action_value);
            break;

        case DIALOGUE_ACTION_QUEST_TURNIN:
            if (!quest_player_turnin(character_id, client_fd, option->action_value) &&
                option->fail_page != DIALOGUE_PAGE_INHERIT) {
                next_page = option->fail_page;
            }
            break;

        default:
            break;
    }

    if (next_page == DIALOGUE_PAGE_CLOSE) {
        end_conversation(client_fd, character_id, npc_id);
        return;
    }

    const DialoguePageDef* new_page = dialogue_find_page(dialogue, next_page);
    if (!new_page || !page_fits_on_wire(next_page, dialogue_id)) {
        LOG_ERROR("[DIALOGUE] dialogue %u routes to page %d, which does not exist",
                  dialogue_id, next_page);
        end_conversation(client_fd, character_id, npc_id);
        return;
    }

    dialogue_session_update_page(character_id, new_page->page_num);
    send_page(client_fd, character_id, &npc, dialogue_id, new_page, 0);
}

/**
 * Close a character's conversation.
 */
void handle_dialogue_close(uint32_t character_id) {
    dialogue_session_close(character_id);
}

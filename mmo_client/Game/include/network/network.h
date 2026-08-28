/**
 * @file
 * Declare asynchronous realm/world networking and packet-result polling.
 */

#ifndef NETWORK_H
#define NETWORK_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include "protocol.h"
#include "combat_render.h"

int  network_init(uint32_t account_id);
void network_cleanup(void);
void network_disconnect(void);
int  network_is_connected(void);
void network_set_character_id(uint32_t character_id);

/** Read and clear the last rejected opcode and retry delay in milliseconds. */
int  network_get_rate_limit_notice(uint8_t* out_type, uint16_t* out_retry_ms);

/** Read a disconnect reason retained after socket teardown. */
int  network_get_disconnect_reason(uint8_t* out_reason, char* out_message, int message_size);

void network_clear_disconnect_reason(void);

void network_update(void);
void network_update_with_ping(int game_mode);

/* Connecting is asynchronous; see net_connect.h.
 *
 * network_connect_to_realm() and network_connect_to_world() used to live here
 * and blocked the caller for the connect plus up to ten seconds of
 * acknowledgement polling -- on the render thread, from the frame loop. */

int network_request_world_list(void);
int network_get_world_list(WorldListResponsePacket* out);

int network_request_character_list(uint32_t world_id);
int network_get_character_list(CharacterListResponsePacket* out);

/** Ask the realm server which races exist. The client keeps no race table. */
int network_request_race_list(void);
int network_get_race_list(RaceListResponsePacket* out);

/** Ask the world server to swap forms, and read its authoritative reply. */
int network_request_form_swap(uint8_t requested_form);
int network_get_form_swap_ack(FormSwapAckPacket* out);

int network_create_character(uint32_t world_id, const char* name,
                            uint32_t class_id, uint32_t race_id);
int network_get_character_create_response(CharacterCreateResponsePacket* out);

int network_delete_character(uint32_t world_id, uint32_t character_id);
int network_get_character_delete_response(CharacterDeleteResponsePacket* out);

int network_request_enter_world(uint32_t character_id, uint32_t world_id);
int network_get_enter_world_response(EnterWorldResponsePacket* out);


int network_request_character_data(uint32_t character_id, uint32_t world_id);
int network_get_character_data(CharacterInfo* out);

/** Take the next movement sequence number. Monotonic within a session. */
uint32_t network_next_move_sequence(void);

/** Send one position proposal, numbered so a refusal can name it.
 *
 * @param sequence  From network_next_move_sequence(). The caller keeps it
 *                  alongside the position sent, so a correction naming it can
 *                  be reconciled rather than applied as a teleport.
 */
int network_send_player_move(float x, float y, float speed, float vel_x, float vel_y,
                             uint32_t sequence);

/** Consume the most recent position correction, if any.
 *
 * @param out_sequence  Receives the move the server refused; may be NULL.
 * @return Nonzero when a correction was copied out.
 */
int network_get_server_correction(float* out_x, float* out_y, uint32_t* out_sequence);

void network_send_ping(void);
/** Return the most recently measured round-trip time in milliseconds. */
int  network_get_ping_ms(void);

void network_update_facing_direction(float vel_x, float vel_y);
void network_send_attack_intent(float aim_x, float aim_y);

void network_send_ability_cast(uint16_t ability_id, float aim_x, float aim_y, uint32_t target_id);
void network_send_ability_cancel(void);

void network_request_player_stats(void);

void network_request_player_data_refresh(void);

void network_send_npc_interact_request(uint32_t npc_id);

void network_send_dialogue_option_select(uint32_t npc_id, uint32_t dialogue_id, uint8_t current_page, uint8_t option_id);

/** Ask the server to drop a quest. The log entry goes when the server confirms. */
void network_send_quest_abandon(uint32_t quest_id);

int network_get_npc_interact_response(NPCInteractResponsePacket* out);

int network_get_dialogue_update(DialogueUpdatePacket* out);

int network_get_dialogue_close(DialogueClosePacket* out);

void network_send_equip_item(uint32_t item_id, uint8_t inventory_slot, uint8_t equip_slot);
void network_send_unequip_item(uint8_t equip_slot);
void network_send_use_item(uint8_t inventory_slot);
void network_send_drop_item(uint8_t inventory_slot);
void network_send_move_item(uint8_t from_slot, uint8_t to_slot);

void network_send_chat(uint8_t channel, const char* message);

void network_send_loot_pickup(uint32_t ground_item_id);

void network_send_party_invite(const char* target_name);
void network_send_party_accept(void);
void network_send_party_decline(void);
void network_send_party_leave(void);
void network_send_party_kick(uint32_t target_id);

void network_send_shop_buy(uint32_t shop_id, uint32_t item_id);
void network_send_shop_sell(uint32_t shop_id, uint8_t inventory_slot);

void network_send_session_list_request(uint16_t page);

/* Friends. Every one of these names a character; the server resolves that to
 * the account behind it, because friendships are between accounts and a player
 * only ever knows their friends by the character they are playing. */
void network_send_friend_request(const char* target_name);
void network_send_friend_respond(const char* from_name, int accept);
void network_send_friend_remove(const char* target_name);
void network_send_friend_block(const char* target_name, int block);
void network_send_friend_list_request(void);

#endif // NETWORK_H

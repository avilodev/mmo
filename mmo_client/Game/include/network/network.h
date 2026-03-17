#ifndef NETWORK_H
#define NETWORK_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include "protocol.h"
#include "combat_render.h"

// ----------------------------------------------------------------------------
// INITIALIZATION
// ----------------------------------------------------------------------------

int  network_init(uint32_t account_id);
void network_cleanup(void);
void network_disconnect(void);
int  network_is_connected(void);
void network_set_character_id(uint32_t character_id);

// ----------------------------------------------------------------------------
// UPDATE (call every frame)
// ----------------------------------------------------------------------------

void network_update(void);
void network_update_with_ping(int game_mode);

// ----------------------------------------------------------------------------
// REALM SERVER
// ----------------------------------------------------------------------------

int network_connect_to_realm(const char* ip, uint16_t port, 
                             const char* session_key, uint32_t account_id);

int network_request_world_list(void);
int network_get_world_list(WorldListResponsePacket* out);

int network_request_character_list(uint32_t world_id);
int network_get_character_list(CharacterListResponsePacket* out);

int network_create_character(uint32_t world_id, const char* name,
                            uint32_t class_id, uint32_t race_id);
int network_get_character_create_response(CharacterCreateResponsePacket* out);

int network_delete_character(uint32_t world_id, uint32_t character_id);
int network_get_character_delete_response(CharacterDeleteResponsePacket* out);

int network_request_enter_world(uint32_t character_id, uint32_t world_id);
int network_get_enter_world_response(EnterWorldResponsePacket* out);

// ----------------------------------------------------------------------------
// WORLD SERVER
// ----------------------------------------------------------------------------

int network_connect_to_world(const char* ip, uint16_t port,
                            const char* game_ticket, uint32_t character_id);

int network_request_character_data(uint32_t character_id, uint32_t world_id);
int network_get_character_data(CharacterInfo* out);

int network_send_player_move(float x, float y, float speed, float vel_x, float vel_y);
int network_get_server_correction(float* out_x, float* out_y);

void network_send_ping(void);

// ----------------------------------------------------------------------------
// COMBAT
// ----------------------------------------------------------------------------

void network_update_facing_direction(float vel_x, float vel_y);
void network_send_attack_intent(float aim_x, float aim_y);

// ----------------------------------------------------------------------------
// ABILITIES
// ----------------------------------------------------------------------------

void network_send_ability_cast(uint16_t ability_id, float aim_x, float aim_y, uint32_t target_id);
void network_send_ability_cancel(void);

// ----------------------------------------------------------------------------
// STATS
// ----------------------------------------------------------------------------

// Request a full stat refresh from the server (e.g. on reconnect)
void network_request_player_stats(void);

// Request full character data refresh (updates XP, gold, inventory)
void network_request_player_data_refresh(void);

// ----------------------------------------------------------------------------
// NPC DIALOGUE
// ----------------------------------------------------------------------------

// Request to interact with an NPC
void network_send_npc_interact_request(uint32_t npc_id);

// Send the player's selected dialogue option
void network_send_dialogue_option_select(uint32_t npc_id, uint32_t dialogue_id, uint8_t current_page, uint8_t option_selected);

// Get the initial dialogue response from the server (returns 1 if ready, 0 if not)
int network_get_npc_interact_response(NPCInteractResponsePacket* out);

// Get the dialogue update from the server (returns 1 if ready, 0 if not)
int network_get_dialogue_update(DialogueUpdatePacket* out);

// Get the dialogue close notification from the server (returns 1 if ready, 0 if not)
int network_get_dialogue_close(DialogueClosePacket* out);

// ----------------------------------------------------------------------------
// INVENTORY / EQUIPMENT
// ----------------------------------------------------------------------------

void network_send_equip_item(uint32_t item_id, uint8_t inventory_slot, uint8_t equip_slot);
void network_send_unequip_item(uint8_t equip_slot);
void network_send_use_item(uint8_t inventory_slot);
void network_send_drop_item(uint8_t inventory_slot);
void network_send_move_item(uint8_t from_slot, uint8_t to_slot);

// ----------------------------------------------------------------------------
// CHAT
// ----------------------------------------------------------------------------

void network_send_chat(uint8_t channel, const char* message);

// ----------------------------------------------------------------------------
// LOOT
// ----------------------------------------------------------------------------

void network_send_loot_pickup(uint32_t ground_item_id);

// ----------------------------------------------------------------------------
// PARTY
// ----------------------------------------------------------------------------

void network_send_party_invite(const char* target_name);
void network_send_party_accept(void);
void network_send_party_decline(void);
void network_send_party_leave(void);
void network_send_party_kick(uint32_t target_id);

#endif // NETWORK_H
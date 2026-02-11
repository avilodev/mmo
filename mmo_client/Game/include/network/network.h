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

#endif // NETWORK_H
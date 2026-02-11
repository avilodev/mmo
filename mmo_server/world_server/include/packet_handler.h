#include "types.h"

#include "routes.h"
#include "player_data.h"
#include "items_database.h"
#include "utils.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>


void handle_player_move(int client_fd, uint32_t character_id, PlayerMovePacket* pkt);
void handle_request_player_data(int client_fd, uint32_t character_id);
void handle_ping(int client_fd, uint8_t* buffer);
void handle_equip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes);
void handle_unequip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes);
void handle_use_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes);
void handle_drop_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes);
void handle_move_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes);

#ifndef AUTH_H
#define AUTH_H

#include "types.h"
#include "utils.h"

#include <stdlib.h>
#include <stdint.h>

void auth_handle_login(int client_fd, AuthLoginPacket* packet);
void auth_handle_start_game(int client_fd, StartGameRequestPacket* packet);
void auth_handle_register(int client_fd, AuthRegisterPacket* packet);

#endif // AUTH_H
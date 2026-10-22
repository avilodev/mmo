#ifndef PARTY_COMMANDS_H
#define PARTY_COMMANDS_H

// Party commands. Network code parses a request and calls these.

#include <stdint.h>

// validate and deliver a party invitation by player name
void party_command_invite(int client_fd, uint32_t character_id, const char* target_name);

// consume a pending invitation and join or create its party
void party_command_accept(int client_fd, uint32_t character_id);

// remove the character's pending party invitation
void party_command_decline(int client_fd, uint32_t character_id);

// remove the character from its current party
void party_command_leave(int client_fd, uint32_t character_id);

// remove a target member when requested by the party leader
void party_command_kick(int client_fd, uint32_t character_id, uint32_t target_id);

#endif /* PARTY_COMMANDS_H */

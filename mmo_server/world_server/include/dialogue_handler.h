#ifndef DIALOGUE_HANDLER_H
#define DIALOGUE_HANDLER_H

#include <stdint.h>
#include <sys/types.h>

// Handle client request to interact with NPC
void handle_npc_interact_request(int client_fd, uint32_t character_id,
                                 uint8_t* buffer, ssize_t bytes);

// Handle client selection of dialogue option
void handle_dialogue_option_select(int client_fd, uint32_t character_id,
                                   uint8_t* buffer, ssize_t bytes);

// Handle client closing dialogue window
void handle_dialogue_close(uint32_t character_id);

#endif // DIALOGUE_HANDLER_H

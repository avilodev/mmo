#ifndef PLAYER_DIRECTORY_H
#define PLAYER_DIRECTORY_H

// Who is online. Network code parses a request and calls these.

#include <stdint.h>

// record the latency a client reported
void player_directory_record_ping(uint32_t character_id, int player_slot,
                                  uint16_t ping_ms);

// send a client its character data
void player_directory_send_data(int client_fd, uint32_t character_id);

// answer a name query; ids are in network byte order
void player_directory_names(int client_fd, const uint32_t* character_ids, int asked);

// send one page of the online roster
void player_directory_session_page(int client_fd, uint16_t requested_page);

#endif /* PLAYER_DIRECTORY_H */

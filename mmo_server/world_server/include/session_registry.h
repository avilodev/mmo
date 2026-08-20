#ifndef SESSION_REGISTRY_H
#define SESSION_REGISTRY_H

#include <stdint.h>
#include <time.h>

/** @file Track authenticated world sessions in a descriptor-indexed table.
 *
 * Every lookup and mutation is O(1). The table is keyed by descriptor, exactly like
 * the connection tables in net_loop.c and connection_io.c, with hash indexes over
 * account and character identifiers for the other two lookups.
 */

/** Record one connection's authenticated identity.
 *
 * There is deliberately no activity timestamp here. Idle disconnects are driven by
 * Connection::last_recv in net_loop.c, an atomic that every read already updates; the
 * timestamp this struct used to carry was written on every inbound packet, under a
 * process-wide exclusive lock, and read by nothing.
 */
typedef struct {
    int      fd;
    uint32_t account_id;
    uint32_t character_id;
    time_t   connect_time;
    uint8_t  active;
} SessionEntry;

// allocate the descriptor-indexed table; call once before accepting clients
void session_registry_init(void);

// release the table; safe to call without a preceding init
void session_registry_shutdown(void);

// return -1 when the descriptor is out of range or the table is unavailable
int session_registry_add(int fd, uint32_t account_id, uint32_t character_id);

void session_registry_remove(int fd);

// copy a matching entry and return without retaining the registry lock
int session_find_by_fd(int fd, SessionEntry* out);

// copy a matching entry and return without retaining the registry lock
int session_find_by_account(uint32_t account_id, SessionEntry* out);

// copy a matching entry and return without retaining the registry lock
int session_find_by_character(uint32_t character_id, SessionEntry* out);

#endif

#ifndef WORLD_TABLE_H
#define WORLD_TABLE_H

/** @file The world roster, loaded from configuration rather than compiled in.
 *
 * Every service needs some slice of the same facts about a world: the realm
 * needs its address to probe it and its identifier to hand out a ticket, the
 * world server needs its own database, and every one of them wants its display
 * name for a log line. Those facts used to live in five places -- three
 * hardcoded ladders in world_database_config.c, the realm's worlds.txt, and
 * each world's .conf -- so adding a world meant six coordinated edits and a
 * recompile, and any one of them going missing produced a NULL connection
 * string at runtime rather than an error at startup.
 *
 * They live here instead, read once from `worlds.conf` into a table that grows
 * with the file. There is no ceiling on how many worlds may be listed: the
 * table reallocates, and the callers that used to index fixed arrays now ask
 * this module how many there are.
 *
 * Thread safety: load once during startup, read freely afterwards. The
 * accessors take no lock and the entries are never mutated after a successful
 * load, so concurrent readers are safe; a concurrent load is not.
 */

#include <stddef.h>
#include <stdint.h>

/** One configured world. */
typedef struct {
    uint32_t id;           /**< 1-based; assigned from row order in the file. */
    char*    name;         /**< World name; also the .conf basename. */
    char*    region;       /**< Display region, with underscores turned into spaces. */
    char*    database;     /**< PostgreSQL database name. */
    char*    host;         /**< Address clients and the realm connect to. */
    char*    conninfo;     /**< Full libpq connection string built from `database`. */
    uint16_t port;         /**< TCP port clients connect to. */
    /** TCP port the realm's heartbeat link connects to.
     *
     * A separate listener rather than a second protocol on the game port. The
     * world used to tell the two apart by peeking at the first byte of every
     * accepted connection, which meant the realm's privileged handshake was
     * reachable on the port the whole internet talks to, gated only by an
     * address allowlist applied after the accept. Two ports means the admission
     * policy is the socket, not a byte comparison -- a firewall can keep the
     * realm link on the private interface and never expose it at all.
     *
     * Defaults to `port + WORLD_REALM_PORT_OFFSET` when worlds.conf does not
     * give an eighth column.
     */
    uint16_t realm_port;
    uint32_t max_players;
    uint8_t  hardcore;
} WorldEntry;

/** Default distance from a world's client port to its realm port. */
#define WORLD_REALM_PORT_OFFSET 1000

/** Load the world table, replacing any table already loaded.
 *
 * @param path  File to read, or NULL to resolve one the way
 *              world_table_default_path() describes.
 * @return      1 when at least one world was loaded, otherwise 0. On failure
 *              the previously loaded table is left untouched.
 */
int world_table_load(const char* path);

/** Resolve the path world_table_load(NULL) would read.
 *
 * Checks, in order: `$MMO_WORLDS_CONF`, `<exe dir>/data/worlds.conf`,
 * `<exe dir>/../data/worlds.conf`, `setup/worlds.conf` relative to the working
 * directory. The first that exists wins.
 *
 * @return 1 when a readable candidate was found and written to `out`, else 0.
 */
int world_table_default_path(char* out, size_t out_size);

/** Number of worlds currently loaded. Loads lazily on first use. */
size_t world_table_count(void);

/** Look up a world by its 1-based identifier.
 *
 * @return The entry, or NULL when no world carries that identifier.
 */
const WorldEntry* world_table_by_id(uint32_t world_id);

/** Look up a world by name, case-insensitively.
 *
 * @return The entry, or NULL when no world carries that name.
 */
const WorldEntry* world_table_by_name(const char* name);

/** Iterate the table in identifier order.
 *
 * @param index  Zero-based position, `< world_table_count()`.
 * @return       The entry, or NULL when `index` is past the end.
 */
const WorldEntry* world_table_at(size_t index);

/** Release the table. Only for teardown and tests; accessors reload lazily. */
void world_table_free(void);

#endif // WORLD_TABLE_H

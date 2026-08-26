/**
 * @file
 * Read the world roster from configuration into a table that grows with it.
 */

#define _POSIX_C_SOURCE 200809L

#include "world_table.h"
#include "log.h"
#include "data_paths.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>

/** The loaded roster. Grows by doubling; nothing here has a configured ceiling. */
static WorldEntry* g_worlds   = NULL;
static size_t      g_count    = 0;

/** Guards the lazy first load only. Readers after that need no lock. */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/** PostgreSQL connection parameters shared by every world.
 *
 * Only the database name differs per world, so the rest is configured once.
 * A setting may be given in worlds.conf as `key = value`; the matching
 * environment variable overrides the file, which is how a deployment points
 * the same checked-in configuration at a different server.
 */
typedef struct {
    char* host;
    char* port;
    char* user;
    char* password;
    char* sslmode;
    char* options;
} PgSettings;

/** Duplicate a string, returning NULL only when the input is NULL or empty. */
static char* dup_or_null(const char* s) {
    if (!s || !*s) return NULL;
    return strdup(s);
}

/** Replace a heap string with a copy of `value`. */
static void set_string(char** slot, const char* value) {
    char* copy = dup_or_null(value);
    free(*slot);
    *slot = copy;
}

/** Append `key=value ` to a connection string when the value is present. */
static int conninfo_append(char** out, size_t* len, size_t* cap,
                           const char* key, const char* value) {
    if (!value || !*value) return 1;

    size_t need = *len + strlen(key) + strlen(value) + 3;  /* key=value + space + NUL */
    if (need > *cap) {
        size_t grown = *cap ? *cap : 64;
        while (grown < need) grown *= 2;
        char* bigger = realloc(*out, grown);
        if (!bigger) return 0;
        *out = bigger;
        *cap = grown;
    }
    int written = snprintf(*out + *len, *cap - *len, "%s%s=%s",
                           *len ? " " : "", key, value);
    if (written < 0) return 0;
    *len += (size_t)written;
    return 1;
}

/**
 * Say once what the database credentials actually are.
 *
 * Two defaults are worth naming out loud, because both are invisible until
 * something goes wrong and neither is what a deployment should be running:
 *
 *  - `postgres` is the cluster superuser. Every world server connecting as it
 *    means a SQL injection anywhere, or one compromised world process, owns
 *    every other world's database and the cluster itself. The world servers
 *    need SELECT/INSERT/UPDATE/DELETE on their own database and nothing more;
 *    `make setup` creates an `mmo_app` role with exactly that.
 *
 *  - No sslmode means libpq's default of `prefer`, which silently accepts an
 *    unencrypted connection. On one host that is fine; across a network it
 *    sends the password and every character's data in the clear.
 *
 * Once per process, not once per world, because ten identical warnings are one
 * warning nobody reads.
 */
static void warn_about_credentials(const PgSettings* pg) {
    static int warned = 0;
    if (warned) return;
    warned = 1;

    const char* user = pg->user ? pg->user : "postgres";
    if (strcmp(user, "postgres") == 0) {
        LOG_WARN("Connecting to PostgreSQL as the superuser 'postgres'. "
                 "Set pg_user in worlds.conf (or $MMO_PG_USER) to a role with "
                 "rights only on the world databases -- 'make setup' creates "
                 "'mmo_app' for exactly this.");
    }

    if (!pg->sslmode || !*pg->sslmode) {
        LOG_WARN("No pg_sslmode configured; libpq will accept an unencrypted "
                 "connection. Set pg_sslmode = require (or verify-full) "
                 "whenever PostgreSQL is not on this host.");
    }
}

/** Build one world's libpq connection string from the shared settings. */
static char* build_conninfo(const PgSettings* pg, const char* database) {
    char*  out = NULL;
    size_t len = 0, cap = 0;

    warn_about_credentials(pg);

    const char* host = pg->host ? pg->host : "localhost";
    const char* user = pg->user ? pg->user : "postgres";

    if (!conninfo_append(&out, &len, &cap, "host", host) ||
        !conninfo_append(&out, &len, &cap, "port", pg->port) ||
        !conninfo_append(&out, &len, &cap, "dbname", database) ||
        !conninfo_append(&out, &len, &cap, "user", user) ||
        !conninfo_append(&out, &len, &cap, "password", pg->password) ||
        !conninfo_append(&out, &len, &cap, "sslmode", pg->sslmode) ||
        !conninfo_append(&out, &len, &cap, "options", pg->options)) {
        free(out);
        return NULL;
    }
    return out;
}

/** Release every string one entry owns. */
static void entry_free(WorldEntry* e) {
    free(e->name);
    free(e->region);
    free(e->database);
    free(e->host);
    free(e->conninfo);
    memset(e, 0, sizeof(*e));
}

/** Release a table built during a load that has not been published. */
static void table_free(WorldEntry* entries, size_t count) {
    for (size_t i = 0; i < count; i++) entry_free(&entries[i]);
    free(entries);
}

/** Trim leading and trailing whitespace in place, returning the new start. */
static char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/** Turn the `_` separators a region label uses into spaces, in place.
 *
 * The file is whitespace-separated, so a region cannot contain a literal
 * space; "North_America" is how the file spells what a player should read as
 * "North America".
 */
static void underscores_to_spaces(char* s) {
    for (; *s; s++) if (*s == '_') *s = ' ';
}

/** Apply one `key = value` setting line. */
static void apply_setting(PgSettings* pg, const char* key, const char* value) {
    if (strcasecmp(key, "pg_host") == 0)          set_string(&pg->host, value);
    else if (strcasecmp(key, "pg_port") == 0)     set_string(&pg->port, value);
    else if (strcasecmp(key, "pg_user") == 0)     set_string(&pg->user, value);
    else if (strcasecmp(key, "pg_password") == 0) set_string(&pg->password, value);
    else if (strcasecmp(key, "pg_sslmode") == 0)  set_string(&pg->sslmode, value);
    else if (strcasecmp(key, "pg_options") == 0)  set_string(&pg->options, value);
    else LOG_ERROR("worlds.conf: ignoring unknown setting '%s'", key);
}

/** Let the environment override what the file said. */
static void apply_environment(PgSettings* pg) {
    const struct { const char* env; char** slot; } overrides[] = {
        { "MMO_PG_HOST",     &pg->host },
        { "MMO_PG_PORT",     &pg->port },
        { "MMO_PG_USER",     &pg->user },
        { "MMO_PG_PASSWORD", &pg->password },
        { "MMO_PG_SSLMODE",  &pg->sslmode },
        { "MMO_PG_OPTIONS",  &pg->options },
    };
    for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++) {
        const char* value = getenv(overrides[i].env);
        if (value) set_string(overrides[i].slot, value);
    }
}

/** Release every string the settings own. */
static void pg_settings_free(PgSettings* pg) {
    free(pg->host);
    free(pg->port);
    free(pg->user);
    free(pg->password);
    free(pg->sslmode);
    free(pg->options);
    memset(pg, 0, sizeof(*pg));
}

/**
 * Resolve the path world_table_load(NULL) would read.
 *
 * @return 1 when a readable candidate was found, otherwise 0.
 */
int world_table_default_path(char* out, size_t out_size) {
    if (!out || out_size == 0) return 0;

    const char* env = getenv("MMO_WORLDS_CONF");
    if (env && *env) {
        snprintf(out, out_size, "%s", env);
        return access(out, R_OK) == 0;
    }

    /* Beside the binary first: `make` packages the file into each bin/data,
     * so a deployed server finds it wherever it was launched from. The
     * remaining candidates are the source tree, for running out of a build
     * directory without installing. */
    static const char* relative[] = {
        "/data/worlds.conf",
        "/../data/worlds.conf",
        "/../../setup/worlds.conf",
        "/../../../setup/worlds.conf",
    };
    for (size_t i = 0; i < sizeof(relative) / sizeof(relative[0]); i++) {
        if (!data_path_resolve(out, out_size, relative[i])) continue;
        if (access(out, R_OK) == 0) return 1;
    }

    snprintf(out, out_size, "setup/worlds.conf");
    return access(out, R_OK) == 0;
}

/**
 * Load the world table, replacing any table already loaded.
 *
 * A malformed row fails the whole file rather than shrinking the roster
 * silently: a world that quietly fails to load is a world whose players
 * cannot log in, reported as an empty list rather than as an error.
 *
 * @return 1 when at least one world was loaded, otherwise 0.
 */
int world_table_load(const char* path) {
    char resolved[1024];
    if (!path) {
        if (!world_table_default_path(resolved, sizeof(resolved))) {
            LOG_ERROR("world_table: no worlds.conf found. Set MMO_WORLDS_CONF, or "
                      "run `make setup` so the file is packaged beside the binary.");
            return 0;
        }
        path = resolved;
    }

    FILE* f = fopen(path, "r");
    if (!f) {
        LOG_ERROR("world_table: cannot open '%s': %s", path, strerror(errno));
        return 0;
    }

    PgSettings  pg      = {0};
    WorldEntry* entries = NULL;
    size_t      count   = 0;
    size_t      cap     = 0;
    int         ok      = 1;
    int         line_no = 0;
    char        line[512];

    while (fgets(line, sizeof(line), f)) {
        line_no++;

        char* comment = strchr(line, '#');
        if (comment) *comment = '\0';

        char* row = trim(line);
        if (!*row) continue;

        char* equals = strchr(row, '=');
        if (equals) {
            *equals = '\0';
            apply_setting(&pg, trim(row), trim(equals + 1));
            continue;
        }

        char name[128], region[128], database[128], host[128];
        unsigned port = 0, max_players = 0, hardcore = 0, realm_port = 0;
        /* The eighth column is optional. A row that omits it gets
         * port + WORLD_REALM_PORT_OFFSET, which is what every existing
         * worlds.conf means without having to be rewritten. */
        int fields = sscanf(row, "%127s %127s %127s %127s %u %u %u %u",
                            name, region, database, host,
                            &port, &max_players, &hardcore, &realm_port);
        if (fields != 7 && fields != 8) {
            LOG_ERROR("world_table: %s:%d: expected 7 or 8 fields, got %d: %s",
                      path, line_no, fields, row);
            ok = 0;
            break;
        }
        if (port == 0 || port > 65535) {
            LOG_ERROR("world_table: %s:%d: port %u out of range",
                      path, line_no, port);
            ok = 0;
            break;
        }
        if (fields == 7) realm_port = port + WORLD_REALM_PORT_OFFSET;
        if (realm_port == 0 || realm_port > 65535) {
            LOG_ERROR("world_table: %s:%d: realm port %u out of range",
                      path, line_no, realm_port);
            ok = 0;
            break;
        }
        if (realm_port == port) {
            LOG_ERROR("world_table: %s:%d: realm port %u is the client port; "
                      "they must differ", path, line_no, realm_port);
            ok = 0;
            break;
        }

        if (count == cap) {
            size_t grown = cap ? cap * 2 : 8;
            WorldEntry* bigger = realloc(entries, grown * sizeof(*entries));
            if (!bigger) { ok = 0; break; }
            entries = bigger;
            memset(entries + cap, 0, (grown - cap) * sizeof(*entries));
            cap = grown;
        }

        WorldEntry* e = &entries[count];
        e->id          = (uint32_t)count + 1;
        e->name        = strdup(name);
        e->region      = strdup(region);
        e->database    = strdup(database);
        e->host        = strdup(host);
        e->port        = (uint16_t)port;
        e->realm_port  = (uint16_t)realm_port;
        e->max_players = max_players;
        e->hardcore    = (uint8_t)(hardcore != 0);

        if (!e->name || !e->region || !e->database || !e->host) { ok = 0; break; }
        underscores_to_spaces(e->region);
        count++;
    }

    fclose(f);

    if (ok && count == 0) {
        LOG_ERROR("world_table: '%s' lists no worlds", path);
        ok = 0;
    }

    /* The connection strings are built last so a settings line placed after
     * the rows still applies -- the file reads as one document, not a
     * sequence of statements. */
    if (ok) {
        apply_environment(&pg);
        for (size_t i = 0; i < count; i++) {
            entries[i].conninfo = build_conninfo(&pg, entries[i].database);
            if (!entries[i].conninfo) { ok = 0; break; }
        }
    }

    pg_settings_free(&pg);

    if (!ok) {
        table_free(entries, count);
        return 0;
    }

    /* Publish only after the whole file parsed. A partially applied roster is
     * worse than the previous one. */
    table_free(g_worlds, g_count);
    g_worlds = entries;
    g_count  = count;
    return 1;
}

/** Load once on first access so callers that never call load still work. */
static void lazy_load(void) {
    if (g_count == 0) world_table_load(NULL);
}

static void ensure_loaded(void) {
    pthread_once(&g_once, lazy_load);
}

size_t world_table_count(void) {
    ensure_loaded();
    return g_count;
}

const WorldEntry* world_table_at(size_t index) {
    ensure_loaded();
    return index < g_count ? &g_worlds[index] : NULL;
}

const WorldEntry* world_table_by_id(uint32_t world_id) {
    ensure_loaded();
    /* Identifiers are assigned from row order, so the index is known; the scan
     * is the fallback for a table that ever stops being densely numbered. */
    if (world_id >= 1 && (size_t)world_id <= g_count &&
        g_worlds[world_id - 1].id == world_id) {
        return &g_worlds[world_id - 1];
    }
    for (size_t i = 0; i < g_count; i++)
        if (g_worlds[i].id == world_id) return &g_worlds[i];
    return NULL;
}

const WorldEntry* world_table_by_name(const char* name) {
    if (!name) return NULL;
    ensure_loaded();
    for (size_t i = 0; i < g_count; i++)
        if (strcasecmp(g_worlds[i].name, name) == 0) return &g_worlds[i];
    return NULL;
}

void world_table_free(void) {
    table_free(g_worlds, g_count);
    g_worlds = NULL;
    g_count  = 0;
}

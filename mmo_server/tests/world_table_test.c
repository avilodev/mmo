/**
 * @file
 * Exercise the world table: parsing, growth, lookup, and failure modes.
 */

#include "world_table.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_checks = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/** Write a temporary configuration file and return its path in `out`. */
static void write_conf(char* out, size_t out_size, const char* body) {
    snprintf(out, out_size, "/tmp/world_table_test_%d.conf", (int)getpid());
    FILE* f = fopen(out, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

/** A file with more worlds than any compiled ceiling must still load in full. */
static void test_grows_past_any_fixed_ceiling(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/world_table_big_%d.conf", (int)getpid());

    const int many = 257;  /* deliberately past MAX_WORLDS and past any power of two */
    FILE* f = fopen(path, "w");
    assert(f);
    fputs("# generated\n", f);
    for (int i = 1; i <= many; i++)
        fprintf(f, "World%d Region_%d world%d_db 10.0.0.%d %d %d 0\n",
                i, i % 3, i, i % 255, 20000 + i, i * 7);
    fclose(f);

    CHECK(world_table_load(path), "a %d-world roster must load", many);
    CHECK(world_table_count() == (size_t)many,
          "expected %d worlds, got %zu", many, world_table_count());

    const WorldEntry* last = world_table_by_id((uint32_t)many);
    CHECK(last != NULL, "world %d must be present", many);
    CHECK(strcmp(last->name, "World257") == 0, "last world name is '%s'", last->name);
    CHECK(last->port == 20000 + many, "last world port is %u", last->port);
    CHECK(last->max_players == (uint32_t)many * 7,
          "last world capacity is %u", last->max_players);

    remove(path);
}

/** Identifiers come from row order, and lookups agree with each other. */
static void test_identifiers_follow_row_order(void) {
    char path[256];
    write_conf(path, sizeof(path),
               "Alpha North_America alpha_db 127.0.0.1 7001 100 0\n"
               "Beta  Europe        beta_db  127.0.0.1 7002 200 1\n");

    CHECK(world_table_load(path), "roster must load");
    CHECK(world_table_count() == 2, "expected 2 worlds, got %zu", world_table_count());

    const WorldEntry* alpha = world_table_by_id(1);
    const WorldEntry* beta  = world_table_by_id(2);
    CHECK(alpha && strcmp(alpha->name, "Alpha") == 0, "world 1 is Alpha");
    CHECK(beta  && strcmp(beta->name,  "Beta")  == 0, "world 2 is Beta");
    CHECK(world_table_by_name("alpha") == alpha, "lookup by name is case-insensitive");
    CHECK(world_table_by_name("BETA")  == beta,  "lookup by name is case-insensitive");
    CHECK(world_table_by_name("Gamma") == NULL,  "an absent world resolves to NULL");
    CHECK(world_table_by_id(0) == NULL, "identifier 0 is never valid");
    CHECK(world_table_by_id(3) == NULL, "an identifier past the end resolves to NULL");

    CHECK(beta->hardcore == 1, "Beta is hardcore");
    CHECK(alpha->hardcore == 0, "Alpha is not hardcore");

    /* Underscores in the file are spaces in the label a player reads. */
    CHECK(strcmp(alpha->region, "North America") == 0,
          "region reads '%s'", alpha->region);

    remove(path);
}

/** The connection string is built from the shared settings plus the database. */
static void test_conninfo_from_settings(void) {
    char path[256];
    write_conf(path, sizeof(path),
               "pg_host = db.internal\n"
               "pg_user = mmo_app\n"
               "pg_sslmode = require\n"
               "Alpha North_America alpha_db 127.0.0.1 7001 100 0\n");

    CHECK(world_table_load(path), "roster must load");
    const WorldEntry* alpha = world_table_by_id(1);
    CHECK(alpha != NULL, "Alpha must be present");
    CHECK(strstr(alpha->conninfo, "host=db.internal"), "conninfo: %s", alpha->conninfo);
    CHECK(strstr(alpha->conninfo, "dbname=alpha_db"),  "conninfo: %s", alpha->conninfo);
    CHECK(strstr(alpha->conninfo, "user=mmo_app"),     "conninfo: %s", alpha->conninfo);
    CHECK(strstr(alpha->conninfo, "sslmode=require"),  "conninfo: %s", alpha->conninfo);
    CHECK(strstr(alpha->conninfo, "password=") == NULL,
          "no password key when none is configured: %s", alpha->conninfo);

    remove(path);
}

/** A settings line after the rows still applies: the file reads as one document. */
static void test_settings_apply_regardless_of_position(void) {
    char path[256];
    write_conf(path, sizeof(path),
               "Alpha North_America alpha_db 127.0.0.1 7001 100 0\n"
               "pg_host = late.example\n");

    CHECK(world_table_load(path), "roster must load");
    CHECK(strstr(world_table_by_id(1)->conninfo, "host=late.example"),
          "a trailing setting must still reach the connection string");

    remove(path);
}

/** The environment overrides the file, so one file serves many deployments. */
static void test_environment_overrides_file(void) {
    char path[256];
    write_conf(path, sizeof(path),
               "pg_host = file.example\n"
               "Alpha North_America alpha_db 127.0.0.1 7001 100 0\n");

    setenv("MMO_PG_HOST", "env.example", 1);
    CHECK(world_table_load(path), "roster must load");
    CHECK(strstr(world_table_by_id(1)->conninfo, "host=env.example"),
          "the environment must win: %s", world_table_by_id(1)->conninfo);
    unsetenv("MMO_PG_HOST");

    remove(path);
}

/** A malformed row fails the whole file and leaves the previous table intact. */
static void test_malformed_row_keeps_previous_table(void) {
    char good[256], bad[256];
    write_conf(good, sizeof(good),
               "Alpha North_America alpha_db 127.0.0.1 7001 100 0\n");
    CHECK(world_table_load(good), "the good roster must load");
    CHECK(world_table_count() == 1, "one world loaded");

    snprintf(bad, sizeof(bad), "/tmp/world_table_bad_%d.conf", (int)getpid());
    FILE* f = fopen(bad, "w");
    assert(f);
    fputs("Alpha North_America alpha_db 127.0.0.1 7001 100 0\n"
          "Beta  Europe        beta_db  127.0.0.1\n", f);   /* four fields, not seven */
    fclose(f);

    CHECK(!world_table_load(bad), "a short row must fail the whole file");
    CHECK(world_table_count() == 1, "the previous roster must survive a failed load");
    CHECK(world_table_by_name("Alpha") != NULL, "and still answer lookups");

    remove(good);
    remove(bad);
}

/** An out-of-range port is a configuration error, not something to accept. */
static void test_port_range_is_checked(void) {
    char path[256];
    write_conf(path, sizeof(path),
               "Alpha North_America alpha_db 127.0.0.1 70000 100 0\n");
    CHECK(!world_table_load(path), "port 70000 must be refused");
    remove(path);
}

/** An empty or comment-only file lists no worlds, which is a failure. */
static void test_empty_file_is_a_failure(void) {
    char path[256];
    write_conf(path, sizeof(path), "# nothing but a comment\n\n   \n");
    CHECK(!world_table_load(path), "a roster with no worlds must fail");
    remove(path);
}

/** The roster shipped in this repository must parse. */
static void test_repository_roster_parses(void) {
    const char* candidates[] = {
        "setup/worlds.conf",
        "../setup/worlds.conf",
    };
    const char* found = NULL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], R_OK) == 0) { found = candidates[i]; break; }
    }
    if (!found) {
        printf("  (skipped: setup/worlds.conf not reachable from here)\n");
        return;
    }

    CHECK(world_table_load(found), "the repository roster must parse");
    CHECK(world_table_count() > 0, "and must list at least one world");

    /* Every entry must be complete: a half-filled row is what the old
     * hardcoded ladders produced when one of the six edits was forgotten. */
    for (size_t i = 0; i < world_table_count(); i++) {
        const WorldEntry* e = world_table_at(i);
        CHECK(e->id == (uint32_t)i + 1, "identifiers are dense and 1-based");
        CHECK(e->name && *e->name, "world %zu has a name", i);
        CHECK(e->database && *e->database, "world %s has a database", e->name);
        CHECK(e->host && *e->host, "world %s has a host", e->name);
        CHECK(e->conninfo && strstr(e->conninfo, e->database),
              "world %s conninfo names its database", e->name);
        CHECK(e->port != 0, "world %s has a port", e->name);
    }
    printf("  repository roster: %zu worlds\n", world_table_count());
}

int main(void) {
    printf("=== world table ===\n");

    test_identifiers_follow_row_order();
    printf("  identifiers follow row order: ok\n");

    test_grows_past_any_fixed_ceiling();
    printf("  grows past any fixed ceiling: ok\n");

    test_conninfo_from_settings();
    printf("  connection string from settings: ok\n");

    test_settings_apply_regardless_of_position();
    printf("  settings apply regardless of position: ok\n");

    test_environment_overrides_file();
    printf("  environment overrides the file: ok\n");

    test_malformed_row_keeps_previous_table();
    printf("  a malformed row keeps the previous table: ok\n");

    test_port_range_is_checked();
    printf("  port range is checked: ok\n");

    test_empty_file_is_a_failure();
    printf("  an empty roster is a failure: ok\n");

    test_repository_roster_parses();
    printf("  repository roster parses: ok\n");

    world_table_free();
    printf("=== world table: %d checks passed ===\n", g_checks);
    return 0;
}

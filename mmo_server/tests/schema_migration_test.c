/**
 * @file
 * Exercise the migration runner against a real PostgreSQL database.
 *
 * What matters about a migration system is not that it can run SQL — it is
 * that it runs each step exactly once, records what it ran, refuses to proceed
 * past a failure, and refuses to run at all against a database a newer build
 * has already migrated. Those are the four things checked here, plus the
 * rollback path.
 *
 * Set MMO_TEST_PGCONN to run; an unset variable produces a successful skip, the
 * same as character_items_db_test.
 */

#include "schema_migrations.h"
#include "log.h"

#include <libpq-fe.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/** The component name, so this test's rows never collide with a real schema. */
#define TEST_COMPONENT "schema_migration_test"

/** Table names chosen so a stray one is obviously this test's. */
static const SchemaMigration k_migrations[] = {
    {
        .version = 1,
        .name    = "create the first table",
        .up      = "CREATE TABLE IF NOT EXISTS mig_test_a (id INTEGER PRIMARY KEY);",
        .down    = "DROP TABLE IF EXISTS mig_test_a;",
    },
    {
        .version = 2,
        .name    = "create the second table",
        .up      = "CREATE TABLE IF NOT EXISTS mig_test_b (id INTEGER PRIMARY KEY);",
        .down    = "DROP TABLE IF EXISTS mig_test_b;",
    },
    {
        .version = 3,
        .name    = "add a column",
        .up      = "ALTER TABLE mig_test_a ADD COLUMN IF NOT EXISTS extra TEXT;",
        .down    = "ALTER TABLE mig_test_a DROP COLUMN IF EXISTS extra;",
    },
};

/** The same set, with a step that cannot succeed in the middle. */
static const SchemaMigration k_broken[] = {
    {
        .version = 1,
        .name    = "create the first table",
        .up      = "CREATE TABLE IF NOT EXISTS mig_test_a (id INTEGER PRIMARY KEY);",
        .down    = "DROP TABLE IF EXISTS mig_test_a;",
    },
    {
        .version = 2,
        .name    = "a step that cannot work",
        .up      = "ALTER TABLE mig_test_does_not_exist ADD COLUMN nope INTEGER;",
        .down    = NULL,
    },
    {
        .version = 3,
        .name    = "a step that must never run",
        .up      = "CREATE TABLE mig_test_should_not_exist (id INTEGER);",
        .down    = NULL,
    },
};

static int table_exists(PGconn* conn, const char* name) {
    const char* params[1] = { name };
    PGresult* res = PQexecParams(conn,
        "SELECT 1 FROM information_schema.tables "
        "WHERE table_schema = 'public' AND table_name = $1",
        1, NULL, params, NULL, NULL, 0);
    int found = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    PQclear(res);
    return found;
}

static int column_exists(PGconn* conn, const char* table, const char* column) {
    const char* params[2] = { table, column };
    PGresult* res = PQexecParams(conn,
        "SELECT 1 FROM information_schema.columns "
        "WHERE table_schema = 'public' AND table_name = $1 AND column_name = $2",
        2, NULL, params, NULL, NULL, 0);
    int found = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    PQclear(res);
    return found;
}

static void run(PGconn* conn, const char* sql) {
    PQclear(PQexec(conn, sql));
}

/** Remove everything this test creates, so a rerun starts from nothing. */
static void clean(PGconn* conn) {
    run(conn, "DROP TABLE IF EXISTS mig_test_a;");
    run(conn, "DROP TABLE IF EXISTS mig_test_b;");
    run(conn, "DROP TABLE IF EXISTS mig_test_should_not_exist;");
    run(conn, "DELETE FROM schema_version WHERE component = '" TEST_COMPONENT "';");
}

int main(void) {
    log_init();
    printf("=== schema migrations ===\n");

    const char* conninfo = getenv("MMO_TEST_PGCONN");
    if (!conninfo || !*conninfo) {
        printf("  skipped: set MMO_TEST_PGCONN to run against a real database\n");
        return 0;
    }

    PGconn* conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        printf("  FAIL could not connect: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return 1;
    }

    /* The version table may not exist yet on a fresh database, and clean()
     * deletes from it. One migrate call creates it. */
    schema_migrate(conn, k_migrations, 1, TEST_COMPONENT);
    clean(conn);

    const size_t all = sizeof(k_migrations) / sizeof(k_migrations[0]);

    printf("\nTEST 1: an empty database is brought to the newest version\n");
    {
        CHECK(schema_migrate(conn, k_migrations, all, TEST_COMPONENT),
              "the run succeeds");

        int version = -1;
        CHECK(schema_current_version(conn, TEST_COMPONENT, &version),
              "the version can be read back");
        CHECK(version == 3, "and it is the newest migration");

        CHECK(table_exists(conn, "mig_test_a"), "the first table exists");
        CHECK(table_exists(conn, "mig_test_b"), "the second table exists");
        CHECK(column_exists(conn, "mig_test_a", "extra"),
              "and the added column exists");
    }

    printf("\nTEST 2: a second run applies nothing\n");
    {
        /* The point of a version table: re-running must not re-execute. The
         * old startup path re-ran every CREATE and ALTER on every start. */
        run(conn, "INSERT INTO mig_test_a (id) VALUES (1);");

        CHECK(schema_migrate(conn, k_migrations, all, TEST_COMPONENT),
              "the second run succeeds");

        PGresult* res = PQexec(conn, "SELECT COUNT(*) FROM mig_test_a");
        int rows = (PQresultStatus(res) == PGRES_TUPLES_OK)
                 ? atoi(PQgetvalue(res, 0, 0)) : -1;
        PQclear(res);
        CHECK(rows == 1, "and the data it would have recreated is untouched");
    }

    printf("\nTEST 3: a partial run is applied up to the failure and no further\n");
    {
        clean(conn);

        CHECK(!schema_migrate(conn, k_broken,
                              sizeof(k_broken) / sizeof(k_broken[0]),
                              TEST_COMPONENT),
              "a run containing a bad step fails");

        int version = -1;
        schema_current_version(conn, TEST_COMPONENT, &version);
        CHECK(version == 1, "the version stops at the last step that worked");
        CHECK(table_exists(conn, "mig_test_a"), "that step's change is committed");
        CHECK(!table_exists(conn, "mig_test_should_not_exist"),
              "and nothing after the failure ran");
    }

    printf("\nTEST 4: a resumed run picks up where it stopped\n");
    {
        CHECK(schema_migrate(conn, k_migrations, all, TEST_COMPONENT),
              "the fixed set runs to completion");

        int version = -1;
        schema_current_version(conn, TEST_COMPONENT, &version);
        CHECK(version == 3, "and reaches the newest version");
        CHECK(table_exists(conn, "mig_test_b"),
              "the step after the earlier failure has now run");
    }

    printf("\nTEST 5: an older build refuses a database a newer one migrated\n");
    {
        /* The database is at 3; offer it a build that only knows 1 and 2. This
         * is the case that silently destroys data without a version table:
         * the old binary sees tables it recognises and carries on. */
        CHECK(!schema_migrate(conn, k_migrations, 2, TEST_COMPONENT),
              "the older build refuses to run");

        int version = -1;
        schema_current_version(conn, TEST_COMPONENT, &version);
        CHECK(version == 3, "and changes nothing");
    }

    printf("\nTEST 6: the newest migration can be rolled back\n");
    {
        CHECK(schema_rollback_one(conn, k_migrations, all, TEST_COMPONENT),
              "the rollback succeeds");

        int version = -1;
        schema_current_version(conn, TEST_COMPONENT, &version);
        CHECK(version == 2, "the version drops by one");
        CHECK(!column_exists(conn, "mig_test_a", "extra"),
              "and the change is undone");

        CHECK(schema_migrate(conn, k_migrations, all, TEST_COMPONENT),
              "and it can be applied again");
        CHECK(column_exists(conn, "mig_test_a", "extra"), "restoring the change");
    }

    printf("\nTEST 7: a migration with no rollback refuses to be undone\n");
    {
        static const SchemaMigration irreversible[] = {
            { .version = 1, .name = "first",
              .up = "CREATE TABLE IF NOT EXISTS mig_test_a (id INTEGER PRIMARY KEY);",
              .down = "DROP TABLE IF EXISTS mig_test_a;" },
            { .version = 2, .name = "second",
              .up = "CREATE TABLE IF NOT EXISTS mig_test_b (id INTEGER PRIMARY KEY);",
              .down = "DROP TABLE IF EXISTS mig_test_b;" },
            { .version = 3, .name = "one way only",
              .up = "ALTER TABLE mig_test_a ADD COLUMN IF NOT EXISTS extra TEXT;",
              .down = NULL },
        };

        CHECK(!schema_rollback_one(conn, irreversible, 3, TEST_COMPONENT),
              "a step that declares no rollback is refused");

        int version = -1;
        schema_current_version(conn, TEST_COMPONENT, &version);
        CHECK(version == 3, "and the version is unchanged");
    }

    clean(conn);
    PQfinish(conn);

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}

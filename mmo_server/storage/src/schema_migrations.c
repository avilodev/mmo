/**
 * @file
 * Apply numbered schema migrations, one transaction each, recorded as they go.
 */

#include "schema_migrations.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Advisory lock key, so concurrent starts against one database serialise.
 *
 * An arbitrary constant, but a *stable* one: every process that migrates this
 * schema must ask for the same key or the lock does nothing. Session-scoped
 * (pg_advisory_lock, not xact) because the run spans several transactions.
 */
#define SCHEMA_LOCK_KEY 0x4D4D4F53L   /* "MMOS" */

/** Run one statement, reporting the failure. @return 1 on success. */
static int exec_ok(PGconn* conn, const char* sql, const char* what) {
    PGresult* res = PQexec(conn, sql);
    ExecStatusType status = PQresultStatus(res);
    int ok = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);
    if (!ok) LOG_ERROR("[SCHEMA] %s failed: %s", what, PQerrorMessage(conn));
    PQclear(res);
    return ok;
}

/** Take or release the schema lock. @return 1 on success. */
static int advisory_lock(PGconn* conn, int take) {
    /* Formatted from the constant rather than written out twice: a lock key
     * that appears as a literal in one place and a #define in another is a
     * lock that silently stops working the day the two disagree. */
    char sql[96];
    snprintf(sql, sizeof(sql), "SELECT pg_advisory_%s(%ld)",
             take ? "lock" : "unlock", SCHEMA_LOCK_KEY);
    return exec_ok(conn, sql,
                   take ? "taking the schema advisory lock"
                        : "releasing the schema advisory lock");
}

/** Create the version table if this database has never been migrated. */
static int ensure_version_table(PGconn* conn) {
    return exec_ok(conn,
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "    component  TEXT        NOT NULL,"
        "    version    INTEGER     NOT NULL,"
        "    name       TEXT        NOT NULL,"
        "    applied_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
        "    PRIMARY KEY (component, version)"
        ");",
        "creating schema_version");
}

int schema_current_version(PGconn* conn, const char* component, int* out_version) {
    if (!conn || !component || !out_version) return 0;

    const char* params[1] = { component };
    PGresult* res = PQexecParams(conn,
        "SELECT COALESCE(MAX(version), 0) FROM schema_version WHERE component = $1",
        1, NULL, params, NULL, NULL, 0);

    int ok = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1);
    if (ok) *out_version = atoi(PQgetvalue(res, 0, 0));
    else    LOG_ERROR("[SCHEMA] could not read the current version: %s",
                      PQerrorMessage(conn));
    PQclear(res);
    return ok;
}

/** Apply one migration and record it, all inside one transaction. */
static int apply_one(PGconn* conn, const SchemaMigration* m, const char* component) {
    LOG_INFO("[SCHEMA] %s: applying migration %d (%s)",
             component, m->version, m->name);

    if (!exec_ok(conn, "BEGIN", "BEGIN")) return 0;

    if (!exec_ok(conn, m->up, m->name)) {
        exec_ok(conn, "ROLLBACK", "ROLLBACK");
        return 0;
    }

    char version_str[32];
    snprintf(version_str, sizeof(version_str), "%d", m->version);
    const char* params[3] = { component, version_str, m->name };

    PGresult* res = PQexecParams(conn,
        "INSERT INTO schema_version (component, version, name) VALUES ($1, $2, $3)",
        3, NULL, params, NULL, NULL, 0);
    int recorded = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!recorded)
        LOG_ERROR("[SCHEMA] could not record migration %d: %s",
                  m->version, PQerrorMessage(conn));
    PQclear(res);

    if (!recorded) {
        exec_ok(conn, "ROLLBACK", "ROLLBACK");
        return 0;
    }

    /* The change and its record commit together or not at all. That is the
     * whole point of doing this in a transaction: a schema that moved without
     * its version moving is exactly the state that has no recovery. */
    if (!exec_ok(conn, "COMMIT", "COMMIT")) {
        exec_ok(conn, "ROLLBACK", "ROLLBACK");
        return 0;
    }

    return 1;
}

int schema_migrate(PGconn* conn, const SchemaMigration* migrations, size_t count,
                   const char* component) {
    if (!conn || !migrations || count == 0 || !component) return 0;

    /* One migrator at a time. Ten world servers start together and several
     * may share a database; without this they would each run the same CREATE
     * and ALTER concurrently and deadlock against each other on the catalog. */
    if (!advisory_lock(conn, 1)) return 0;

    int ok = 0;
    do {
        if (!ensure_version_table(conn)) break;

        int current = 0;
        if (!schema_current_version(conn, component, &current)) break;

        int newest = migrations[count - 1].version;

        if (current > newest) {
            /* The database has been migrated by a newer binary. Running an old
             * one against it is how a rollback quietly becomes data loss, so
             * refuse and say what to do about it. */
            LOG_ERROR("[SCHEMA] %s is at version %d but this build only knows "
                      "up to %d. Deploy the newer build, or roll the schema "
                      "back deliberately.", component, current, newest);
            break;
        }

        if (current == newest) {
            LOG_INFO("[SCHEMA] %s is at version %d, up to date", component, current);
            ok = 1;
            break;
        }

        LOG_INFO("[SCHEMA] %s is at version %d, migrating to %d",
                 component, current, newest);

        int applied = 0;
        ok = 1;
        for (size_t i = 0; i < count; i++) {
            if (migrations[i].version <= current) continue;
            if (!apply_one(conn, &migrations[i], component)) {
                /* Stop at the first failure. Everything before it is
                 * committed and recorded, so a rerun resumes exactly here
                 * rather than starting over. */
                LOG_ERROR("[SCHEMA] %s stopped at migration %d; the database is "
                          "at version %d", component, migrations[i].version,
                          current + applied);
                ok = 0;
                break;
            }
            applied++;
        }

        if (ok) LOG_INFO("[SCHEMA] %s migrated to version %d (%d step(s))",
                         component, newest, applied);
    } while (0);

    advisory_lock(conn, 0);
    return ok;
}

int schema_rollback_one(PGconn* conn, const SchemaMigration* migrations,
                        size_t count, const char* component) {
    if (!conn || !migrations || count == 0 || !component) return 0;

    if (!advisory_lock(conn, 1)) return 0;

    int ok = 0;
    do {
        int current = 0;
        if (!schema_current_version(conn, component, &current)) break;
        if (current == 0) {
            LOG_ERROR("[SCHEMA] %s has no applied migration to roll back", component);
            break;
        }

        const SchemaMigration* top = NULL;
        for (size_t i = 0; i < count; i++)
            if (migrations[i].version == current) { top = &migrations[i]; break; }

        if (!top) {
            LOG_ERROR("[SCHEMA] %s is at version %d, which this build does not "
                      "define; it cannot roll it back", component, current);
            break;
        }
        if (!top->down) {
            LOG_ERROR("[SCHEMA] migration %d (%s) cannot be undone: it declares "
                      "no rollback", top->version, top->name);
            break;
        }

        LOG_WARN("[SCHEMA] %s: rolling back migration %d (%s)",
                 component, top->version, top->name);

        if (!exec_ok(conn, "BEGIN", "BEGIN")) break;
        if (!exec_ok(conn, top->down, "rollback statements")) {
            exec_ok(conn, "ROLLBACK", "ROLLBACK");
            break;
        }

        char version_str[32];
        snprintf(version_str, sizeof(version_str), "%d", top->version);
        const char* params[2] = { component, version_str };
        PGresult* res = PQexecParams(conn,
            "DELETE FROM schema_version WHERE component = $1 AND version = $2",
            2, NULL, params, NULL, NULL, 0);
        int cleared = (PQresultStatus(res) == PGRES_COMMAND_OK);
        PQclear(res);

        if (!cleared || !exec_ok(conn, "COMMIT", "COMMIT")) {
            exec_ok(conn, "ROLLBACK", "ROLLBACK");
            break;
        }

        LOG_WARN("[SCHEMA] %s is now at version %d", component, top->version - 1);
        ok = 1;
    } while (0);

    advisory_lock(conn, 0);
    return ok;
}

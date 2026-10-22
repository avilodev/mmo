#ifndef SCHEMA_MIGRATIONS_H
#define SCHEMA_MIGRATIONS_H

/** @file Versioned, recorded, transactional schema changes.
 *
 * The schema used to be created and altered by a run of `CREATE TABLE IF NOT
 * EXISTS` and ad-hoc `ALTER`s re-executed on every single startup. That has
 * three problems, and all three are the kind that only show up when something
 * has already gone wrong:
 *
 *  - **Nothing recorded what had been applied.** The only way to know what
 *    shape a database was in was to inspect it, and the only way to know what
 *    shape it *should* be in was to read the C source of whichever binary
 *    happened to have started against it last.
 *
 *  - **A failed step was a warning.** Several of the ALTERs logged "Warning:
 *    failed to ..." and carried on, so a database could run for months missing
 *    a column or a constraint that the code assumed was there.
 *
 *  - **There was no way back.** No version to roll back to, no statement to
 *    roll back with, and no record of when anything changed.
 *
 * A migration here is a numbered step with a name, an `up`, and an optional
 * `down`. Steps are applied in order, each inside its own transaction, and each
 * records itself in a `schema_version` table on success. A step that fails
 * rolls back and stops the run; nothing after it is attempted, and startup
 * fails rather than proceeding against a half-migrated database.
 *
 * Concurrency: several world servers may share one database and start at the
 * same time. schema_migrate() takes a PostgreSQL advisory lock for the whole
 * run, so exactly one of them migrates and the rest wait and then find there is
 * nothing to do.
 */

#include <libpq-fe.h>
#include <stddef.h>

/** One numbered schema change. */
typedef struct {
    /** Strictly increasing from 1, with no gaps and no reuse.
     *
     * Never renumber an applied migration and never edit its `up`: databases
     * in the wild have already run it, and the version number is the only
     * record of what they ran. Add a new one instead.
     */
    int         version;

    /** What this step does, in a few words. Stored in schema_version. */
    const char* name;

    /** Statements to apply. Runs as one PQexec, inside one transaction.
     *
     * Write them idempotently (`IF NOT EXISTS`, `IF EXISTS`, guarded `DO`
     * blocks) wherever it is cheap to. The baseline migration in particular
     * must apply cleanly to a database that already has the whole schema,
     * because that is what every existing deployment looks like.
     */
    const char* up;

    /** Statements that undo `up`, or NULL when the step cannot be undone.
     *
     * A migration that drops data honestly has no `down`; say so with NULL
     * rather than writing one that recreates an empty column and calls it a
     * rollback.
     */
    const char* down;
} SchemaMigration;

/**
 * Bring a database up to the newest known migration.
 *
 * @param conn        An open connection. Left outside any transaction.
 * @param migrations  Steps in ascending version order.
 * @param count       How many.
 * @param component   Namespace for the version record, so two subsystems can
 *                    migrate independently in one database.
 * @return            1 when the schema is at the newest version, 0 on failure
 *                    or when the database is newer than this binary knows.
 */
int schema_migrate(PGconn* conn, const SchemaMigration* migrations, size_t count,
                   const char* component);

/**
 * Undo the most recently applied migration.
 *
 * For an operator recovering from a bad deployment, not for startup: nothing
 * calls this automatically. Fails when the top migration has no `down`.
 *
 * @return 1 when one step was rolled back, otherwise 0.
 */
int schema_rollback_one(PGconn* conn, const SchemaMigration* migrations,
                        size_t count, const char* component);

/**
 * Read the version a database is currently at.
 *
 * @param out_version  Receives the version; 0 for a database with no record.
 * @return             1 when the version was read, 0 on query failure.
 */
int schema_current_version(PGconn* conn, const char* component, int* out_version);

#endif // SCHEMA_MIGRATIONS_H

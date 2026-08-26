-- Renumber every item instance into a fresh, collision-free identifier range.
--
-- Run this only when scripts/audit_item_instance_ids.sql reports inverted rows,
-- and only with the world server for this database STOPPED. A running world
-- holds item instances in memory by identifier and would write them back under
-- their old numbers.
--
--     sudo -u postgres psql -d prototype_db -f scripts/repair_item_instance_ids.sql
--
-- What it does, and why this is the repair
-- ----------------------------------------
-- A reissued identifier cannot be undone: the write that landed on the wrong
-- character's row overwrote the only record of what was there. What can be done
-- is to guarantee it never happens again for these rows, and to make the
-- database's own history consistent with the allocator's rule that identifiers
-- increase with creation time.
--
-- Every row is given a new identifier above the current maximum, ordered by
-- created_at so the new numbering agrees with the order the items were made.
-- Ownership, slot, quantity, binding and stats are untouched; only the identity
-- token changes, and nothing outside character_items references it.
--
-- The whole thing is one transaction, and the verification at the end raises
-- rather than reports: a repair that half worked is worse than one that did not
-- run, so the failure has to abort the transaction rather than print a warning
-- above a COMMIT.

\set ON_ERROR_STOP on

BEGIN;

-- Refuse to run against a database another session is writing to. A world
-- server mid-save would have rows locked; taking the table exclusively makes
-- that a clean failure rather than a partial renumber.
LOCK TABLE character_items IN ACCESS EXCLUSIVE MODE;

\echo ''
\echo '--- before ---'
SELECT count(*) AS item_rows,
       COALESCE(min(instance_id), 0) AS min_instance_id,
       COALESCE(max(instance_id), 0) AS max_instance_id
  FROM character_items;

-- Build the mapping first, so the new identifiers are decided from one
-- consistent read rather than recomputed per row.
CREATE TEMP TABLE instance_renumber ON COMMIT DROP AS
SELECT instance_id AS old_id,
       (SELECT COALESCE(max(instance_id), 0) FROM character_items)
           + row_number() OVER (ORDER BY created_at NULLS FIRST, instance_id) AS new_id
  FROM character_items;

\echo ''
\echo '--- mapping built ---'
SELECT count(*)            AS rows_to_renumber,
       COALESCE(min(new_id), 0) AS first_new_id,
       COALESCE(max(new_id), 0) AS last_new_id
  FROM instance_renumber;

-- Every new identifier is strictly greater than every old one, so this single
-- pass can never transiently collide with a row it has not reached yet.
UPDATE character_items ci
   SET instance_id = m.new_id
  FROM instance_renumber m
 WHERE ci.instance_id = m.old_id;

\echo ''
\echo '--- after ---'
SELECT count(*) AS item_rows,
       COALESCE(min(instance_id), 0) AS min_instance_id,
       COALESCE(max(instance_id), 0) AS max_instance_id
  FROM character_items;

\echo ''
\echo '--- verification: identifiers must now increase with creation time ---'

-- Aborts the transaction if anything is still inverted, so a repair that did
-- not achieve what it claims cannot commit.
DO $$
DECLARE
    inversions bigint;
    duplicates bigint;
BEGIN
    WITH ordered AS (
        SELECT instance_id,
               max(instance_id) OVER (
                   ORDER BY created_at
                   ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
               ) AS max_id_created_earlier
          FROM character_items
         WHERE created_at IS NOT NULL
    )
    SELECT count(*) INTO inversions
      FROM ordered
     WHERE max_id_created_earlier IS NOT NULL
       AND instance_id < max_id_created_earlier;

    SELECT count(*) INTO duplicates
      FROM (SELECT instance_id FROM character_items
             GROUP BY instance_id HAVING count(*) > 1) d;

    IF inversions > 0 THEN
        RAISE EXCEPTION
            'renumbering left % inverted row(s); transaction aborted, nothing changed',
            inversions;
    END IF;

    IF duplicates > 0 THEN
        RAISE EXCEPTION
            'renumbering produced % duplicate identifier(s); transaction aborted',
            duplicates;
    END IF;

    RAISE NOTICE 'verification passed: no inversions, no duplicates';
END $$;

COMMIT;

\echo ''
\echo '--- committed. Start the world server; it seeds its allocator above the'
\echo '--- new maximum at startup.'
\echo ''

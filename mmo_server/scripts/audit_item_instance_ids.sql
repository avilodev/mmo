-- Read-only audit for item-instance identifier reuse.
--
-- Why this exists
-- ---------------
-- item_instance_next_id() issues identifiers from a process-local counter that
-- starts at 1. Until the world server was made to seed that counter from
-- character_items_max_instance_id() at startup, every restart began reissuing
-- identifiers that persisted rows already owned.
--
-- The consequence is not a duplicate row -- instance_id is the primary key, so
-- the database will not hold two. It is worse than that: character_items_save()
-- upserts with ON CONFLICT (instance_id) DO UPDATE SET slot, quantity, is_bound
-- and does NOT update character_id. So when character B saves a stack whose
-- reissued identifier belongs to character A, the write lands on A's row: A's
-- item silently becomes a different quantity in a different slot, still owned by
-- A, and B's item is never stored. If that new slot is one A already occupies,
-- UNIQUE (character_id, slot) rejects the statement and B's entire save
-- transaction rolls back instead.
--
-- Run against every world database before the next restart, e.g.
--     sudo -u postgres psql -d prototype_db -f scripts/audit_item_instance_ids.sql
--
-- Nothing is written. If section 3 reports any rows, run
-- scripts/repair_item_instance_ids.sql against that database.

\echo ''
\echo '=== 1. Identifier range currently in use ==================================='

SELECT count(*)                        AS item_rows,
       count(DISTINCT character_id)    AS characters_holding_items,
       COALESCE(min(instance_id), 0)   AS min_instance_id,
       COALESCE(max(instance_id), 0)   AS max_instance_id
  FROM character_items;

\echo ''
\echo '   max_instance_id is the value the world server now seeds its allocator'
\echo '   from at startup. Anything it issues from here on is strictly above it.'

\echo ''
\echo '=== 2. Gaps, which are normal ============================================='
\echo '   Consumed and destroyed items leave holes. A large gap count is not a'
\echo '   defect; it is only shown so section 3 is read in context.'

SELECT COALESCE(max(instance_id), 0) - count(*) AS unused_identifiers_in_range
  FROM character_items;

\echo ''
\echo '=== 3. Generation inversions -- the fingerprint of a reset allocator ======='
\echo '   A single allocator run issues identifiers in increasing order, so a row'
\echo '   created later always carries a higher identifier than every row created'
\echo '   before it. A row that breaks that rule was written by a counter that had'
\echo '   restarted, in a range that was already in use.'
\echo ''
\echo '   Any rows listed here mean this database was exposed to the defect.'

WITH ordered AS (
    SELECT instance_id,
           character_id,
           slot,
           item_id,
           created_at,
           max(instance_id) OVER (
               ORDER BY created_at
               ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
           ) AS max_id_created_earlier
      FROM character_items
     WHERE created_at IS NOT NULL
)
SELECT count(*) AS inverted_rows
  FROM ordered
 WHERE max_id_created_earlier IS NOT NULL
   AND instance_id < max_id_created_earlier;

\echo ''
\echo '   The 50 worst inversions, newest first:'

WITH ordered AS (
    SELECT instance_id,
           character_id,
           slot,
           item_id,
           quantity,
           created_at,
           max(instance_id) OVER (
               ORDER BY created_at
               ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
           ) AS max_id_created_earlier
      FROM character_items
     WHERE created_at IS NOT NULL
)
SELECT instance_id,
       character_id,
       slot,
       item_id,
       quantity,
       created_at,
       max_id_created_earlier,
       max_id_created_earlier - instance_id AS identifiers_behind
  FROM ordered
 WHERE max_id_created_earlier IS NOT NULL
   AND instance_id < max_id_created_earlier
 ORDER BY created_at DESC
 LIMIT 50;

\echo ''
\echo '=== 4. Identifier ranges shared by many characters ========================'
\echo '   Each 1000-identifier block, and how many characters hold rows in it. A'
\echo '   block low in the range that many characters share is where reissued'
\echo '   identifiers would have collided most often.'

SELECT (instance_id / 1000) * 1000        AS block_start,
       count(*)                           AS rows_in_block,
       count(DISTINCT character_id)        AS characters_in_block
  FROM character_items
 GROUP BY 1
HAVING count(DISTINCT character_id) > 1
 ORDER BY 1
 LIMIT 50;

\echo ''
\echo '=== done =================================================================='
\echo 'If section 3 reported any inverted rows, this database issued identifiers'
\echo 'that were already in use. Run scripts/repair_item_instance_ids.sql against'
\echo 'it, with the world server stopped, before starting the world again.'
\echo ''

-- Validates the character_items schema and the save/load round trip against a
-- real Postgres, which the C test suite cannot do (it has no database).
--
-- Run against any world database, e.g.:
--     sudo -u postgres psql -d prototype_db -f scripts/verify_character_items.sql
--
-- It creates nothing permanent: everything happens inside a transaction that
-- is rolled back at the end, so it is safe to run against a live world.

BEGIN;

-- ---------------------------------------------------------------------------
-- The same DDL character_database_init() issues. Running it here proves the
-- statement is valid before a server ever tries it at startup.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS character_items (
    instance_id    BIGINT PRIMARY KEY,
    character_id   INTEGER NOT NULL,
    slot           SMALLINT NOT NULL,
    item_id        INTEGER NOT NULL,
    quantity       SMALLINT NOT NULL DEFAULT 1 CHECK (quantity > 0),
    is_bound       BOOLEAN NOT NULL DEFAULT FALSE,
    durability     SMALLINT,
    max_durability SMALLINT,
    rolled_stats   JSONB,
    created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (character_id, slot)
);

CREATE INDEX IF NOT EXISTS idx_character_items_owner
    ON character_items(character_id);

\echo '--- schema created ---'

-- Use a character id no real row will collide with.
\set CHAR 2147483600

-- ---------------------------------------------------------------------------
-- 1. Insert: two bag stacks and one worn item.
-- ---------------------------------------------------------------------------
INSERT INTO character_items (instance_id, character_id, slot, item_id, quantity, is_bound)
VALUES (900000001, :CHAR,   0, 4000, 25, false),
       (900000002, :CHAR,   1,    1, 60, false),
       (900000003, :CHAR, 200, 2000,  1, true);

SELECT 'after insert' AS step, count(*) AS rows FROM character_items WHERE character_id = :CHAR;

-- ---------------------------------------------------------------------------
-- 2. The quantity CHECK must reject a zero-quantity stack. An empty stack is
--    how a duplication bug hides, so the database refuses to store one.
-- ---------------------------------------------------------------------------
\echo '--- expect: violates check constraint "character_items_quantity_check" ---'
SAVEPOINT s1;
INSERT INTO character_items (instance_id, character_id, slot, item_id, quantity)
VALUES (900000004, :CHAR, 5, 4000, 0);
ROLLBACK TO SAVEPOINT s1;

-- ---------------------------------------------------------------------------
-- 3. The UNIQUE(character_id, slot) constraint must reject two items in one
--    slot. This is the constraint that turns an item dupe into a failed write.
-- ---------------------------------------------------------------------------
\echo '--- expect: duplicate key violates unique constraint ---'
SAVEPOINT s2;
INSERT INTO character_items (instance_id, character_id, slot, item_id, quantity)
VALUES (900000005, :CHAR, 0, 4010, 1);
ROLLBACK TO SAVEPOINT s2;

-- ---------------------------------------------------------------------------
-- 4. The save path: delete anything no longer held, then upsert what is.
--    Here the character has consumed stack ...002, moved ...001 from slot 0 to
--    slot 7, and picked up a new item.
-- ---------------------------------------------------------------------------
DELETE FROM character_items
 WHERE character_id = :CHAR
   AND NOT (instance_id = ANY('{900000001,900000003,900000006}'::bigint[]));

INSERT INTO character_items (instance_id, character_id, slot, item_id, quantity, is_bound)
VALUES (900000001, :CHAR, 7, 4000, 20, false),
       (900000003, :CHAR, 200, 2000, 1, true),
       (900000006, :CHAR, 8, 4020, 3, false)
ON CONFLICT (instance_id) DO UPDATE
    SET slot = EXCLUDED.slot,
        quantity = EXCLUDED.quantity,
        is_bound = EXCLUDED.is_bound;

\echo '--- after save: 900000002 gone, 900000001 moved to slot 7 with qty 20 ---'
SELECT instance_id, slot, item_id, quantity, is_bound
  FROM character_items
 WHERE character_id = :CHAR
 ORDER BY slot;

-- ---------------------------------------------------------------------------
-- 5. The load query.
-- ---------------------------------------------------------------------------
\echo '--- load query ---'
SELECT instance_id, slot, item_id, quantity, is_bound
  FROM character_items WHERE character_id = :CHAR;

-- ---------------------------------------------------------------------------
-- 6. Seeding the id allocator.
-- ---------------------------------------------------------------------------
\echo '--- max instance id (seeds the in-process allocator) ---'
SELECT COALESCE(MAX(instance_id), 0) AS max_instance_id FROM character_items;

ROLLBACK;

\echo '--- rolled back; nothing was kept ---'

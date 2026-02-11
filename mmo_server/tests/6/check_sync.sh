#!/bin/bash

echo "=== DATABASE SYNCHRONIZATION CHECK ==="
echo ""

# Database connection info
DB_HOST="${PG_HOST:-localhost}"
DB_NAME="${PG_DATABASE:-postgres}"
DB_USER="${PG_USER:-postgres}"

echo "[1] Checking character in realm database..."
echo ""

# Check if character exists in realm database
psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" -c "
SELECT 
    c.character_id,
    c.name,
    c.account_id,
    c.world_id,
    c.class_id,
    c.race_id,
    c.level
FROM characters c
WHERE c.character_id = 3;
" 2>&1

echo ""
echo "[2] Checking if world server has access to same database..."
echo ""

# Check table structure
psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" -c "
\dt characters
" 2>&1

echo ""
echo "[3] Checking all characters for account 3..."
echo ""

psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" -c "
SELECT 
    character_id,
    name,
    account_id,
    world_id,
    class_id,
    race_id,
    level,
    created_at
FROM characters
WHERE account_id = 3
ORDER BY character_id;
" 2>&1

echo ""
echo "=== DIAGNOSIS ==="
echo ""
echo "Common Issues:"
echo ""
echo "1. SEPARATE DATABASES"
echo "   - Realm server uses one database"
echo "   - World server uses a different database"
echo "   - Solution: Configure both to use the same database"
echo ""
echo "2. DATABASE PERMISSIONS"
echo "   - World server can't read from characters table"
echo "   - Solution: Grant permissions to world server user"
echo ""
echo "3. WRONG DATABASE CONNECTION"
echo "   - World server connecting to wrong database"
echo "   - Check PG_CONNECTION_STRING environment variable"
echo ""
echo "To fix, check your world server's database connection:"
echo "  \$PG_CONNECTION_STRING"
echo ""
echo "It should match realm server's connection string."
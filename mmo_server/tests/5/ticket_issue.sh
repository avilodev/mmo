#!/bin/bash

echo "=== GAME TICKET DEBUG TOOL ==="
echo ""

# Check if Redis is running
echo "[1] Checking Redis connection..."
if redis-cli ping > /dev/null 2>&1; then
    echo "    ✓ Redis is running"
else
    echo "    ✗ Redis is NOT running - this is the problem!"
    exit 1
fi

echo ""
echo "[2] Checking for game tickets in Redis..."
TICKETS=$(redis-cli KEYS "game_ticket:*")
if [ -z "$TICKETS" ]; then
    echo "    ⚠ No game tickets found in Redis"
    echo "    This means the realm server is not generating tickets properly"
else
    echo "    Found tickets:"
    echo "$TICKETS" | while read ticket; do
        if [ ! -z "$ticket" ]; then
            VALUE=$(redis-cli GET "$ticket")
            TTL=$(redis-cli TTL "$ticket")
            echo "      - $ticket = $VALUE (TTL: ${TTL}s)"
        fi
    done
fi

echo ""
echo "[3] Checking for sessions in Redis..."
SESSIONS=$(redis-cli KEYS "session:*")
if [ -z "$SESSIONS" ]; then
    echo "    ⚠ No sessions found"
else
    echo "    Found $(echo "$SESSIONS" | wc -l) session(s)"
fi

echo ""
echo "[4] Common issues and fixes:"
echo ""
echo "   Issue: 'Failed to enter world' at Step 6"
echo "   Cause: Realm server not generating/storing game tickets"
echo ""
echo "   Check realm server code for:"
echo "   1. Does PACKET_ENTER_WORLD handler create a game ticket?"
echo "   2. Does it store the ticket in Redis with format:"
echo "      SET game_ticket:<ticket> <character_id>:<world_id> EX 60"
echo "   3. Does EnterWorldResponsePacket include the ticket?"
echo ""
echo "   Example realm server code:"
echo "   ----------------------------------------"
echo "   // Generate ticket"
echo "   char ticket[64];"
echo "   generate_game_ticket(ticket, sizeof(ticket));"
echo ""
echo "   // Store in Redis (60 second expiry)"
echo "   char key[128];"
echo "   snprintf(key, sizeof(key), \"game_ticket:%s\", ticket);"
echo ""
echo "   char value[64];"
echo "   snprintf(value, sizeof(value), \"%u:%u\", character_id, world_id);"
echo ""
echo "   redisReply* reply = redisCommand(g_redis, \"SET %s %s EX 60\", key, value);"
echo "   ----------------------------------------"
echo ""

echo "[5] Watching Redis in real-time (Ctrl+C to stop)..."
echo "    Run your test again and watch for ticket creation:"
echo ""
redis-cli MONITOR
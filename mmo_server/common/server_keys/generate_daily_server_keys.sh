#!/bin/bash
# /home/avilo/mmo_server/common/server_keys/generate_daily_server_keys.sh
# Run this as a daily cronjob to rotate server auth keys
# Keys are stored without dates - just rotated daily

# Generate a cryptographically secure random key
generate_key() {
    openssl rand -hex 32
}

# Redis connection
REDIS_CLI="redis-cli"

echo "Rotating server auth keys"

# Generate global key (fallback for all servers)
# NOTE: Using "server_auth_key:" prefix to match code expectations
GLOBAL_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:global" 86400 "$GLOBAL_KEY"
echo "✓ Global key rotated (expires in 24 hours)"

# Per-world keys with capital first letters to match world names in code
ARMEIA_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Armeia" 86400 "$ARMEIA_KEY"
echo "✓ Armeia key rotated"

BOSTEUIS_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Bosteuis" 86400 "$BOSTEUIS_KEY"
echo "✓ Bosteuis key rotated"

CARDINAL_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Cardinal" 86400 "$CARDINAL_KEY"
echo "✓ Cardinal key rotated"

DERIVE_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Derive" 86400 "$DERIVE_KEY"
echo "✓ Derive key rotated"

EXODUS_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Exodus" 86400 "$EXODUS_KEY"
echo "✓ Exodus key rotated"

PROTOTYPE_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Prototype" 86400 "$PROTOTYPE_KEY"
echo "✓ Prototype key rotated"

KARMEL_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Karmel" 86400 "$KARMEL_KEY"
echo "✓ Karmel key rotated"

LONGEVITY_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Longevity" 86400 "$LONGEVITY_KEY"
echo "✓ Longevity key rotated"

NERVOW_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Nervow" 86400 "$NERVOW_KEY"
echo "✓ Nervow key rotated"

JATRUS_KEY=$(generate_key)
$REDIS_CLI SETEX "server_auth_key:Jatrus" 86400 "$JATRUS_KEY"
echo "✓ Jatrus key rotated"

echo ""
echo "Server auth keys rotated successfully"
echo "Keys will auto-expire in 24 hours"
echo "Next rotation: $(date -d '+1 day' +'%Y-%m-%d %H:%M:%S')"
echo ""
echo "Verifying keys in Redis..."
$REDIS_CLI KEYS "server_auth_key:*"
#!/bin/bash

echo "Starting MMO Servers..."

# Start Login Server
echo "Starting Login Server..."
./login_server/bin/login_server &
LOGIN_PID=$!
sleep 1

# Start Realm Server
echo "Starting Realm Server..."
./realm_server/bin/realm_server realm_server/realm_config/realm_1.conf &
#./realm_server/bin/realm_server realm_server/realm_config/realm_2.conf &
REALM_PID=$!
sleep 1

# Start World Servers
echo "Starting World Servers..."
./world_server/bin/world_server world_server/world_config/Armeia.conf &
./world_server/bin/world_server world_server/world_config/Bosteuis.conf &
./world_server/bin/world_server world_server/world_config/Cardinal.conf &
./world_server/bin/world_server world_server/world_config/Derive.conf &
./world_server/bin/world_server world_server/world_config/Exodus.conf &
./world_server/bin/world_server world_server/world_config/Jatrus.conf &
./world_server/bin/world_server world_server/world_config/Karmel.conf &
./world_server/bin/world_server world_server/world_config/Longevity.conf &
./world_server/bin/world_server world_server/world_config/Nervow.conf &
./world_server/bin/world_server world_server/world_config/Prototype.conf &

echo ""
echo "All servers started!"
echo "Login PID: $LOGIN_PID"
echo "Realm PID: $REALM_PID"
echo ""
echo "Use './scripts/stop_servers.sh' to stop all servers"
echo "Or use 'make stop' from project root"
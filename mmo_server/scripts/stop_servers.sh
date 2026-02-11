#!/bin/bash

echo "Stopping all servers..."

pkill -f login_server
pkill -f realm_server
pkill -f world_server

echo "All servers stopped."
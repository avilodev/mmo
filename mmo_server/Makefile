CC = gcc

# Optimization level. Overridable so a debugging session can drop to -O0
# without editing this file:  make OPT=-O0
#
# -fno-strict-aliasing is not optional here, and must move wherever OPT moves.
# The packet paths type-pun a byte buffer into PacketHeader* and friends
# (net_loop.c, routes.c, connection_io.c, and both other services), which is a
# strict-aliasing violation that -O0 happened to hide. Optimizing without this
# flag would let the compiler assume those pointers can never alias the buffer
# and reorder the reads accordingly.
OPT = -O2 -fno-strict-aliasing

# -MMD -MP writes a .d file beside every object listing the headers it read, and
# the -include at the bottom of this file feeds them back to make. Without them
# an edit to protocol.h rebuilt nothing at all: the three services would keep
# their stale objects, agree on a PROTOCOL_VERSION they no longer shared, and
# refuse each other at the handshake with "different version than the server".
CFLAGS = -Wall -Wextra -g $(OPT) -pthread -MMD -MP
LDFLAGS = -pthread -lsqlite3 -lhiredis -lpq -lm

# OpenSSL — used by the login server only
LOGIN_LDFLAGS = -lssl -lcrypto -lsodium

# PostgreSQL include path
PG_INCLUDE = -I/usr/include/postgresql

# Directories
COMMON_DIR = common
LOGIN_DIR = login_server
REALM_DIR = realm_server
WORLD_DIR = world_server
DB_DIR = database

# Output directories
LOGIN_BIN = $(LOGIN_DIR)/bin
LOGIN_OBJ = $(LOGIN_DIR)/obj
REALM_BIN = $(REALM_DIR)/bin
REALM_OBJ = $(REALM_DIR)/obj
WORLD_BIN = $(WORLD_DIR)/bin
WORLD_OBJ = $(WORLD_DIR)/obj
COMMON_BIN = $(COMMON_DIR)/bin
COMMON_OBJ = $(COMMON_DIR)/obj
DB_BIN = $(DB_DIR)/bin
DB_OBJ = $(DB_DIR)/obj
DB_OPS_OBJ = $(DB_DIR)/obj

# Source files
COMMON_SRC = $(wildcard $(COMMON_DIR)/src/*.c)
LOGIN_SRC = $(wildcard $(LOGIN_DIR)/src/*.c)
REALM_SRC = $(wildcard $(REALM_DIR)/src/*.c)
WORLD_SRC = $(wildcard $(WORLD_DIR)/src/*.c)
DB_SRC = $(wildcard $(DB_DIR)/src/*.c)

# Object files
COMMON_OBJS = $(COMMON_SRC:$(COMMON_DIR)/src/%.c=$(COMMON_OBJ)/%.o)
LOGIN_OBJS = $(LOGIN_SRC:$(LOGIN_DIR)/src/%.c=$(LOGIN_OBJ)/%.o)
REALM_OBJS = $(REALM_SRC:$(REALM_DIR)/src/%.c=$(REALM_OBJ)/%.o)
WORLD_OBJS = $(WORLD_SRC:$(WORLD_DIR)/src/%.c=$(WORLD_OBJ)/%.o)
DB_OBJS = $(DB_SRC:$(DB_DIR)/src/%.c=$(DB_OBJ)/%.o)
DB_OPS_OBJS = $(DB_OPS_SRC:$(DB_DIR)/operations/%.c=$(DB_OPS_OBJ)/%.o)

# Executables
LOGIN_EXEC = $(LOGIN_BIN)/login_server
REALM_EXEC = $(REALM_BIN)/realm_server
WORLD_EXEC = $(WORLD_BIN)/world_server

# Build everything
all: $(LOGIN_EXEC) $(REALM_EXEC) $(WORLD_EXEC) data

# Login server (needs database + OpenSSL for TLS)
$(LOGIN_EXEC): $(COMMON_OBJS) $(DB_OBJS) $(LOGIN_OBJS)
	@mkdir -p $(LOGIN_BIN)
	@mkdir -p database
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LOGIN_LDFLAGS)
	@mkdir -p $(LOGIN_BIN)/data
	@cp setup/worlds.conf $(LOGIN_BIN)/data/
	@echo "Built login_server -> $@"

# Realm server (needs PostgreSQL for player_data and character operations)
$(REALM_EXEC): $(COMMON_OBJS) $(REALM_OBJS) $(DB_OBJS)
	@mkdir -p $(REALM_BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LOGIN_LDFLAGS)
	@mkdir -p $(REALM_BIN)/data
	@cp world_server/data/races.json $(REALM_BIN)/data/
	@cp world_server/data/progression.json $(REALM_BIN)/data/
	@# The world roster. One file feeds the realm's world list, every world's
	@# database and every world's display name; it is packaged beside each
	@# binary so a server finds it wherever it was launched from.
	@cp setup/worlds.conf $(REALM_BIN)/data/
	@echo "Built realm_server -> $@"

# World server (needs PostgreSQL and Redis for game state) - FIXED: Added $(DB_OBJS)
$(WORLD_EXEC): $(COMMON_OBJS) $(DB_OBJS) $(WORLD_OBJS)
	@mkdir -p $(WORLD_BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LOGIN_LDFLAGS)
	@echo "Built world_server -> $@"

# Copy all world server data files (always runs so edits to JSON are picked up without a recompile)
#
# world.dat is deliberately NOT copied here. At ~1 GB it took 15s of the ~15.1s
# this target used to cost, on every single `make run`, even when unchanged.
# It is a separate file rule below so make skips it unless the source is newer.
WORLD_DAT_SRC := $(wildcard world_server/data/world.dat)

.PHONY: data
data: $(if $(WORLD_DAT_SRC),$(WORLD_BIN)/data/world.dat)
	@mkdir -p $(WORLD_BIN)/data
	@cp world_server/data/items.json $(WORLD_BIN)/data/
	@cp world_server/data/abilities.json $(WORLD_BIN)/data/
	@cp world_server/data/races.json $(WORLD_BIN)/data/
	@cp world_server/data/progression.json $(WORLD_BIN)/data/
	@# Cleared first: cp -r into an existing directory nests it, and a dialogue
	@# deleted from the source tree would otherwise keep loading from here.
	@rm -rf $(WORLD_BIN)/data/dialogues
	@cp -r world_server/data/dialogues $(WORLD_BIN)/data/
	@cp world_server/data/npc_types.json $(WORLD_BIN)/data/
	@# Same reason the dialogues directory is cleared first: an enemy deleted from
	@# the source tree must stop loading here, and cp -r would nest instead.
	@rm -rf $(WORLD_BIN)/data/npc
	@cp -r world_server/data/npc $(WORLD_BIN)/data/
	@cp world_server/data/spawns.json $(WORLD_BIN)/data/
	@cp world_server/data/attack_profiles.json $(WORLD_BIN)/data/
	@cp world_server/data/quests.json $(WORLD_BIN)/data/
	@cp world_server/data/shops.json $(WORLD_BIN)/data/
	@cp world_server/data/zones.json $(WORLD_BIN)/data/
	@cp setup/worlds.conf $(WORLD_BIN)/data/
	@echo "Data files copied -> $(WORLD_BIN)/data/"

# ~1 GB: copy only when the generated world is actually newer than the deployed
# one. Regenerate it with `make world` in mmo_client.
$(WORLD_BIN)/data/world.dat: world_server/data/world.dat
	@mkdir -p $(WORLD_BIN)/data
	@echo "Copying world.dat (~1 GB, changed since last copy)..."
	@cp $< $@

# Common object files
$(COMMON_OBJ)/%.o: $(COMMON_DIR)/src/%.c
	@mkdir -p $(COMMON_OBJ)
	$(CC) $(CFLAGS) $(PG_INCLUDE) -I$(COMMON_DIR)/include -c $< -o $@

# Database object files
$(DB_OBJ)/%.o: $(DB_DIR)/src/%.c
	@mkdir -p $(DB_OBJ)
	$(CC) $(CFLAGS) $(PG_INCLUDE) -I$(DB_DIR)/include -I$(COMMON_DIR)/include -c $< -o $@

# Database operations object files (for character management)
$(DB_OPS_OBJ)/%.o: $(DB_DIR)/operations/%.c
	@mkdir -p $(DB_OPS_OBJ)
	$(CC) $(CFLAGS) $(PG_INCLUDE) -I$(COMMON_DIR)/include -I$(REALM_DIR)/include -c $< -o $@

# Login server object files
$(LOGIN_OBJ)/%.o: $(LOGIN_DIR)/src/%.c
	@mkdir -p $(LOGIN_OBJ)
	$(CC) $(CFLAGS) $(PG_INCLUDE) -I$(COMMON_DIR)/include -I$(DB_DIR)/include -I$(LOGIN_DIR)/include -c $< -o $@

# Realm server object files (needs PostgreSQL headers)
$(REALM_OBJ)/%.o: $(REALM_DIR)/src/%.c
	@mkdir -p $(REALM_OBJ)
	$(CC) $(CFLAGS) $(PG_INCLUDE) -I$(COMMON_DIR)/include -I$(DB_DIR)/include -I$(REALM_DIR)/include -c $< -o $@

# World server object files (needs PostgreSQL headers)
$(WORLD_OBJ)/%.o: $(WORLD_DIR)/src/%.c
	@mkdir -p $(WORLD_OBJ)
	$(CC) $(CFLAGS) $(PG_INCLUDE) -I$(COMMON_DIR)/include -I$(DB_DIR)/include -I$(WORLD_DIR)/include -c $< -o $@

# ---------------------------------------------------------------------------
# One-command setup
#
# Installs dependencies, starts PostgreSQL and Redis, creates every world
# database, generates the world configuration from setup/worlds.conf, produces
# the login certificate and the launcher pin, and generates world.dat.
#
# Idempotent: re-running reports what was already correct instead of redoing
# it, so it is also how a half-configured machine gets repaired.
# ---------------------------------------------------------------------------
setup:
	@bash setup/setup.sh

# Same checks, changing nothing. Run this when something will not start.
setup-check:
	@bash setup/check.sh

# Regenerate the world .conf files even where they disagree with worlds.conf.
# Overwrites hand edits to the five positional values.
setup-worlds:
	@FORCE_CONFIGS=1 bash setup/setup.sh

# Setup PostgreSQL databases for each world server
setup-postgres:
	@echo "=== Setting up PostgreSQL databases for world servers ==="
	@echo ""
	@echo "North America Region (6 servers)..."
	@sudo -u postgres psql -c "CREATE DATABASE armeia_db;" 2>/dev/null && echo "  ✓ armeia_db created" || echo "  - armeia_db exists"
	@sudo -u postgres psql -c "CREATE DATABASE bosteuis_db;" 2>/dev/null && echo "  ✓ bosteuis_db created" || echo "  - bosteuis_db exists"
	@sudo -u postgres psql -c "CREATE DATABASE cardinal_db;" 2>/dev/null && echo "  ✓ cardinal_db created" || echo "  - cardinal_db exists"
	@sudo -u postgres psql -c "CREATE DATABASE derive_db;" 2>/dev/null && echo "  ✓ derive_db created" || echo "  - derive_db exists"
	@sudo -u postgres psql -c "CREATE DATABASE exodus_db;" 2>/dev/null && echo "  ✓ exodus_db created" || echo "  - exodus_db exists"
	@sudo -u postgres psql -c "CREATE DATABASE prototype_db;" 2>/dev/null && echo "  ✓ prototype_db created" || echo "  - prototype_db exists"
	@echo ""
	@echo "Europe Region (3 servers)..."
	@sudo -u postgres psql -c "CREATE DATABASE karmel_db;" 2>/dev/null && echo "  ✓ karmel_db created" || echo "  - karmel_db exists"
	@sudo -u postgres psql -c "CREATE DATABASE longevity_db;" 2>/dev/null && echo "  ✓ longevity_db created" || echo "  - longevity_db exists"
	@sudo -u postgres psql -c "CREATE DATABASE nervow_db;" 2>/dev/null && echo "  ✓ nervow_db created" || echo "  - nervow_db exists"
	@echo ""
	@echo "Asia Region (1 server)..."
	@sudo -u postgres psql -c "CREATE DATABASE jatrus_db;" 2>/dev/null && echo "  ✓ jatrus_db created" || echo "  - jatrus_db exists"
	@echo ""
	@echo "=== All world databases ready ==="

# init-world-tables is gone deliberately.
#
# It held a second CREATE TABLE for `characters` that had already drifted from
# the one character_database_init() issues -- different UNIQUE constraint, an
# extra pos_z, no first_load. Both use CREATE TABLE IF NOT EXISTS, so whichever
# ran first won silently and the server believed it had created a table it had
# not. The servers own their schema and run their own migrations; setup creates
# the databases and stops there.

# Show all databases and character counts
show-dbs:
	@echo "=== SQLite Databases ==="
	@ls -lh database/*.db 2>/dev/null || echo "No SQLite databases found"
	@echo ""
	@echo "=== PostgreSQL World Databases ==="
	@sudo -u postgres psql -l | grep -E "_db" | grep -v "template" | awk '{print $$1}' || echo "No world databases"
	@echo ""
	@echo "=== Character Counts by World ==="
	@echo "North America:"
	@sudo -u postgres psql -d armeia_db -t -c "SELECT '  Armeia: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Armeia: 0"
	@sudo -u postgres psql -d bosteuis_db -t -c "SELECT '  Bosteuis: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Bosteuis: 0"
	@sudo -u postgres psql -d cardinal_db -t -c "SELECT '  Cardinal: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Cardinal: 0"
	@sudo -u postgres psql -d derive_db -t -c "SELECT '  Derive: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Derive: 0"
	@sudo -u postgres psql -d exodus_db -t -c "SELECT '  Exodus: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Exodus: 0"
	@sudo -u postgres psql -d prototype_db -t -c "SELECT '  Prototype: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Prototype: 0"
	@echo "Europe:"
	@sudo -u postgres psql -d karmel_db -t -c "SELECT '  Karmel: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Karmel: 0"
	@sudo -u postgres psql -d longevity_db -t -c "SELECT '  Longevity: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Longevity: 0"
	@sudo -u postgres psql -d nervow_db -t -c "SELECT '  Nervow: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Nervow: 0"
	@echo "Asia:"
	@sudo -u postgres psql -d jatrus_db -t -c "SELECT '  Jatrus: ' || COUNT(*) FROM characters;" 2>/dev/null || echo "  Jatrus: 0"

# Reset a specific world's character database
reset-world:
	@if [ -z "$(WORLD)" ]; then \
		echo "Usage: make reset-world WORLD=armeia"; \
		echo "Available worlds: armeia, bosteuis, cardinal, derive, exodus, prototype, karmel, longevity, nervow, jatrus"; \
	else \
		sudo -u postgres psql -d $(WORLD)_db -c "TRUNCATE TABLE characters RESTART IDENTITY CASCADE;" && \
		echo "✓ Reset $(WORLD)_db characters" || \
		echo "✗ Failed to reset $(WORLD)_db (does database exist?)"; \
	fi

# Clean everything
clean:
	rm -rf $(COMMON_OBJ) $(COMMON_BIN)
	rm -rf $(DB_OBJ) $(DB_BIN)
	rm -rf $(LOGIN_OBJ) $(LOGIN_BIN)
	rm -rf $(REALM_OBJ) $(REALM_BIN)
	rm -rf $(WORLD_OBJ) $(WORLD_BIN)
	@echo "Cleaned all build artifacts"

# Clean database (use with caution!)
clean-db:
	@echo "=== Cleaning SQLite databases ==="
	rm -f database/databases/users_data/users.db
	rm -f database/databases/postgres_data/realmdata.db
	@echo "=== Dropping PostgreSQL world databases ==="
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS armeia_db;" 2>/dev/null && echo "  ✓ Dropped armeia_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS bosteuis_db;" 2>/dev/null && echo "  ✓ Dropped bosteuis_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS cardinal_db;" 2>/dev/null && echo "  ✓ Dropped cardinal_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS derive_db;" 2>/dev/null && echo "  ✓ Dropped derive_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS exodus_db;" 2>/dev/null && echo "  ✓ Dropped exodus_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS prototype_db;" 2>/dev/null && echo "  ✓ Dropped prototype_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS karmel_db;" 2>/dev/null && echo "  ✓ Dropped karmel_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS longevity_db;" 2>/dev/null && echo "  ✓ Dropped longevity_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS nervow_db;" 2>/dev/null && echo "  ✓ Dropped nervow_db" || true
	@sudo -u postgres psql -c "DROP DATABASE IF EXISTS jatrus_db;" 2>/dev/null && echo "  ✓ Dropped jatrus_db" || true
	@echo "=== All databases cleaned ==="

# Reset all character data (keeps databases, just clears characters)
reset-all-characters:
	@echo "=== Resetting all character data ==="
	@for db in armeia_db bosteuis_db cardinal_db derive_db exodus_db prototype_db karmel_db longevity_db nervow_db jatrus_db; do \
		sudo -u postgres psql -d $$db -c "TRUNCATE TABLE characters RESTART IDENTITY CASCADE;" && \
		echo "  ✓ Reset $$db" || echo "  ✗ Failed $$db"; \
	done
	@echo "=== All character data reset ==="

# Clean and rebuild
rebuild: clean all

# Rebuild every target from scratch and reject all compiler warnings.
.PHONY: strict
strict:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -Werror" all

# ---------------------------------------------------------------------------
# Tests
#
# These run standalone against stubbed dependencies — no PostgreSQL or Redis
# needed, so they are safe to run in CI or before a commit.
# ---------------------------------------------------------------------------
TEST_BIN = tests/bin
TEST_CFLAGS_BASE = $(PG_INCLUDE) -I$(COMMON_DIR)/include -I$(DB_DIR)/include -I$(WORLD_DIR)/include
# -Werror here as well as in `make strict`.
#
# The suite built with warnings on and errors off, which meant a warning in a
# test was a line of scrollback rather than a failure. Converting the realm
# suite to TLS turned `int client = connect_client()` into a pointer truncated
# to an int -- a -Wint-conversion warning, a clean build, and a segfault at
# run time that looked like a bug in the server rather than in the fixture.
# The compiler had already found it and had no way to say so.
TEST_CFLAGS = -Wall -Wextra -Werror -g $(OPT) -pthread $(TEST_CFLAGS_BASE)

# The shared accept path, split by what a reader is looking for. Named once so
# a fifth file is one edit rather than six.
REACTOR_SRC = $(COMMON_DIR)/src/net_reactor.c $(COMMON_DIR)/src/net_reactor_conn.c \
              $(COMMON_DIR)/src/net_reactor_work.c $(COMMON_DIR)/src/net_reactor_loop.c

# The friends and presence feature. Split by which process owns the data: the
# realm owns the durable graph (social_database.c) and drains the mutation
# queue (realm_server/friends.c); every process shares the Redis caches
# (presence.c) and the event bus (friend_bus.c).
#
# Unlike the rest of the suite these want a live Redis. They are written to
# skip the Redis half and still check the encodings when there is none, so a
# machine without one gets a partial pass rather than a failure.
SOCIAL_SRC = $(DB_DIR)/src/social_database.c
PRESENCE_SRC = $(COMMON_DIR)/src/presence.c $(COMMON_DIR)/src/session.c
FRIEND_BUS_SRC = $(COMMON_DIR)/src/friend_bus.c $(PRESENCE_SRC)

# The quest feature is three translation units split by what changes them: the
# authored definitions, one character's tables and their file, and the rules
# joining the two. Named once here so a fourth is one edit, not five.
QUEST_SRC = $(WORLD_DIR)/src/quest_system.c $(WORLD_DIR)/src/quest_registry.c \
            $(WORLD_DIR)/src/quest_storage.c $(WORLD_DIR)/src/quest_repeat.c

.PHONY: test
test: $(TEST_BIN)/packet_limiter_test $(TEST_BIN)/config_limits_test $(TEST_BIN)/net_loop_test $(TEST_BIN)/net_reactor_test $(TEST_BIN)/realm_net_test $(TEST_BIN)/cert_pin_digest_test $(TEST_BIN)/tls_link_test $(TEST_BIN)/login_net_test $(TEST_BIN)/login_concurrency_test $(TEST_BIN)/milestone_save_test $(TEST_BIN)/player_index_test $(TEST_BIN)/session_registry_test $(TEST_BIN)/tick_snapshot_test $(TEST_BIN)/interest_test $(TEST_BIN)/npc_world_test $(TEST_BIN)/broadcast_pool_test $(TEST_BIN)/item_data_test $(TEST_BIN)/item_instance_test $(TEST_BIN)/character_items_db_test $(TEST_BIN)/move_validator_test $(TEST_BIN)/world_collision_test $(TEST_BIN)/spatial_grid_test $(TEST_BIN)/tick_scheduler_test $(TEST_BIN)/json_util_test $(TEST_BIN)/race_registry_test $(TEST_BIN)/dialogue_system_test $(TEST_BIN)/opening_sequence_test $(TEST_BIN)/quest_persistence_test $(TEST_BIN)/quest_repeat_test $(TEST_BIN)/quest_abandon_test $(TEST_BIN)/progression_test $(TEST_BIN)/damage_stacking_test \
      $(TEST_BIN)/stats_test $(TEST_BIN)/passive_gating_test $(TEST_BIN)/cooldown_test $(TEST_BIN)/ability_data_test \
      $(TEST_BIN)/world_regions_test $(TEST_BIN)/ip_allowlist_test $(TEST_BIN)/party_test $(TEST_BIN)/world_table_test $(TEST_BIN)/loot_test $(TEST_BIN)/rate_limiter_test \
      $(TEST_BIN)/metrics_server_test $(TEST_BIN)/schema_migration_test \
      $(TEST_BIN)/shop_trade_test $(TEST_BIN)/log_trace_test \
      $(TEST_BIN)/login_routes_fuzz_test $(TEST_BIN)/realm_routes_fuzz_test \
      $(TEST_BIN)/world_routes_fuzz_test $(TEST_BIN)/chat_routing_test \
      $(TEST_BIN)/inventory_handler_test \
      $(TEST_BIN)/social_database_test $(TEST_BIN)/presence_test \
      $(TEST_BIN)/friend_bus_test $(TEST_BIN)/realm_friends_test \
      $(TEST_BIN)/world_friends_test \
      $(TEST_BIN)/npc_registry_test $(TEST_BIN)/npc_content_test $(TEST_BIN)/npc_behavior_test
	@echo "=== Checking the wire format against PROTOCOL_VERSION ==="
	@./tests/check_protocol_version.sh
	@echo ""
	@echo "=== Checking both protocol.h trees declare the same thing ==="
	@./tests/check_protocol_sync.sh
	@echo ""
	@echo "=== Checking the two trees are from the same release ==="
	@./tests/check_tree_pairing.sh
	@echo ""
	@echo "=== Checking both trees agree on the world.dat layout ==="
	@./tests/check_world_format.sh
	@echo ""
	@echo "=== Checking the client names every NPC type the world spawns ==="
	@./tests/check_client_content.sh
	@echo ""
	@echo "=== Checking scale constants are defined once ==="
	@echo ""
	@echo "=== Checking both trees compute a public-key pin the same way ==="
	@./tests/check_pinning_sync.sh
	@./tests/check_single_definition.sh
	@echo ""
	@echo "=== Running world regions test ==="
	@./$(TEST_BIN)/world_regions_test
	@echo ""
	@echo "=== Running world table test ==="
	@./$(TEST_BIN)/world_table_test
	@echo ""
	@echo "=== Running loot lifecycle test ==="
	@./$(TEST_BIN)/loot_test
	@echo ""
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/shop_trade_test
	@echo ""
	@echo "=== Running rate limiter test ==="
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/rate_limiter_test
	@echo ""
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/metrics_server_test
	@echo ""
	@./$(TEST_BIN)/log_trace_test
	@echo ""
	@echo "=== Running chat routing test ==="
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/chat_routing_test
	@echo ""
	@echo "=== Running inventory handler test ==="
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/inventory_handler_test
	@echo ""
	@echo "=== Running the friends and presence suite ==="
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/social_database_test
	@echo ""
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/presence_test
	@echo ""
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/friend_bus_test
	@echo ""
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/realm_friends_test
	@echo ""
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/world_friends_test
	@echo ""
	@echo "=== Fuzzing the three server packet routers ==="
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/login_routes_fuzz_test
	@echo ""
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/realm_routes_fuzz_test
	@echo ""
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/world_routes_fuzz_test
	@echo ""
	@echo "=== Running JSON parser test ==="
	@./$(TEST_BIN)/json_util_test
	@echo ""
	@echo "=== Running race registry test ==="
	@./$(TEST_BIN)/race_registry_test
	@echo ""
	@echo "=== Running NPC registry test ==="
	@./$(TEST_BIN)/npc_registry_test $(TEST_BIN)/npc_fixtures
	@echo ""
	@echo "=== Running NPC content test (validates the shipped data files) ==="
	@./$(TEST_BIN)/npc_content_test $(WORLD_DIR)/data/npc
	@echo ""
	@echo "=== Running progression test ==="
	@./$(TEST_BIN)/progression_test
	@echo ""
	@echo "=== Running damage stacking test ==="
	@./$(TEST_BIN)/damage_stacking_test
	@echo ""
	@echo "=== Running stats test ==="
	@./$(TEST_BIN)/stats_test
	@echo ""
	@echo "=== Running passive gating test ==="
	@./$(TEST_BIN)/passive_gating_test
	@echo ""
	@echo "=== Running cooldown test ==="
	@./$(TEST_BIN)/cooldown_test
	@echo ""
	@echo "=== Running ability data test ==="
	@./$(TEST_BIN)/ability_data_test
	@echo ""
	@echo "=== Running packet limiter test ==="
	@./$(TEST_BIN)/packet_limiter_test
	@echo ""
	@echo "=== Running move validator test ==="
	@./$(TEST_BIN)/move_validator_test
	@./$(TEST_BIN)/world_collision_test
	@echo ""
	@echo "=== Running spatial grid test ==="
	@./$(TEST_BIN)/spatial_grid_test
	@echo ""
	@echo "=== Running tick scheduler test ==="
	@./$(TEST_BIN)/tick_scheduler_test
	@echo ""
	@echo "=== Running config limits test ==="
	@./$(TEST_BIN)/config_limits_test
	@echo ""
	@echo "=== Running net reactor test ==="
	@./$(TEST_BIN)/net_reactor_test
	@echo ""
	@echo "=== Running realm connection test ==="
	@./$(TEST_BIN)/realm_net_test
	@echo ""
	@echo "=== Running public-key pin digest test ==="
	@./$(TEST_BIN)/cert_pin_digest_test
	@echo ""
	@echo "=== Running realm<->world TLS test ==="
	@./$(TEST_BIN)/tls_link_test
	@echo ""
	@echo "=== Running login connection test ==="
	@./$(TEST_BIN)/login_net_test
	@echo ""
	@echo "=== Running event loop test ==="
	@./$(TEST_BIN)/net_loop_test
	@echo ""
	@echo "=== Running login concurrency test ==="
	@./$(TEST_BIN)/login_concurrency_test
	@./$(TEST_BIN)/milestone_save_test
	@echo ""
	@echo "=== Running player index test ==="
	@./$(TEST_BIN)/player_index_test
	@echo ""
	@echo "=== Running session registry test ==="
	@./$(TEST_BIN)/session_registry_test
	@echo ""
	@echo "=== Running tick snapshot test ==="
	@./$(TEST_BIN)/tick_snapshot_test
	@echo ""
	@echo "=== Running interest broadcast test ==="
	@./$(TEST_BIN)/interest_test
	@echo ""
	@echo "=== Running dialogue system test ==="
	@./$(TEST_BIN)/dialogue_system_test
	@echo ""
	@echo "=== Running quest storage and persistence test ==="
	@./$(TEST_BIN)/quest_persistence_test
	@echo ""
	@echo "=== Running repeatable quest test ==="
	@./$(TEST_BIN)/quest_repeat_test
	@echo ""
	@echo "=== Running quest abandon test ==="
	@./$(TEST_BIN)/quest_abandon_test
	@echo ""
	@echo "=== Running opening sequence content test ==="
	@./$(TEST_BIN)/opening_sequence_test
	@echo ""
	@echo "=== Running NPC pool and snapshot test ==="
	@./$(TEST_BIN)/npc_world_test
	@echo ""
	@echo "=== Running broadcast fan-out pool test ==="
	@./$(TEST_BIN)/broadcast_pool_test
	@echo ""
	@echo "=== Running item data test ==="
	@./$(TEST_BIN)/item_data_test
	@echo ""
	@echo "=== Running item instance test ==="
	@./$(TEST_BIN)/item_instance_test
	@echo ""
	@echo "=== Running character items DB test ==="
	@./$(TEST_BIN)/schema_migration_test
	@echo ""
	@./$(TEST_BIN)/character_items_db_test
	@echo ""
	@echo "=== Running IP allowlist test ==="
	@./$(TEST_BIN)/ip_allowlist_test
	@echo ""
	@echo "=== Running party test ==="
	@./$(TEST_BIN)/party_test
	@echo ""
	@# Last, because it is the slowest: it drives the real supervisor against
	@# stub services and the supervisor notices a dead child once a second.
	@echo "=== Checking one dead service does not take the stack with it ==="
	@./tests/supervisor_restart_test.sh
	@echo ""
	@echo "=== All tests passed ==="

# Guards the realm-handshake admission check on the world's player listener.
# The correlation id: what appears on a log line, and what must never leak
# from one player's thread to another's.
$(TEST_BIN)/social_database_test: tests/social_database_test.c $(SOCIAL_SRC) $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lsqlite3

$(TEST_BIN)/presence_test: tests/presence_test.c $(PRESENCE_SRC) $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lhiredis

$(TEST_BIN)/friend_bus_test: tests/friend_bus_test.c $(FRIEND_BUS_SRC) $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lhiredis

# The realm half end to end: a mutation goes onto the queue, the consumer
# applies it to real SQLite, and the result comes back over a real subscription.
$(TEST_BIN)/realm_friends_test: tests/realm_friends_test.c $(REALM_DIR)/src/friends.c $(SOCIAL_SRC) $(FRIEND_BUS_SRC) $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -I$(REALM_DIR)/include -o $@ $^ -lsqlite3 -lhiredis

# The reverse index that turns "somebody, somewhere, changed presence" into
# "these local sessions care" without scanning the player pool.
$(TEST_BIN)/world_friends_test: tests/world_friends_test.c $(WORLD_DIR)/src/friends.c $(WORLD_DIR)/src/player_data.c $(FRIEND_BUS_SRC) $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lhiredis

$(TEST_BIN)/log_trace_test: tests/log_trace_test.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

$(TEST_BIN)/metrics_server_test: tests/metrics_server_test.c $(COMMON_DIR)/src/metrics_server.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

$(TEST_BIN)/ip_allowlist_test: tests/ip_allowlist_test.c $(COMMON_DIR)/src/ip_allowlist.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# Party membership is decided on network loop threads and read from others, so
# the concurrent cases matter as much as the sequential ones. See party-sanitize.
$(TEST_BIN)/party_test: tests/party_test.c $(WORLD_DIR)/src/party.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# Address bucketing (an IPv6 /64 is one budget, not 2^64 of them) and a table
# that grows instead of refusing whoever hashed into a crowded chain.
$(TEST_BIN)/rate_limiter_test: tests/rate_limiter_test.c $(LOGIN_DIR)/src/rate_limiter.c $(COMMON_DIR)/src/log.c $(COMMON_DIR)/src/peer_addr.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -I$(LOGIN_DIR)/include -o $@ $^

# The ground-item lifecycle: reserve-then-commit, snapshot-by-copy, and a floor
# that grows instead of refusing the 257th drop.
# The shop counter and the money that moves through it. shop_session.c plus
# the currency and inventory helpers -- no player registry, no NPC pool, so
# what is tested is the rules rather than the plumbing around them.
$(TEST_BIN)/shop_trade_test: tests/shop_trade_test.c $(WORLD_DIR)/src/shop_trade.c $(WORLD_DIR)/src/shop_session.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/loot_test: tests/loot_test.c $(WORLD_DIR)/src/loot.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/packet_limiter_test: tests/packet_limiter_test.c $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

$(TEST_BIN)/config_limits_test: tests/config_limits_test.c $(WORLD_DIR)/src/config.c $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/ip_allowlist.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# Drives real loopback sockets through the epoll loops with the database and
# gameplay layers stubbed out.
$(TEST_BIN)/net_reactor_test: tests/net_reactor_test.c $(REACTOR_SRC) $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Its own include order, ahead of TEST_CFLAGS_BASE: the realm and the world both
# ship a routes.h declaring a different process_packet(), and whichever include
# directory comes first wins.
# One fixed certificate against a pin produced by openssl(1) rather than by
# this codebase. See the file header: it is the test the minted-certificate
# suites structurally cannot be.
$(TEST_BIN)/cert_pin_digest_test: tests/cert_pin_digest_test.c $(COMMON_DIR)/src/cert_pin.c $(COMMON_DIR)/src/cert_spki.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lssl -lcrypto

# The mutually authenticated realm<->world link: both pin checks, the
# no-certificate case, and the SSL_pending path the heartbeat loop depends on.
$(TEST_BIN)/tls_link_test: tests/tls_link_test.c $(COMMON_DIR)/src/tls.c $(COMMON_DIR)/src/cert_pin.c $(COMMON_DIR)/src/cert_spki.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lssl -lcrypto -lm

# Drives real TLS handshakes against the realm reactor, so the transport the
# realm actually runs is exercised rather than a plaintext stand-in for it.
$(TEST_BIN)/realm_net_test: tests/realm_net_test.c $(REALM_DIR)/src/realm_net.c $(REACTOR_SRC) $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/net_notify.c $(COMMON_DIR)/src/peer_addr.c $(COMMON_DIR)/src/tls.c $(COMMON_DIR)/src/cert_pin.c $(COMMON_DIR)/src/cert_spki.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) -Wall -Wextra -Werror -g $(OPT) -pthread -I$(REALM_DIR)/include $(PG_INCLUDE) -I$(COMMON_DIR)/include -I$(DB_DIR)/include -o $@ $^ -lssl -lcrypto -lm

# Drives a real TLS handshake across event-loop wakeups, so it links the
# actual OpenSSL path rather than a stand-in for it.
$(TEST_BIN)/login_net_test: tests/login_net_test.c $(LOGIN_DIR)/src/login_net.c $(COMMON_DIR)/src/tls.c $(COMMON_DIR)/src/cert_pin.c $(COMMON_DIR)/src/cert_spki.c $(REACTOR_SRC) $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) -Wall -Wextra -Werror -g $(OPT) -pthread -I$(LOGIN_DIR)/include $(PG_INCLUDE) -I$(COMMON_DIR)/include -I$(DB_DIR)/include -o $@ $^ -lssl -lcrypto -lm

# The one source list behind net_loop_test and both of its sanitizer
# builds. It is a variable because it drifted: session_registry.c and
# shop_session.c were added to the plain rule below and not to the
# sanitizer targets, so `make sanitize` failed to link while `make test`
# stayed green.
NET_LOOP_TEST_SRC = tests/net_loop_test.c $(WORLD_DIR)/src/net_loop.c $(REACTOR_SRC) $(WORLD_DIR)/src/connection_io.c $(WORLD_DIR)/src/session_registry.c $(WORLD_DIR)/src/shop_session.c $(WORLD_DIR)/src/utils.c $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/net_notify.c $(COMMON_DIR)/src/peer_addr.c $(COMMON_DIR)/src/log.c

# session_registry.c is linked rather than stubbed: it is where a duplicate
# login kicks the stale session, which is what TESTs 10-12 exercise. It is
# self-contained, so linking it costs nothing.
$(TEST_BIN)/net_loop_test: $(NET_LOOP_TEST_SRC)
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# The event loop is concurrent code; compiling it is not the same as proving it.
# Run this when touching net_loop.c. TSan needs ASLR off on some kernels.
# The player registry is read by every gameplay thread and written by logins.
# Run this when touching player_data.c. Same ASLR caveat as above.
# ---------------------------------------------------------------------------
# Every sanitizer target, in one command.
#
# They were each invoked by hand, which is to say they were each run by
# whoever remembered. CI runs this.
# ---------------------------------------------------------------------------
.PHONY: sanitize
sanitize: player-index-sanitize session-registry-sanitize party-sanitize \
          npc-world-sanitize npc-behavior-sanitize interest-sanitize broadcast-pool-sanitize \
          net-reactor-sanitize net-loop-sanitize tick-scheduler-sanitize \
          spatial-grid-sanitize routes-fuzz-sanitize loot-sanitize \
          shop-sanitize
	@echo ""
	@echo "=== All sanitizer suites clean ==="

# The three routers under AddressSanitizer and UBSan.
#
# The plain run above proves the corpus executes. This is the run that proves
# it means something: every fixture is a heap block of exactly the length being
# passed, so a router that reads one byte past what it was handed is a heap
# overflow here rather than a read of whatever the previous fixture left
# behind. -fsanitize=bounds-strict catches an index past a fixed array even
# when that array is a field inside a larger packet, which is the shape every
# one of these reads has.
#
# -fno-sanitize-recover=all makes the first violation the exit status rather
# than a line in output nobody reads.
.PHONY: routes-fuzz-sanitize
routes-fuzz-sanitize:
	@echo "=== The three packet routers under ASan + UBSan ==="
	@mkdir -p $(TEST_BIN)
	$(CC) $(ROUTE_FUZZ_SAN) $(ROUTE_FUZZ_LOGIN) -o $(TEST_BIN)/login_routes_fuzz_asan $(ROUTE_FUZZ_LOGIN_SRC)
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/login_routes_fuzz_asan > /tmp/login_routes_asan.log 2>&1 \
	  && echo "  login router clean" || { tail -40 /tmp/login_routes_asan.log; exit 1; }
	$(CC) $(ROUTE_FUZZ_SAN) $(ROUTE_FUZZ_REALM) -o $(TEST_BIN)/realm_routes_fuzz_asan $(ROUTE_FUZZ_REALM_SRC)
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/realm_routes_fuzz_asan > /tmp/realm_routes_asan.log 2>&1 \
	  && echo "  realm router clean" || { tail -40 /tmp/realm_routes_asan.log; exit 1; }
	$(CC) $(ROUTE_FUZZ_SAN) $(ROUTE_FUZZ_WORLD) -o $(TEST_BIN)/world_routes_fuzz_asan $(ROUTE_FUZZ_WORLD_SRC)
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/world_routes_fuzz_asan > /tmp/world_routes_asan.log 2>&1 \
	  && echo "  world router clean" || { tail -40 /tmp/world_routes_asan.log; exit 1; }

.PHONY: player-index-sanitize
player-index-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer (concurrent logins vs. scanning readers) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/player_index_tsan tests/login_concurrency_test.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/player_index_tsan > /tmp/player_index_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/player_index_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/player_index_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan (index probing and bounds) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/player_index_asan tests/player_index_test.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/player_index_asan > /tmp/player_index_asan.log 2>&1 && echo "  clean" || \
	  { tail -40 /tmp/player_index_asan.log; exit 1; }

# The session registry is written by every login and read by lookups on other
# threads. Run this when touching session_registry.c. Same ASLR caveat as above.
.PHONY: session-registry-sanitize
session-registry-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer (concurrent logins vs. identifier lookups) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/session_registry_tsan tests/session_registry_test.c $(WORLD_DIR)/src/session_registry.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/session_registry_tsan > /tmp/session_registry_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/session_registry_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/session_registry_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan (index probing and bounds) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/session_registry_asan tests/session_registry_test.c $(WORLD_DIR)/src/session_registry.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/session_registry_asan > /tmp/session_registry_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR: |runtime error|leak' /tmp/session_registry_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

# Party membership is mutated from network loop threads while other threads read
# it, and its lookups used to hand out pointers into recyclable pool slots. Run
# this when touching party.c. Same ASLR caveat as the targets above.
.PHONY: party-sanitize
party-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer (concurrent accepts vs. party queries) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/party_tsan tests/party_test.c $(WORLD_DIR)/src/party.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/party_tsan > /tmp/party_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/party_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/party_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan (pool bounds and slot recycling) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/party_asan tests/party_test.c $(WORLD_DIR)/src/party.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/party_asan > /tmp/party_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR: |runtime error|leak' /tmp/party_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

# Ground items are reserved by network workers and expired by the simulation
# thread, and a pickup moves an item from a pool guarded by one lock into a
# player guarded by another. That is where an item duplication bug lives, and
# it was the largest gameplay file with no sanitizer coverage at all. Run this
# when touching loot.c. Same ASLR caveat as the targets above.
.PHONY: loot-sanitize
loot-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer (eight players racing one floor of loot) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/loot_tsan tests/loot_test.c $(WORLD_DIR)/src/loot.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c -lm
	@MMO_LOG_LEVEL=error setarch `uname -m` -R ./$(TEST_BIN)/loot_tsan > /tmp/loot_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/loot_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/loot_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan (pool growth, slot reuse, snapshots) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/loot_asan tests/loot_test.c $(WORLD_DIR)/src/loot.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c -lm
	@MMO_LOG_LEVEL=error setarch `uname -m` -R ./$(TEST_BIN)/loot_asan > /tmp/loot_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR: |runtime error|leak' /tmp/loot_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

# Buying and selling move coin and items in one breath, from worker threads,
# against a session table those same threads open and close. A trade that
# debited without delivering -- or delivered twice for one debit -- is the same
# class of bug as duplicated loot and had the same amount of coverage: none.
# Run this when touching shop_trade.c or shop_session.c.
.PHONY: shop-sanitize
shop-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer (concurrent purchases and sales at one counter) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/shop_tsan tests/shop_trade_test.c $(WORLD_DIR)/src/shop_trade.c $(WORLD_DIR)/src/shop_session.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c -lm
	@MMO_LOG_LEVEL=error setarch `uname -m` -R ./$(TEST_BIN)/shop_tsan > /tmp/shop_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/shop_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/shop_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan (session table churn and inventory bounds) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/shop_asan tests/shop_trade_test.c $(WORLD_DIR)/src/shop_trade.c $(WORLD_DIR)/src/shop_session.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c -lm
	@MMO_LOG_LEVEL=error setarch `uname -m` -R ./$(TEST_BIN)/shop_asan > /tmp/shop_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR: |runtime error|leak' /tmp/shop_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

# The NPC pool is read by every gameplay phase and written by spawns, deaths,
# and respawns, all concurrently. Run this when touching npc_world.c or
# npc_snapshot.c. Same ASLR caveat as the targets above.
.PHONY: npc-world-sanitize
npc-behavior-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== AddressSanitizer + UBSan (NPC effects, mitigation, triggers, one tick per type) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/npc_behavior_asan tests/npc_behavior_test.c \
	  $(WORLD_DIR)/src/npc_effects.c $(WORLD_DIR)/src/npc_mitigation.c \
	  $(WORLD_DIR)/src/npc_triggers.c $(WORLD_DIR)/src/npc_delivery.c \
	  $(WORLD_DIR)/src/npc_geometry.c $(WORLD_DIR)/src/npc_behavior.c \
	  $(WORLD_DIR)/src/npc_ai.c $(WORLD_DIR)/src/npc_summon.c \
	  $(WORLD_DIR)/src/npc_ai_profile.c $(WORLD_DIR)/src/npc_world.c \
	  $(WORLD_DIR)/src/npc_snapshot.c $(WORLD_DIR)/src/spatial_grid.c \
	  $(NPC_REGISTRY_SRC) $(COMMON_DIR)/src/damage_model.c \
	  $(COMMON_DIR)/src/progression.c $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/log.c -lm
	@MMO_LOG_LEVEL=error ./$(TEST_BIN)/npc_behavior_asan $(WORLD_DIR)/data/npc > /tmp/npc_behavior_asan.log 2>&1 \
	  && echo "  NPC runtime clean under ASan + UBSan" \
	  || { tail -40 /tmp/npc_behavior_asan.log; exit 1; }
	@echo ""

npc-world-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer (concurrent damage vs. snapshots vs. churn) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/npc_world_tsan tests/npc_world_test.c $(WORLD_DIR)/src/npc_world.c $(WORLD_DIR)/src/npc_snapshot.c $(WORLD_DIR)/src/spatial_grid.c $(COMMON_DIR)/src/log.c -lm
	@setarch `uname -m` -R ./$(TEST_BIN)/npc_world_tsan > /tmp/npc_world_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/npc_world_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/npc_world_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan (pool bounds and slot recycling) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/npc_world_asan tests/npc_world_test.c $(WORLD_DIR)/src/npc_world.c $(WORLD_DIR)/src/npc_snapshot.c $(WORLD_DIR)/src/spatial_grid.c $(COMMON_DIR)/src/log.c -lm
	@setarch `uname -m` -R ./$(TEST_BIN)/npc_world_asan > /tmp/npc_world_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR|runtime error|leak' /tmp/npc_world_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

# interest_collect_fds() takes the player registry read lock and each candidate's
# slot lock while gameplay threads hold both. Run this when touching interest.c.
# Same ASLR caveat as the targets above.
.PHONY: interest-sanitize
interest-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== AddressSanitizer + UBSan (radius maths and output bounds) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/interest_asan tests/interest_test.c $(WORLD_DIR)/src/interest.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c -lm
	@setarch `uname -m` -R ./$(TEST_BIN)/interest_asan > /tmp/interest_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR|runtime error|leak' /tmp/interest_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

# The fan-out pool runs the same function on several threads at once. Run this
# when touching broadcast_pool.c. Same ASLR caveat as the targets above.
.PHONY: broadcast-pool-sanitize
broadcast-pool-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer (shard coverage under concurrency) ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/broadcast_pool_tsan tests/broadcast_pool_test.c $(WORLD_DIR)/src/broadcast_pool.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/broadcast_pool_tsan > /tmp/broadcast_pool_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/broadcast_pool_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/broadcast_pool_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/broadcast_pool_asan tests/broadcast_pool_test.c $(WORLD_DIR)/src/broadcast_pool.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/broadcast_pool_asan > /tmp/broadcast_pool_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR|runtime error|leak' /tmp/broadcast_pool_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

.PHONY: net-reactor-sanitize
# The reactor is the accept path for all three services, so it is the one piece
# of this tree where a data race is a production incident rather than a test
# failure. Run this on every change to it.
net-reactor-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/net_reactor_tsan tests/net_reactor_test.c $(REACTOR_SRC) $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/net_reactor_tsan > /tmp/net_reactor_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/net_reactor_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/net_reactor_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/net_reactor_asan tests/net_reactor_test.c $(REACTOR_SRC) $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/net_reactor_asan > /tmp/net_reactor_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR|runtime error|leak' /tmp/net_reactor_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

.PHONY: net-loop-sanitize
net-loop-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/net_loop_tsan $(NET_LOOP_TEST_SRC)
	@setarch `uname -m` -R ./$(TEST_BIN)/net_loop_tsan > /tmp/net_loop_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/net_loop_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/net_loop_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/net_loop_asan $(NET_LOOP_TEST_SRC)
	@setarch `uname -m` -R ./$(TEST_BIN)/net_loop_asan > /tmp/net_loop_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR|runtime error|leak' /tmp/net_loop_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

$(TEST_BIN)/login_concurrency_test: tests/login_concurrency_test.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# The two save cadences: which players each pass takes, and which bits it
# leaves behind. Same stub shape as login_concurrency_test -- the database is
# a recorder, because what is under test is the selection, not the write.
$(TEST_BIN)/milestone_save_test: tests/milestone_save_test.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# Round-trips character_items through a real PostgreSQL. Skips itself (exit 0)
# unless MMO_TEST_PGCONN is set, so the suite stays green without a database.
$(TEST_BIN)/schema_migration_test: tests/schema_migration_test.c $(DB_DIR)/src/schema_migrations.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lpq

$(TEST_BIN)/character_items_db_test: tests/character_items_db_test.c $(DB_DIR)/src/players_database.c $(DB_DIR)/src/schema_migrations.c $(DB_DIR)/src/class_stats.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/progression.c $(COMMON_DIR)/src/json_util.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lpq -lm

# Covers item instances and stacking. Every assertion is a conservation check:
# an item duplication bug shows up as a change in the total.
$(TEST_BIN)/item_instance_test: tests/item_instance_test.c $(WORLD_DIR)/src/item_instance.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# --- The three server packet routers, fuzzed -------------------------------
#
# The client's four dispatchers have been fuzzed since the P1 pass; the
# server's three had nothing, which is the wrong way round -- the client parses
# bytes from a server its user chose, and the server parses bytes from anyone
# who can reach the port. Each of these links its router against recording
# stubs and checks, per opcode, that no handler is entered with a buffer
# shorter than the packet it is written against. See tests/routes_fuzz.h.
#
# Three binaries rather than one because the realm and world routers both
# export process_packet() with different signatures, and login and realm each
# carry their own routes.h.
#
# gnu11 for the world router: npc_world.h holds a pthread_rwlock_t, which is
# not visible under strict ISO C.
# Chat routing: who each channel reaches, and who it does not. The real chat.c
# including its dispatch thread; the player registry and the socket are
# replaced, so what is measured is recipient selection.
$(TEST_BIN)/chat_routing_test: tests/chat_routing_test.c $(WORLD_DIR)/src/chat.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -std=gnu11 -o $@ $^ -lm

# The five handlers that move a player's belongings. The real handlers, the
# real inventory model and the real item registry; player_level.c is stubbed
# rather than linked, because an inventory move does not touch class tables or
# the experience curve.
$(TEST_BIN)/inventory_handler_test: tests/inventory_handler_test.c $(WORLD_DIR)/src/packet_handler.c $(WORLD_DIR)/src/items_database.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -std=gnu11 -o $@ $^ -lm

# All three services name their dispatcher's header "routes.h", so the search
# path has to carry exactly one of them: TEST_CFLAGS_BASE includes the world's
# include directory, and a login or realm fuzzer built with it silently picks
# up the world's routes.h and its process_packet() signature. These flag sets
# name only what each fuzzer needs.
ROUTE_FUZZ_BASE   = -Itests $(PG_INCLUDE) -I$(COMMON_DIR)/include -I$(DB_DIR)/include
ROUTE_FUZZ_LOGIN  = -I$(LOGIN_DIR)/include $(ROUTE_FUZZ_BASE)
ROUTE_FUZZ_REALM  = -I$(REALM_DIR)/include $(ROUTE_FUZZ_BASE)
# gnu11 for the world: npc_world.h holds a pthread_rwlock_t, which strict ISO C
# does not expose.
ROUTE_FUZZ_WORLD  = -std=gnu11 -I$(WORLD_DIR)/include $(ROUTE_FUZZ_BASE)

# -fsanitize=bounds-strict catches an index past a fixed array even when that
# array is a field inside a larger packet, which is the shape every one of
# these reads has; plain ASan does not see it, because the read never leaves
# the enclosing struct. -fno-sanitize-recover=all makes the first violation the
# exit status rather than a line in output nobody reads.
ROUTE_FUZZ_SAN = -Wall -Wextra -g -O1 -pthread \
                 -fsanitize=address,undefined -fsanitize=bounds-strict \
                 -fno-sanitize-recover=all

ROUTE_FUZZ_LOGIN_SRC = tests/login_routes_fuzz_test.c $(LOGIN_DIR)/src/routes.c \
                       $(COMMON_DIR)/src/log.c
ROUTE_FUZZ_REALM_SRC = tests/realm_routes_fuzz_test.c $(REALM_DIR)/src/routes.c \
                       $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c \
                       $(COMMON_DIR)/src/net_notify.c $(COMMON_DIR)/src/log.c
ROUTE_FUZZ_WORLD_SRC = tests/world_routes_fuzz_test.c $(WORLD_DIR)/src/routes.c \
                       $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c \
                       $(COMMON_DIR)/src/net_notify.c $(COMMON_DIR)/src/log.c

$(TEST_BIN)/login_routes_fuzz_test: $(ROUTE_FUZZ_LOGIN_SRC)
	@mkdir -p $(TEST_BIN)
	$(CC) -Wall -Wextra -Werror -g $(OPT) -pthread $(ROUTE_FUZZ_LOGIN) -o $@ $^

$(TEST_BIN)/realm_routes_fuzz_test: $(ROUTE_FUZZ_REALM_SRC)
	@mkdir -p $(TEST_BIN)
	$(CC) -Wall -Wextra -Werror -g $(OPT) -pthread $(ROUTE_FUZZ_REALM) -o $@ $^

$(TEST_BIN)/world_routes_fuzz_test: $(ROUTE_FUZZ_WORLD_SRC)
	@mkdir -p $(TEST_BIN)
	$(CC) -Wall -Wextra -Werror -g $(OPT) -pthread $(ROUTE_FUZZ_WORLD) -o $@ $^

# Covers item definition parsing and shop content validation, including the
# shipped data files so a bad content edit fails here rather than in play.
# Covers the shared JSON parser every data file is read through: nested and
# repeated keys, escape decoding, total accessors, and rejection of malformed input.
$(TEST_BIN)/json_util_test: tests/json_util_test.c $(COMMON_DIR)/src/json_util.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the composed NPC content model: that unresolved references fail the load
# rather than being skipped, that an override may retune a scalar but not change an
# ability's shape, that each validation rule actually fires, and -- the two the
# design turns on -- that nothing caps how many types load and that per-NPC sizing
# is read off the content instead of a #define.
NPC_REGISTRY_SRC = $(WORLD_DIR)/src/npc_registry.c $(WORLD_DIR)/src/npc_registry_load.c \
                   $(WORLD_DIR)/src/npc_registry_types.c \
                   $(WORLD_DIR)/src/npc_content_validate.c

$(TEST_BIN)/npc_registry_test: tests/npc_registry_test.c $(NPC_REGISTRY_SRC) $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Runs every rule over the data files the server actually ships, so a content typo
# fails here rather than as an enemy standing inert in a live world.
$(TEST_BIN)/npc_content_test: tests/npc_content_test.c $(NPC_REGISTRY_SRC) $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the race registry and, with it, contract M1: adding a race is a data
# change. The eleventh-race fixture is the executable form of that promise.
$(TEST_BIN)/dialogue_system_test: tests/dialogue_system_test.c $(WORLD_DIR)/src/dialogue_system.c $(WORLD_DIR)/src/dialogue_loader.c $(QUEST_SRC) $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Walks the opening sequence against live servers. Not in `make test`: it needs
# the login, realm and world servers, PostgreSQL and Redis actually running.
#   make tests/bin/opening_flow_test && ./tests/bin/opening_flow_test wolf
# Points real concurrent clients at running servers and reports the latency
# distribution at each junction. Not in `make test`: it needs the three servers
# up, and it is a load generator.
#   make tests/bin/load_test && ./tests/bin/load_test realm 32 20
$(TEST_BIN)/load_test: tests/load_test.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lssl -lcrypto -lm

$(TEST_BIN)/opening_flow_test: tests/opening_flow_test.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lssl -lcrypto -lm

$(TEST_BIN)/quest_persistence_test: tests/quest_persistence_test.c $(QUEST_SRC) $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/quest_repeat_test: tests/quest_repeat_test.c $(QUEST_SRC) $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/quest_abandon_test: tests/quest_abandon_test.c $(QUEST_SRC) $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/opening_sequence_test: tests/opening_sequence_test.c $(QUEST_SRC) $(WORLD_DIR)/src/dialogue_system.c $(WORLD_DIR)/src/dialogue_loader.c $(WORLD_DIR)/src/npc_world.c $(WORLD_DIR)/src/npc_spawns.c $(WORLD_DIR)/src/npc_mitigation.c $(WORLD_DIR)/src/npc_geometry.c $(WORLD_DIR)/src/npc_effects.c $(WORLD_DIR)/src/npc_snapshot.c $(WORLD_DIR)/src/spatial_grid.c $(COMMON_DIR)/src/damage_model.c $(COMMON_DIR)/src/progression.c $(WORLD_DIR)/src/world_collision.c $(NPC_REGISTRY_SRC) $(WORLD_DIR)/src/items_database.c $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/race_registry_test: tests/race_registry_test.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the level and experience curve and, with it, contract M3: retuning never
# needs a recompile.
$(TEST_BIN)/progression_test: tests/progression_test.c $(COMMON_DIR)/src/progression.c $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/world_regions_test: tests/world_regions_test.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The world roster: parsing, growth past any fixed ceiling, lookups, and the
# failure modes that used to be silent when the mapping was compiled in.
$(TEST_BIN)/world_table_test: tests/world_table_test.c $(COMMON_DIR)/src/world_table.c $(COMMON_DIR)/src/data_paths.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the percentage-stacking rule all mitigation runs through: additive
# stacking, order independence across every permutation, and the immunity clamp.
$(TEST_BIN)/damage_stacking_test: tests/damage_stacking_test.c $(COMMON_DIR)/src/damage_model.c $(COMMON_DIR)/src/progression.c $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The stat model, the derived values it produces, and the role-to-resource mapping.
BLESSED_CORE_SRC = $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/progression.c \
                   $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/damage_model.c \
                   $(DB_DIR)/src/class_stats.c
BLESSED_PLAYER_SRC = $(BLESSED_CORE_SRC) $(WORLD_DIR)/src/player_effects.c \
                     $(WORLD_DIR)/src/items_database.c $(WORLD_DIR)/src/item_instance.c \
                     $(COMMON_DIR)/src/log.c

$(TEST_BIN)/stats_test: tests/stats_test.c $(BLESSED_PLAYER_SRC)
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Contract D10: the race passive resolves in Animal Form and nowhere else, which is
# what makes Human Form provably identical across all ten races.
$(TEST_BIN)/passive_gating_test: tests/passive_gating_test.c $(BLESSED_PLAYER_SRC)
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Contract D7: cooldowns are absolute expiry instants, so they tick in both forms and
# no swap sequence can shorten one.
$(TEST_BIN)/cooldown_test: tests/cooldown_test.c $(BLESSED_PLAYER_SRC)
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The joins between races.json and abilities.json, and a round-trip of every new
# effect primitive through the loader.
$(TEST_BIN)/ability_data_test: tests/ability_data_test.c $(WORLD_DIR)/src/ability_def.c $(BLESSED_CORE_SRC) $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/item_data_test: tests/item_data_test.c $(WORLD_DIR)/src/items_database.c $(WORLD_DIR)/src/shop.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/json_util.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the gameplay tick's player snapshot and interest grid: correctness
# against brute force, nearest-first ordering, and that query cost stops
# scaling with population.
$(TEST_BIN)/broadcast_pool_test: tests/broadcast_pool_test.c $(WORLD_DIR)/src/broadcast_pool.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# The runtime half: effects, mitigation and triggers against the shipped content.
# The deferred queue's far end is stubbed in the test -- every action an ability
# takes reaches outside the NPC pool, and a test of the decision has no business
# linking the projectile pool and the player registry to watch it.
$(TEST_BIN)/npc_behavior_test: tests/npc_behavior_test.c $(WORLD_DIR)/src/npc_effects.c \
    $(WORLD_DIR)/src/npc_mitigation.c $(WORLD_DIR)/src/npc_triggers.c \
    $(WORLD_DIR)/src/npc_delivery.c $(WORLD_DIR)/src/npc_geometry.c \
    $(WORLD_DIR)/src/npc_behavior.c $(WORLD_DIR)/src/npc_ai.c \
    $(WORLD_DIR)/src/npc_summon.c \
    $(WORLD_DIR)/src/npc_ai_profile.c $(WORLD_DIR)/src/npc_world.c \
    $(WORLD_DIR)/src/npc_snapshot.c $(WORLD_DIR)/src/spatial_grid.c \
    $(NPC_REGISTRY_SRC) $(COMMON_DIR)/src/damage_model.c \
    $(COMMON_DIR)/src/progression.c $(COMMON_DIR)/src/json_util.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/npc_world_test: tests/npc_world_test.c $(WORLD_DIR)/src/npc_world.c $(WORLD_DIR)/src/npc_snapshot.c $(WORLD_DIR)/src/spatial_grid.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/tick_snapshot_test: tests/tick_snapshot_test.c $(WORLD_DIR)/src/tick_snapshot.c $(WORLD_DIR)/src/spatial_grid.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -O2 -o $@ $^ -lm

# Covers who an unscheduled world event is broadcast to. Cheap to get wrong in a
# way no two-player test notices; see the file header.
$(TEST_BIN)/interest_test: tests/interest_test.c $(WORLD_DIR)/src/interest.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -O2 -o $@ $^ -lm

# Covers the character_id -> slot index: correctness, the ABA guard on a
# recycled slot, and that lookup cost stays flat as the table fills.
$(TEST_BIN)/player_index_test: tests/player_index_test.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -O2 -o $@ $^

# Covers the descriptor-indexed session table: the three lookup routes, the
# stale-login kick, and index churn hard enough to force a rebuild.
$(TEST_BIN)/session_registry_test: tests/session_registry_test.c $(WORLD_DIR)/src/session_registry.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -O2 -o $@ $^

# Drives the movement rules against a synthetic collision map — no sockets, no
# database, so this runs anywhere.
# The collision loader against synthetic world.dat files: what it accepts, what
# it refuses, and that the layer is mapped rather than copied per world. The
# truncation case matters more since the mapping went in -- a file shorter than
# its header claims used to be a short read and would now be a SIGBUS on the
# simulation thread.
$(TEST_BIN)/world_collision_test: tests/world_collision_test.c $(WORLD_DIR)/src/world_collision.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/move_validator_test: tests/move_validator_test.c $(WORLD_DIR)/src/move_validator.c $(WORLD_DIR)/src/world_collision.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Checks interest management against a brute-force oracle — the O(n^2) scan it
# replaces must agree with it exactly, including result ordering.
$(TEST_BIN)/spatial_grid_test: tests/spatial_grid_test.c $(WORLD_DIR)/src/spatial_grid.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Checks multi-rate deadline scheduling against a synthetic clock: arbitrary
# rates, missed-tick handling, and phase alignment after a stall.
$(TEST_BIN)/tick_scheduler_test: tests/tick_scheduler_test.c $(WORLD_DIR)/src/tick_scheduler.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# Deadline arithmetic and the flat-array grid are both easy to get subtly wrong
# in ways a passing test will not show. Run this when touching either.
.PHONY: tick-scheduler-sanitize
tick-scheduler-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== AddressSanitizer + UBSan ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/tick_scheduler_asan tests/tick_scheduler_test.c $(WORLD_DIR)/src/tick_scheduler.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/tick_scheduler_asan > /tmp/tick_scheduler_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR|runtime error|leak' /tmp/tick_scheduler_asan.log | head -40; exit 1; }

.PHONY: spatial-grid-sanitize
spatial-grid-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== AddressSanitizer + UBSan ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/spatial_grid_asan tests/spatial_grid_test.c $(WORLD_DIR)/src/spatial_grid.c -lm
	@setarch `uname -m` -R ./$(TEST_BIN)/spatial_grid_asan > /tmp/spatial_grid_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR|runtime error|leak' /tmp/spatial_grid_asan.log | head -40; exit 1; }

# Kept as an alias so existing muscle memory and the older README still work.
setup-all: setup
	@echo "=== Setup complete (setup-all is an alias for 'make setup') ==="

# Run Login server
run-login: $(LOGIN_EXEC)
	./$(LOGIN_EXEC)

# Run Realm server
#
# Provisions the server-to-server key first, the same way start_servers.sh does.
# The realm refuses to start without one and no longer invents a default, so
# without this the standalone target fails on a fresh Redis. The key generated
# here is random per run -- this is not a fallback credential, it is the same
# provisioning step the supervised path performs.
run-realm: $(REALM_EXEC)
	@if [ "`redis-cli EXISTS server_auth_key:global 2>/dev/null`" != "1" ]; then \
	    echo "No server auth key in Redis; generating one..."; \
	    bash common/server_keys/generate_daily_server_keys.sh >/dev/null || \
	        { echo "Could not generate Redis server keys"; exit 1; }; \
	fi
	./$(REALM_EXEC) realm_server/realm_config/realm_1.conf

# Run World server
run-world: $(WORLD_EXEC)
	./$(WORLD_EXEC) world_server/world_config/Armeia.conf

# Run the complete stack under one foreground supervisor. Ctrl+C performs a
# graceful exact-PID shutdown; no broad pkill is used.
run-all: all
	@bash scripts/start_servers.sh

run: run-all

# Runs all 3
run-both: run-all

# Stop all servers
stop:
	@bash scripts/stop_servers.sh

# Help
help:
	@echo "Available targets:"
	@echo ""
	@echo "Build:"
	@echo "  make all              - Build all servers (default)"
	@echo "  make clean            - Remove all build artifacts"
	@echo "  make rebuild          - Clean and rebuild everything"
	@echo ""
	@echo "Database Setup:"
	@echo "  make setup            - One-command setup: deps, services, databases, configs, certs"
	@echo "  make setup-check      - Verify this machine can run the stack (changes nothing)"
	@echo "  make setup-worlds     - Regenerate world configs from setup/worlds.conf"
	@echo "  make setup-postgres   - Create all world databases only"
	@echo "  make setup-all        - Alias for 'make setup'"
	@echo ""
	@echo "Database Management:"
	@echo "  make show-dbs         - Show all databases and character counts"
	@echo "  make reset-world WORLD=armeia - Reset one world's characters"
	@echo "  make reset-all-characters     - Reset all world characters"
	@echo "  make clean-db         - Delete all databases (CAUTION!)"
	@echo ""
	@echo "Run Servers:"
	@echo "  make run-login        - Build and run login server"
	@echo "  make run-realm        - Build and run realm server"
	@echo "  make run-world        - Build and run world server"
	@echo "  make run-all          - Build and run all servers"
	@echo "  make run              - Same as run-all; Ctrl+C stops the full stack"
	@echo "  make stop             - Gracefully stop a supervised stack"
	@echo ""
	@echo "World Servers:"
	@echo "  North America: armeia, bosteuis, cardinal, derive, exodus, prototype"
	@echo "  Europe: karmel, longevity, nervow"
	@echo "  Asia: jatrus"

.PHONY: all data clean clean-db rebuild run-login run-realm run-world run-all run run-both stop help \
        setup setup-check setup-worlds setup-postgres show-dbs reset-world reset-all-characters setup-all

# Header dependencies generated by -MMD above. The dash keeps a clean tree, or
# one whose objects predate this, from failing on the missing files.
DEPFILES = $(COMMON_OBJS:.o=.d) $(LOGIN_OBJS:.o=.d) $(REALM_OBJS:.o=.d) \
           $(WORLD_OBJS:.o=.d) $(DB_OBJS:.o=.d)
-include $(DEPFILES)

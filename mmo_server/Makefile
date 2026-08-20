CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
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
	@echo "Built login_server -> $@"

# Realm server (needs PostgreSQL for player_data and character operations)
$(REALM_EXEC): $(COMMON_OBJS) $(REALM_OBJS) $(DB_OBJS)
	@mkdir -p $(REALM_BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LOGIN_LDFLAGS)
	@mkdir -p $(REALM_BIN)/data
	@cp world_server/data/races.json $(REALM_BIN)/data/
	@cp world_server/data/progression.json $(REALM_BIN)/data/
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
	@cp -r world_server/data/dialogues $(WORLD_BIN)/data/
	@cp world_server/data/npc_types.json $(WORLD_BIN)/data/
	@cp world_server/data/spawns.json $(WORLD_BIN)/data/
	@cp world_server/data/attack_profiles.json $(WORLD_BIN)/data/
	@cp world_server/data/quests.json $(WORLD_BIN)/data/
	@cp world_server/data/shops.json $(WORLD_BIN)/data/
	@cp world_server/data/zones.json $(WORLD_BIN)/data/
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

# Initialize tables in each world database
init-world-tables:
	@echo "=== Initializing character tables in each world database ==="
	@for db in armeia_db bosteuis_db cardinal_db derive_db exodus_db prototype_db karmel_db longevity_db nervow_db jatrus_db; do \
		echo "Initializing $$db..."; \
		sudo -u postgres psql -d $$db -c "CREATE TABLE IF NOT EXISTS characters ( \
			character_id SERIAL PRIMARY KEY, \
			account_id INTEGER NOT NULL, \
			world_id INTEGER NOT NULL, \
			name VARCHAR(32) NOT NULL UNIQUE, \
			class_id INTEGER NOT NULL, \
			race_id INTEGER NOT NULL, \
			level INTEGER DEFAULT 1, \
			pos_x REAL DEFAULT 0.0, \
			pos_y REAL DEFAULT 0.0, \
			pos_z REAL DEFAULT 0.0, \
			health INTEGER DEFAULT 100, \
			max_health INTEGER DEFAULT 100, \
			experience BIGINT DEFAULT 0, \
			gold INTEGER DEFAULT 0, \
			created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP \
		);" && echo "  ✓ $$db tables created"; \
		sudo -u postgres psql -d $$db -c "CREATE INDEX IF NOT EXISTS idx_characters_account ON characters(account_id);"; \
	done
	@echo "=== All tables initialized ==="

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
TEST_CFLAGS = -Wall -Wextra -g -pthread $(TEST_CFLAGS_BASE)

.PHONY: test
test: $(TEST_BIN)/packet_limiter_test $(TEST_BIN)/config_limits_test $(TEST_BIN)/net_loop_test $(TEST_BIN)/login_concurrency_test $(TEST_BIN)/player_index_test $(TEST_BIN)/session_registry_test $(TEST_BIN)/tick_snapshot_test $(TEST_BIN)/item_data_test $(TEST_BIN)/item_instance_test $(TEST_BIN)/character_items_db_test $(TEST_BIN)/move_validator_test $(TEST_BIN)/spatial_grid_test $(TEST_BIN)/tick_scheduler_test $(TEST_BIN)/json_util_test $(TEST_BIN)/race_registry_test $(TEST_BIN)/progression_test $(TEST_BIN)/damage_stacking_test \
      $(TEST_BIN)/stats_test $(TEST_BIN)/passive_gating_test $(TEST_BIN)/cooldown_test $(TEST_BIN)/ability_data_test \
      $(TEST_BIN)/world_regions_test
	@echo "=== Running world regions test ==="
	@./$(TEST_BIN)/world_regions_test
	@echo ""
	@echo "=== Running JSON parser test ==="
	@./$(TEST_BIN)/json_util_test
	@echo ""
	@echo "=== Running race registry test ==="
	@./$(TEST_BIN)/race_registry_test
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
	@echo "=== Running event loop test ==="
	@./$(TEST_BIN)/net_loop_test
	@echo ""
	@echo "=== Running login concurrency test ==="
	@./$(TEST_BIN)/login_concurrency_test
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
	@echo "=== Running item data test ==="
	@./$(TEST_BIN)/item_data_test
	@echo ""
	@echo "=== Running item instance test ==="
	@./$(TEST_BIN)/item_instance_test
	@echo ""
	@echo "=== Running character items DB test ==="
	@./$(TEST_BIN)/character_items_db_test
	@echo ""
	@echo "=== All tests passed ==="

$(TEST_BIN)/packet_limiter_test: tests/packet_limiter_test.c $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

$(TEST_BIN)/config_limits_test: tests/config_limits_test.c $(WORLD_DIR)/src/config.c $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# Drives real loopback sockets through the epoll loops with the database and
# gameplay layers stubbed out.
$(TEST_BIN)/net_loop_test: tests/net_loop_test.c $(WORLD_DIR)/src/net_loop.c $(WORLD_DIR)/src/connection_io.c $(WORLD_DIR)/src/utils.c $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/net_notify.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# The event loop is concurrent code; compiling it is not the same as proving it.
# Run this when touching net_loop.c. TSan needs ASLR off on some kernels.
# The player registry is read by every gameplay thread and written by logins.
# Run this when touching player_data.c. Same ASLR caveat as above.
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

.PHONY: net-loop-sanitize
net-loop-sanitize:
	@mkdir -p $(TEST_BIN)
	@echo "=== ThreadSanitizer ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=thread -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/net_loop_tsan tests/net_loop_test.c $(WORLD_DIR)/src/net_loop.c $(WORLD_DIR)/src/connection_io.c $(WORLD_DIR)/src/utils.c $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/net_notify.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/net_loop_tsan > /tmp/net_loop_tsan.log 2>&1; \
	  races=`grep -c 'data race' /tmp/net_loop_tsan.log`; \
	  echo "  data races: $$races"; \
	  [ "$$races" = "0" ] || { grep -A 12 'data race' /tmp/net_loop_tsan.log | head -40; exit 1; }
	@echo "=== AddressSanitizer + UBSan ==="
	$(CC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -pthread $(TEST_CFLAGS_BASE) \
	  -o $(TEST_BIN)/net_loop_asan tests/net_loop_test.c $(WORLD_DIR)/src/net_loop.c $(WORLD_DIR)/src/connection_io.c $(WORLD_DIR)/src/utils.c $(COMMON_DIR)/src/packet_limiter.c $(COMMON_DIR)/src/limit_profiles.c $(COMMON_DIR)/src/net_notify.c $(COMMON_DIR)/src/log.c
	@setarch `uname -m` -R ./$(TEST_BIN)/net_loop_asan > /tmp/net_loop_asan.log 2>&1 && echo "  clean" || \
	  { grep -A 8 -E 'ERROR|runtime error|leak' /tmp/net_loop_asan.log | head -40; exit 1; }
	@echo "=== sanitizers clean ==="

$(TEST_BIN)/login_concurrency_test: tests/login_concurrency_test.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# Round-trips character_items through a real PostgreSQL. Skips itself (exit 0)
# unless MMO_TEST_PGCONN is set, so the suite stays green without a database.
$(TEST_BIN)/character_items_db_test: tests/character_items_db_test.c $(DB_DIR)/src/players_database.c $(DB_DIR)/src/class_stats.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/progression.c $(COMMON_DIR)/src/json_util.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lpq -lm

# Covers item instances and stacking. Every assertion is a conservation check:
# an item duplication bug shows up as a change in the total.
$(TEST_BIN)/item_instance_test: tests/item_instance_test.c $(WORLD_DIR)/src/item_instance.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# Covers item definition parsing and shop content validation, including the
# shipped data files so a bad content edit fails here rather than in play.
# Covers the shared JSON parser every data file is read through: nested and
# repeated keys, escape decoding, total accessors, and rejection of malformed input.
$(TEST_BIN)/json_util_test: tests/json_util_test.c $(COMMON_DIR)/src/json_util.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the race registry and, with it, contract M1: adding a race is a data
# change. The eleventh-race fixture is the executable form of that promise.
$(TEST_BIN)/race_registry_test: tests/race_registry_test.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/json_util.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the level and experience curve and, with it, contract M3: retuning never
# needs a recompile.
$(TEST_BIN)/progression_test: tests/progression_test.c $(COMMON_DIR)/src/progression.c $(COMMON_DIR)/src/json_util.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/world_regions_test: tests/world_regions_test.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the percentage-stacking rule all mitigation runs through: additive
# stacking, order independence across every permutation, and the immunity clamp.
$(TEST_BIN)/damage_stacking_test: tests/damage_stacking_test.c $(COMMON_DIR)/src/damage_model.c $(COMMON_DIR)/src/progression.c $(COMMON_DIR)/src/json_util.c
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
$(TEST_BIN)/ability_data_test: tests/ability_data_test.c $(WORLD_DIR)/src/ability_def.c $(BLESSED_CORE_SRC)
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

$(TEST_BIN)/item_data_test: tests/item_data_test.c $(WORLD_DIR)/src/items_database.c $(WORLD_DIR)/src/shop.c $(WORLD_DIR)/src/item_instance.c $(COMMON_DIR)/src/log.c $(COMMON_DIR)/src/race_registry.c $(COMMON_DIR)/src/json_util.c
	@mkdir -p $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Covers the gameplay tick's player snapshot and interest grid: correctness
# against brute force, nearest-first ordering, and that query cost stops
# scaling with population.
$(TEST_BIN)/tick_snapshot_test: tests/tick_snapshot_test.c $(WORLD_DIR)/src/tick_snapshot.c $(WORLD_DIR)/src/spatial_grid.c $(WORLD_DIR)/src/player_data.c $(COMMON_DIR)/src/log.c
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
$(TEST_BIN)/move_validator_test: tests/move_validator_test.c $(WORLD_DIR)/src/move_validator.c $(WORLD_DIR)/src/world_collision.c
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

# Full setup (databases + tables)
setup-all: setup-postgres init-world-tables
	@echo "=== Full database setup complete ==="

# Run Login server
run-login: $(LOGIN_EXEC)
	./$(LOGIN_EXEC)

# Run Realm server
run-realm: $(REALM_EXEC)
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
	@echo "  make setup-postgres   - Create all world databases"
	@echo "  make init-world-tables - Create character tables in all databases"
	@echo "  make setup-all        - Full setup (databases + tables)"
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

.PHONY: all data clean clean-db rebuild run-login run-realm run-world run-all run run-both stop help setup-postgres init-world-tables show-dbs reset-world reset-all-characters setup-all

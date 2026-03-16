CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDFLAGS = -pthread -lsqlite3 -lhiredis -lpq -lm

# OpenSSL — used by the login server only
LOGIN_LDFLAGS = -lssl -lcrypto

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
all: $(LOGIN_EXEC) $(REALM_EXEC) $(WORLD_EXEC)

# Login server (needs database + OpenSSL for TLS)
$(LOGIN_EXEC): $(COMMON_OBJS) $(DB_OBJS) $(LOGIN_OBJS)
	@mkdir -p $(LOGIN_BIN)
	@mkdir -p database
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LOGIN_LDFLAGS)
	@echo "Built login_server -> $@"

# Realm server (needs PostgreSQL for player_data and character operations)
$(REALM_EXEC): $(COMMON_OBJS) $(REALM_OBJS) $(DB_OBJS)
	@mkdir -p $(REALM_BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built realm_server -> $@"

# World server (needs PostgreSQL and Redis for game state) - FIXED: Added $(DB_OBJS)
$(WORLD_EXEC): $(COMMON_OBJS) $(DB_OBJS) $(WORLD_OBJS)
	@mkdir -p $(WORLD_BIN)
	@mkdir -p $(WORLD_BIN)/data
	@cp world_server/data/items.json $(WORLD_BIN)/data/
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built world_server -> $@"

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

# Full setup (databases + tables)
setup-all: setup-postgres init-world-tables
	@echo "=== Full database setup complete ==="

# Run Login server
run-login: $(LOGIN_EXEC)
	./$(LOGIN_EXEC)

# Run Realm server
run-realm: $(REALM_EXEC)
	./$(REALM_EXEC)

# Run World server
run-world: $(WORLD_EXEC)
	./$(WORLD_EXEC)

# Run all servers (in background)
run-all: $(LOGIN_EXEC) $(REALM_EXEC) $(WORLD_EXEC)
	@echo "Starting login server..."
	./$(LOGIN_EXEC) &
	@sleep 1
	@echo "Starting realm server..."
	./$(REALM_EXEC) &
	@sleep 1
	@echo "Starting world server..."
	./$(WORLD_EXEC) &
	@echo "All servers running. Use 'make stop' to stop them."

# Runs all 3
run-both: run-all

# Stop all servers
stop:
	@pkill -f login_server || true
	@pkill -f realm_server || true
	@pkill -f world_server || true
	@echo "All servers stopped"

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
	@echo "  make stop             - Stop all running servers"
	@echo ""
	@echo "World Servers:"
	@echo "  North America: armeia, bosteuis, cardinal, derive, exodus, prototype"
	@echo "  Europe: karmel, longevity, nervow"
	@echo "  Asia: jatrus"

.PHONY: all clean clean-db rebuild run-login run-realm run-world run-all run-both stop help setup-postgres init-world-tables show-dbs reset-world reset-all-characters setup-all
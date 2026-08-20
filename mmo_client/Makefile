# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2

# OpenSSL for the Launcher (login server TLS).
# MSYS2/MinGW64: pacman -S mingw-w64-x86_64-openssl
# OpenSSL — statically linked so the .exe runs on any Windows PC
# without needing OpenSSL DLLs installed.
# Requires: pacman -S mingw-w64-x86_64-openssl (MSYS2/MinGW64)
# Verify static libs exist: ls /mingw64/lib/libssl.a /mingw64/lib/libcrypto.a
OPENSSL_INCLUDE = -I/mingw64/include
OPENSSL_LIB     = -L/mingw64/lib
# -Wl,-Bstatic  : link ssl+crypto from .a archives (baked into .exe)
# -Wl,-Bdynamic : switch back to dynamic for everything after this
# -lcrypt32     : Windows cert store API required by OpenSSL static build
# Static OpenSSL block must come first; ws2_32/gdi32/crypt32 come after so
# the linker can resolve OpenSSL's Winsock references when it processes them.
OPENSSL_LDFLAGS = -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic

LAUNCHER_LDFLAGS = $(OPENSSL_LDFLAGS) -lws2_32 -lgdi32 -lmsimg32 -lcrypt32
GAME_LDFLAGS = -lglfw3 -lopengl32 -lgdi32 -lws2_32 -lwinmm -lm

COMMON_DIR = common

# GLFW paths (for Game only)
GLFW_INCLUDE = -I$(GAME_INCLUDE_DIR)
GLFW_LIB = -L$(GAME_INCLUDE_DIR)/../lib

# Launcher directories
LAUNCHER_SRC_DIR = Launcher/src
LAUNCHER_OBJ_DIR = Launcher/obj
LAUNCHER_BIN_DIR = Launcher/bin
LAUNCHER_INCLUDE_DIR = Launcher/include

# Game directories
GAME_SRC_DIR = Game/src
GAME_OBJ_DIR = Game/obj
GAME_BIN_DIR = Game/bin
GAME_INCLUDE_DIR = Game/include

# Targets
LAUNCHER_TARGET = $(LAUNCHER_BIN_DIR)/Launcher.exe
GAME_TARGET = $(GAME_BIN_DIR)/Game.exe

# ============================================================================
# SOURCE FILES
# ============================================================================

COMMON_SRC = $(wildcard $(COMMON_DIR)/*.c)
LAUNCHER_SOURCES = $(wildcard $(LAUNCHER_SRC_DIR)/*.c)

# Game sources - all subdirectories
GAME_SOURCES = \
    $(wildcard $(GAME_SRC_DIR)/*.c) \
    $(wildcard $(GAME_SRC_DIR)/camera/*.c) \
    $(wildcard $(GAME_SRC_DIR)/audio/*.c) \
    $(wildcard $(GAME_SRC_DIR)/combat/*.c) \
    $(wildcard $(GAME_SRC_DIR)/core/*.c) \
    $(wildcard $(GAME_SRC_DIR)/game/*.c) \
    $(wildcard $(GAME_SRC_DIR)/input/*.c) \
    $(wildcard $(GAME_SRC_DIR)/network/*.c) \
    $(wildcard $(GAME_SRC_DIR)/player/*.c) \
    $(wildcard $(GAME_SRC_DIR)/render/*.c) \
    $(wildcard $(GAME_SRC_DIR)/states/*.c) \
    $(wildcard $(GAME_SRC_DIR)/texture/*.c) \
    $(wildcard $(GAME_SRC_DIR)/utils/*.c) \
    $(wildcard $(GAME_SRC_DIR)/world/*.c) \
	$(wildcard $(GAME_SRC_DIR)/ui/*.c)

# ============================================================================
# OBJECT FILES
# ============================================================================

COMMON_LAUNCHER_OBJECTS = $(patsubst $(COMMON_DIR)/%.c,$(LAUNCHER_OBJ_DIR)/common_%.o,$(COMMON_SRC))
COMMON_GAME_OBJECTS = $(patsubst $(COMMON_DIR)/%.c,$(GAME_OBJ_DIR)/common_%.o,$(COMMON_SRC))
LAUNCHER_OBJECTS = $(patsubst $(LAUNCHER_SRC_DIR)/%.c,$(LAUNCHER_OBJ_DIR)/%.o,$(LAUNCHER_SOURCES))

# Game objects - flatten subdirectory structure into obj/ with prefixes
GAME_OBJECTS = \
    $(patsubst $(GAME_SRC_DIR)/%.c,$(GAME_OBJ_DIR)/%.o,$(wildcard $(GAME_SRC_DIR)/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/audio/%.c,$(GAME_OBJ_DIR)/audio_%.o,$(wildcard $(GAME_SRC_DIR)/audio/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/camera/%.c,$(GAME_OBJ_DIR)/camera_%.o,$(wildcard $(GAME_SRC_DIR)/camera/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/core/%.c,$(GAME_OBJ_DIR)/core_%.o,$(wildcard $(GAME_SRC_DIR)/core/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/combat/%.c,$(GAME_OBJ_DIR)/combat_%.o,$(wildcard $(GAME_SRC_DIR)/combat/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/game/%.c,$(GAME_OBJ_DIR)/game_%.o,$(wildcard $(GAME_SRC_DIR)/game/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/input/%.c,$(GAME_OBJ_DIR)/input_%.o,$(wildcard $(GAME_SRC_DIR)/input/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/network/%.c,$(GAME_OBJ_DIR)/network_%.o,$(wildcard $(GAME_SRC_DIR)/network/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/player/%.c,$(GAME_OBJ_DIR)/player_%.o,$(wildcard $(GAME_SRC_DIR)/player/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/render/%.c,$(GAME_OBJ_DIR)/render_%.o,$(wildcard $(GAME_SRC_DIR)/render/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/states/%.c,$(GAME_OBJ_DIR)/states_%.o,$(wildcard $(GAME_SRC_DIR)/states/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/texture/%.c,$(GAME_OBJ_DIR)/texture_%.o,$(wildcard $(GAME_SRC_DIR)/texture/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/utils/%.c,$(GAME_OBJ_DIR)/utils_%.o,$(wildcard $(GAME_SRC_DIR)/utils/*.c)) \
    $(patsubst $(GAME_SRC_DIR)/world/%.c,$(GAME_OBJ_DIR)/world_%.o,$(wildcard $(GAME_SRC_DIR)/world/*.c)) \
	$(patsubst $(GAME_SRC_DIR)/ui/%.c,$(GAME_OBJ_DIR)/ui_%.o,$(wildcard $(GAME_SRC_DIR)/ui/*.c))

# ============================================================================
# GAME INCLUDE FLAGS - All subdirectories
# ============================================================================

GAME_INCLUDES = \
    -I$(GAME_INCLUDE_DIR) \
    -I$(GAME_INCLUDE_DIR)/audio \
    -I$(GAME_INCLUDE_DIR)/camera \
    -I$(GAME_INCLUDE_DIR)/combat \
    -I$(GAME_INCLUDE_DIR)/core \
    -I$(GAME_INCLUDE_DIR)/game \
    -I$(GAME_INCLUDE_DIR)/GLFW \
    -I$(GAME_INCLUDE_DIR)/input \
    -I$(GAME_INCLUDE_DIR)/network \
    -I$(GAME_INCLUDE_DIR)/player \
    -I$(GAME_INCLUDE_DIR)/render \
    -I$(GAME_INCLUDE_DIR)/states \
    -I$(GAME_INCLUDE_DIR)/texture \
    -I$(GAME_INCLUDE_DIR)/utils \
    -I$(GAME_INCLUDE_DIR)/world \
	-I$(GAME_INCLUDE_DIR)/ui \
    -I$(COMMON_DIR)

# ============================================================================
# TARGETS
# ============================================================================

# Default target - build both
all: dirs launcher game

# Build only launcher
launcher: dirs $(LAUNCHER_TARGET)

# Build only game
game: dirs $(GAME_TARGET)

# Create directories
dirs:
	@if not exist "$(LAUNCHER_OBJ_DIR)" mkdir "$(LAUNCHER_OBJ_DIR)"
	@if not exist "$(LAUNCHER_BIN_DIR)" mkdir "$(LAUNCHER_BIN_DIR)"
	@if not exist "$(GAME_OBJ_DIR)" mkdir "$(GAME_OBJ_DIR)"
	@if not exist "$(GAME_BIN_DIR)" mkdir "$(GAME_BIN_DIR)"

# Link Launcher (include common objects)
$(LAUNCHER_TARGET): $(LAUNCHER_OBJECTS) $(COMMON_LAUNCHER_OBJECTS)
	$(CC) $^ -o $@ $(OPENSSL_LIB) $(LAUNCHER_LDFLAGS)
	@echo Launcher build complete: $(LAUNCHER_TARGET)

# Link Game (include common objects)
$(GAME_TARGET): $(GAME_OBJECTS) $(COMMON_GAME_OBJECTS)
	$(CC) $^ -o $@ $(GLFW_LIB) $(GAME_LDFLAGS)
	@echo Game build complete: $(GAME_TARGET)

# ============================================================================
# LAUNCHER COMPILATION RULES
# ============================================================================

# Compile Launcher objects
$(LAUNCHER_OBJ_DIR)/%.o: $(LAUNCHER_SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(OPENSSL_INCLUDE) -I$(LAUNCHER_INCLUDE_DIR) -I$(COMMON_DIR) -c $< -o $@

# Compile common objects for Launcher
$(LAUNCHER_OBJ_DIR)/common_%.o: $(COMMON_DIR)/%.c
	$(CC) $(CFLAGS) -I$(COMMON_DIR) -I$(LAUNCHER_INCLUDE_DIR) -c $< -o $@

# ============================================================================
# GAME COMPILATION RULES - Each subdirectory
# ============================================================================

# Root src files
$(GAME_OBJ_DIR)/%.o: $(GAME_SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Audio module
$(GAME_OBJ_DIR)/audio_%.o: $(GAME_SRC_DIR)/audio/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Camera module
$(GAME_OBJ_DIR)/camera_%.o: $(GAME_SRC_DIR)/camera/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Combat module
$(GAME_OBJ_DIR)/combat_%.o: $(GAME_SRC_DIR)/combat/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Core module
$(GAME_OBJ_DIR)/core_%.o: $(GAME_SRC_DIR)/core/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Game module
$(GAME_OBJ_DIR)/game_%.o: $(GAME_SRC_DIR)/game/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Input module
$(GAME_OBJ_DIR)/input_%.o: $(GAME_SRC_DIR)/input/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Network module
$(GAME_OBJ_DIR)/network_%.o: $(GAME_SRC_DIR)/network/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Player module
$(GAME_OBJ_DIR)/player_%.o: $(GAME_SRC_DIR)/player/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Render module
$(GAME_OBJ_DIR)/render_%.o: $(GAME_SRC_DIR)/render/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# States module
$(GAME_OBJ_DIR)/states_%.o: $(GAME_SRC_DIR)/states/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Texture module
$(GAME_OBJ_DIR)/texture_%.o: $(GAME_SRC_DIR)/texture/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Utils module
$(GAME_OBJ_DIR)/utils_%.o: $(GAME_SRC_DIR)/utils/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# World module
$(GAME_OBJ_DIR)/world_%.o: $(GAME_SRC_DIR)/world/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# UI module
$(GAME_OBJ_DIR)/ui_%.o: $(GAME_SRC_DIR)/ui/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# Compile common objects for Game
$(GAME_OBJ_DIR)/common_%.o: $(COMMON_DIR)/%.c
	$(CC) $(CFLAGS) $(GAME_INCLUDES) -c $< -o $@

# ============================================================================
# CLEAN TARGETS
# ============================================================================

# Clean everything
clean:
	@echo Cleaning project...
	-@if exist "Launcher\obj" del /Q "Launcher\obj\*.o"
	-@if exist "Launcher\bin" del /Q "Launcher\bin\Launcher.exe"
	-@if exist "Game\obj" del /Q "Game\obj\*.o"
	-@if exist "Game\bin" del /Q "Game\bin\Game.exe"
	@echo Clean complete

# Clean only launcher
clean-launcher:
	-@del /Q "Launcher\obj\*.o" 2>nul
	-@del /Q "Launcher\bin\Launcher.exe" 2>nul
	@echo Launcher clean complete

# Clean only game
clean-game:
	-@del /Q "Game\obj\*.o" 2>nul
	-@del /Q "Game\bin\Game.exe" 2>nul
	@echo Game clean complete

# ============================================================================
# RUN TARGETS
# ============================================================================

# Run launcher
run-launcher: launcher
	$(LAUNCHER_TARGET)

# Run game
run-game: game
	$(GAME_TARGET)

# Run launcher (which will launch the game)
run: all
	$(LAUNCHER_TARGET)

# ============================================================================
# DEBUG / INFO TARGETS
# ============================================================================

# List all game source files found
list-sources:
	@echo Game sources found:
	@echo ====================
	@for %%f in ($(GAME_SOURCES)) do @echo   %%f
	@echo.
	@echo Total: $(words $(GAME_SOURCES)) files

# List all game object files
list-objects:
	@echo Game objects:
	@echo =============
	@for %%f in ($(GAME_OBJECTS)) do @echo   %%f

# ---------------------------------------------------------------------------
# Tests — standalone, host-compiled, no GLFW/OpenGL required.
#
# Every test is one .c file in Game/tests linked against the generator sources.
# Later tasks extend WORLDGEN_SRC and TESTS by rewriting those two lines; the
# pattern rule below never changes. Both variables must be defined before the
# pattern rule, because make expands prerequisites as it reads the rule.
# ---------------------------------------------------------------------------
TEST_DIR = Game/tests
TEST_BIN = Game/tests/bin
TEST_CFLAGS = -Wall -Wextra -std=c11 -g -I$(GAME_INCLUDE_DIR) -I$(COMMON_DIR)

WORLDGEN_SRC = Game/data/src/worldgen_noise.c Game/data/src/worldgen_biome.c \
               Game/data/src/worldgen_city.c Game/data/src/worldgen_tile.c \
               Game/data/src/worldgen_write.c Game/data/src/worldgen_overview.c

TESTS = $(TEST_BIN)/tile_palette_test \
        $(TEST_BIN)/worldgen_biome_test \
        $(TEST_BIN)/worldgen_city_test \
        $(TEST_BIN)/worldgen_tile_test \
        $(TEST_BIN)/worldgen_format_test \
        $(TEST_BIN)/worldgen_overview_test

$(TEST_BIN):
	mkdir -p $(TEST_BIN)

$(TEST_BIN)/%: $(TEST_DIR)/%.c $(WORLDGEN_SRC) | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

test: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; done

# ---------------------------------------------------------------------------
# World generation
#
# Produces the world.dat consumed by BOTH the client and the world
# server. The two copies must stay byte-identical.
#
# This block must stay AFTER the WORLDGEN_SRC assignment above: make expands a
# rule's prerequisites as it reads the rule, so placing it earlier would build
# worldgen from world_generator.c alone and fail to link every worldgen_ symbol.
# ---------------------------------------------------------------------------
WORLDGEN_BIN     = Game/data/bin
SERVER_DATA_DIR ?= ../mmo_server/world_server/data

$(WORLDGEN_BIN):
	mkdir -p $(WORLDGEN_BIN)

$(WORLDGEN_BIN)/worldgen: Game/data/src/world_generator.c $(WORLDGEN_SRC) | $(WORLDGEN_BIN)
	$(CC) $(CFLAGS) -I$(GAME_INCLUDE_DIR) -I$(COMMON_DIR) -o $@ $^ -lm

world: $(WORLDGEN_BIN)/worldgen
	./$(WORLDGEN_BIN)/worldgen $(GAME_BIN_DIR)/world.dat
	@echo "Copying world.dat to the world server..."
	cp $(GAME_BIN_DIR)/world.dat $(SERVER_DATA_DIR)/world.dat
	@echo "world.dat installed to $(GAME_BIN_DIR) and $(SERVER_DATA_DIR)"

.PHONY: all launcher game dirs clean clean-launcher clean-game run run-launcher run-game list-sources list-objects test world
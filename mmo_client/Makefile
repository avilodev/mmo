# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2

COMMON_DIR = common

# ============================================================================
# PLATFORM
#
# The client's home is MSYS2/MinGW64 on Windows and the vendored /mingw64
# paths below are what it has always used. Those paths were hardcoded, which
# meant the tree could not be compiled anywhere else at all -- so nothing but a
# Windows developer's machine could tell whether an edit to a rendering or UI
# source file even built, and CI could only ever run the handful of tests that
# avoid GLFW.
#
# So the platform is detected, and every dependency is resolved through
# pkg-config where a platform has it. Overriding any of the *_CFLAGS or
# *_LIBS variables on the command line bypasses detection entirely:
#
#   make GLFW_CFLAGS=-I/opt/glfw/include GLFW_LIBS='-L/opt/glfw/lib -lglfw3'
# ============================================================================

# MSYS2/MinGW sets $(OS); a Linux or macOS shell does not.
ifeq ($(OS),Windows_NT)
  PLATFORM := windows
else
  PLATFORM := $(shell uname -s | tr '[:upper:]' '[:lower:]')
endif

PKG_CONFIG ?= pkg-config

ifeq ($(PLATFORM),windows)

  # OpenSSL — statically linked so the .exe runs on any Windows PC without
  # needing OpenSSL DLLs installed.
  # Requires: pacman -S mingw-w64-x86_64-openssl (MSYS2/MinGW64)
  # Verify static libs exist: ls /mingw64/lib/libssl.a /mingw64/lib/libcrypto.a
  #
  # Still tried through pkg-config first, so a non-default MSYS2 prefix works;
  # /mingw64 remains the fallback rather than the only answer.
  OPENSSL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null || echo -I/mingw64/include)
  OPENSSL_LIB    ?= $(shell $(PKG_CONFIG) --libs-only-L openssl 2>/dev/null || echo -L/mingw64/lib)
  # -Wl,-Bstatic  : link ssl+crypto from .a archives (baked into .exe)
  # -Wl,-Bdynamic : switch back to dynamic for everything after this
  # -lcrypt32     : Windows cert store API required by OpenSSL static build
  # Static OpenSSL block must come first; ws2_32/gdi32/crypt32 come after so
  # the linker can resolve OpenSSL's Winsock references when it processes them.
  OPENSSL_LDFLAGS ?= -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic

  LAUNCHER_LDFLAGS = $(OPENSSL_LDFLAGS) -lws2_32 -lgdi32 -lmsimg32 -lcrypt32

  # GLFW ships vendored in the tree for the Windows build.
  GLFW_CFLAGS ?= -I$(GAME_INCLUDE_DIR)
  GLFW_LIBS   ?= -L$(GAME_INCLUDE_DIR)/../lib -lglfw3
  # The Game links OpenSSL as well as the Launcher now: the realm hop is TLS,
  # so Game/src/network/net_tls.c is part of this binary. Same static block for
  # the same reason -- the .exe has to run on a machine with no OpenSSL DLLs --
  # and it comes before ws2_32/crypt32 so the linker can resolve OpenSSL's own
  # references to them.
  GAME_LDFLAGS = $(GLFW_LIBS) $(OPENSSL_LDFLAGS) -lopengl32 -lgdi32 -lws2_32 -lwinmm -lcrypt32 -lm

  EXE_SUFFIX = .exe

  # The Launcher is Win32: window.c, the UI files and the tray code call the
  # Windows API directly. It is not built anywhere else.
  BUILD_LAUNCHER = 1

  # The real Winsock headers are present here, so no shim and no override of
  # the standard the tree has always been built to.
  HOST_SHIM =
  HOST_STD  =

else

  # Linux, and anything else with pkg-config. The Game builds here; the
  # Launcher does not, and says so rather than failing in the linker.
  OPENSSL_CFLAGS  ?= $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
  OPENSSL_LIB     ?=
  OPENSSL_LDFLAGS ?= $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || echo -lssl -lcrypto)

  GLFW_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags glfw3 2>/dev/null)
  GLFW_LIBS   ?= $(shell $(PKG_CONFIG) --libs glfw3 2>/dev/null || echo -lglfw)
  GL_LIBS     ?= $(shell $(PKG_CONFIG) --libs gl 2>/dev/null || echo -lGL)

  LAUNCHER_LDFLAGS = $(OPENSSL_LDFLAGS)
  GAME_LDFLAGS = $(GLFW_LIBS) $(GL_LIBS) $(OPENSSL_LDFLAGS) -lm -lpthread

  EXE_SUFFIX =
  BUILD_LAUNCHER = 0

  # The Winsock shim, so the network sources compile here too.
  #
  # Game/include/network/network.h includes <winsock2.h> unconditionally, which
  # meant a third of the tree -- network.c and the four packet dispatchers, the
  # only client code that parses bytes someone else chose -- could not be
  # compiled outside Windows at all. `make strict` therefore stopped at the
  # first network source, and the CI step that claims to build the whole game
  # tree with warnings fatal never reached it.
  #
  # Game/tests/hostcompat supplies the handful of Winsock and Win32 names those
  # files mention, mapped onto POSIX sockets and a recursive pthread mutex. It
  # was written for the dispatcher tests; pointing the Linux build at it is
  # what makes "the whole tree, -Werror" true rather than aspirational.
  #
  # The shipped client is still the MSYS2 build, which sees the real Winsock
  # headers and never looks in here -- $(HOST_SHIM) is empty on Windows.
  HOST_SHIM = -I$(TEST_DIR)/hostcompat

  # gnu11 and _GNU_SOURCE: the shim maps CRITICAL_SECTION onto a recursive
  # pthread mutex (PTHREAD_MUTEX_RECURSIVE is a GNU extension), and the rolling
  # log uses clock_gettime and localtime_r, which strict ISO C does not declare.
  HOST_STD = -std=gnu11 -D_GNU_SOURCE

endif

# Compatibility alias: OPENSSL_INCLUDE was the old spelling.
OPENSSL_INCLUDE = $(OPENSSL_CFLAGS)
GLFW_INCLUDE    = $(GLFW_CFLAGS)
GLFW_LIB        =

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
LAUNCHER_TARGET = $(LAUNCHER_BIN_DIR)/Launcher$(EXE_SUFFIX)
GAME_TARGET = $(GAME_BIN_DIR)/Game$(EXE_SUFFIX)

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
    $(wildcard $(GAME_SRC_DIR)/model/*.c) \
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
    $(patsubst $(GAME_SRC_DIR)/model/%.c,$(GAME_OBJ_DIR)/model_%.o,$(wildcard $(GAME_SRC_DIR)/model/*.c)) \
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
    $(HOST_STD) \
    $(HOST_SHIM) \
    $(GLFW_CFLAGS) \
    $(OPENSSL_CFLAGS) \
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

# Default target. The Launcher is Win32-only, so elsewhere "everything" is the
# Game -- which is what makes an edit to a rendering or UI file checkable
# outside Windows.
ifeq ($(BUILD_LAUNCHER),1)
all: dirs launcher game
else
all: dirs game
	@echo "Note: the Launcher is Win32-only and was not built on $(PLATFORM)."
endif

# Build only launcher
ifeq ($(BUILD_LAUNCHER),1)
launcher: dirs $(LAUNCHER_TARGET)
else
launcher:
	@echo "The Launcher uses the Win32 API directly and only builds under MSYS2/MinGW64."
	@echo "Build it on Windows, or build just the game with 'make game'."
	@exit 1
endif

# Build only game
game: dirs $(GAME_TARGET)

# Create directories.
#
# mkdir -p, not the cmd.exe 'if not exist' form this used: that is a batch
# builtin, so it worked under make.bat and failed under every other shell --
# including MSYS2's own. -p is silent when the directory already exists.
dirs:
	@mkdir -p $(LAUNCHER_OBJ_DIR) $(LAUNCHER_BIN_DIR) $(GAME_OBJ_DIR) $(GAME_BIN_DIR)

# Link Launcher (include common objects)
#
# The pin file is installed beside the .exe as part of the link step, not as a
# separate thing to remember. tls_client_init() refuses to build a TLS context
# without it, so a launcher shipped without its pins does not connect at all --
# which is the intended failure, but only useful if the ordinary build produces
# a working one.
$(LAUNCHER_TARGET): $(LAUNCHER_OBJECTS) $(COMMON_LAUNCHER_OBJECTS)
	$(CC) $^ -o $@ $(OPENSSL_LIB) $(LAUNCHER_LDFLAGS)
	@mkdir -p $(LAUNCHER_BIN_DIR)/certs
	@cp Launcher/certs/login_pins.txt $(LAUNCHER_BIN_DIR)/certs/login_pins.txt
	@echo "Installed login pins -> $(LAUNCHER_BIN_DIR)/certs/login_pins.txt"
	@echo Launcher build complete: $(LAUNCHER_TARGET)

# Link Game (include common objects)
$(GAME_TARGET): $(GAME_OBJECTS) $(COMMON_GAME_OBJECTS)
	$(CC) $^ -o $@ $(GAME_LDFLAGS)
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

# Model module (glTF loading, posing and skinning; no GL)
$(GAME_OBJ_DIR)/model_%.o: $(GAME_SRC_DIR)/model/%.c
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
# rm -rf, not the cmd.exe 'if exist ... del /Q' form this used: those are batch
# builtins, so clean only worked when make was driven from make.bat and failed
# under every actual shell, MSYS2's included.
#
# Game/bin/world.dat is deliberately not removed. It is around a gigabyte and
# takes minutes to regenerate, and nobody who types 'make clean' means that.
clean:
	@echo Cleaning project...
	@rm -rf $(LAUNCHER_OBJ_DIR) $(GAME_OBJ_DIR)
	@rm -f  $(LAUNCHER_TARGET) $(GAME_TARGET)
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

WORLDGEN_SRC = Game/tools/worldgen/worldgen_noise.c Game/tools/worldgen/worldgen_biome.c \
               Game/tools/worldgen/worldgen_city.c Game/tools/worldgen/worldgen_courtyard.c \
               Game/tools/worldgen/worldgen_city_collision.c \
               Game/tools/worldgen/worldgen_city_spans.c \
               Game/tools/worldgen/worldgen_tile.c \
               Game/tools/worldgen/worldgen_write.c Game/tools/worldgen/worldgen_overview.c

TESTS = $(TEST_BIN)/tile_palette_test \
        $(TEST_BIN)/worldgen_biome_test \
        $(TEST_BIN)/worldgen_city_test \
        $(TEST_BIN)/worldgen_tile_test \
        $(TEST_BIN)/worldgen_format_test \
        $(TEST_BIN)/worldgen_overview_test \
        $(TEST_BIN)/worldgen_courtyard_test \
        $(TEST_BIN)/worldgen_city_collision_test \
        $(TEST_BIN)/worldgen_city_spans_test \
        $(TEST_BIN)/cert_pin_test \
        $(TEST_BIN)/cert_pin_digest_test \
        $(TEST_BIN)/net_dispatch_test \
        $(TEST_BIN)/net_connect_test \
        $(TEST_BIN)/race_registry_test \
        $(TEST_BIN)/name_cache_test \
        $(TEST_BIN)/net_ping_test \
        $(TEST_BIN)/net_ping_watch_test \
        $(TEST_BIN)/client_log_test \
        $(TEST_BIN)/player_reconcile_test \
        $(TEST_BIN)/camera_math_test \
        $(TEST_BIN)/city_heights_test \
        $(TEST_BIN)/item_db_test \
        $(TEST_BIN)/world_seats_test \
        $(TEST_BIN)/emote_test \
        $(TEST_BIN)/emote_hub_test \
        $(TEST_BIN)/char_select_nav_test \
        $(TEST_BIN)/key_icon_file_test \
        $(TEST_BIN)/settings_fields_test \
        $(TEST_BIN)/letterbox_test \
        $(TEST_BIN)/load_queue_test \
        $(TEST_BIN)/join_data_test \
        $(TEST_BIN)/world_clock_test \
        $(TEST_BIN)/weather_state_test \
        $(TEST_BIN)/weather_now_test \
        $(TEST_BIN)/cover_field_test \
        $(TEST_BIN)/weather_cells_test \
        $(TEST_BIN)/wind_gust_test \
        $(TEST_BIN)/weather_synth_test \
        $(TEST_BIN)/mixer_test \
        $(TEST_BIN)/weather_sound_test \
        $(TEST_BIN)/weather_field_test \
        $(TEST_BIN)/window_pos_test \
        $(TEST_BIN)/font_levels_test \
        $(TEST_BIN)/weather_layout_test \
        $(TEST_BIN)/weather_drawer_view_test \
        $(TEST_BIN)/sky_test \
        $(TEST_BIN)/world_lighting_test \
        $(TEST_BIN)/shadow_math_test \
        $(TEST_BIN)/city_lamps_test \
        $(TEST_BIN)/lamp_shadow_math_test \
        $(TEST_BIN)/chat_commands_test \
        $(TEST_BIN)/status_client_test \
        $(TEST_BIN)/inspect_client_test \
        $(TEST_BIN)/chat_ui_test \
        $(TEST_BIN)/world_spans_test \
        $(TEST_BIN)/world_motion_test \
        $(TEST_BIN)/player_vertical_test \
        $(TEST_BIN)/height_smooth_test \
        $(TEST_BIN)/world_pick_test \
        $(TEST_BIN)/texture_atlas_test \
        $(TEST_BIN)/chunk_mesh_test \
        $(TEST_BIN)/model_test \
        $(TEST_BIN)/obj_mesh_test \
        $(TEST_BIN)/character_motion_test \
        $(TEST_BIN)/character_pose_test \
        $(TEST_BIN)/footing_test \
        $(TEST_BIN)/character_foot_ik_test \
        $(TEST_BIN)/character_legs_test \
        $(TEST_BIN)/character_feet_test \
        $(TEST_BIN)/structure_mesh_test \
        $(TEST_BIN)/locomotion_test \
        $(TEST_BIN)/appearance_test \
        $(TEST_BIN)/character_look_test \
        $(TEST_BIN)/character_retarget_test \
        $(TEST_BIN)/character_spring_test \
        $(TEST_BIN)/character_spring_collide_test \
        $(TEST_BIN)/quest_available_test \
        $(TEST_BIN)/quest_finished_test \
        $(TEST_BIN)/quest_steps_test \
        $(TEST_BIN)/quest_distance_test

# ---------------------------------------------------------------------------
# Host builds of the network sources
#
# The four packet dispatchers are the only client code that parses bytes chosen
# by someone else, and they had no test — they were reachable only through a
# Winsock build that runs on one operating system. Game/tests/hostcompat
# supplies the handful of Win32 and Winsock names they mention so they compile
# on the machine running CI. Nothing in the shipped client uses it.
#
# GLFW_INCLUDE_NONE keeps glfw3.h from pulling in <GL/gl.h>, which the
# dispatchers do not need and a headless build does not have.
# ---------------------------------------------------------------------------
# race_registry.c is here rather than stubbed: net_session.c stores the race
# list into it, and a stub would test that the call compiles rather than that
# the list survives the packet.
NET_SRC = Game/src/states/join_data.c Game/src/network/net_session.c Game/src/network/net_world.c Game/src/network/net_dispatch.c \
          Game/src/network/net_combat.c Game/src/network/net_inventory.c \
          Game/src/network/net_friends.c Game/src/ui/friends_panel.c \
          Game/src/core/race_registry.c Game/src/network/name_cache.c \
          Game/src/player/emote.c Game/src/player/emote_clips.c $(COMMON_DIR)/world_seats.c \
          Game/src/states/chat_log.c Game/src/core/client_log.c \
          Game/src/network/net_ping.c Game/src/network/net_ping_watch.c

# gnu11 and _GNU_SOURCE: the shim maps CRITICAL_SECTION onto a recursive
# pthread mutex, and PTHREAD_MUTEX_RECURSIVE is a GNU extension.
NET_TEST_CFLAGS = -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
                  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES)

$(TEST_BIN):
	mkdir -p $(TEST_BIN)

# The pin parsing is deliberately free of OpenSSL and Windows, so it builds and
# runs on the host like any other test. It lives in common/ because the launcher
# and the game both pin a server now. Listed before the pattern rule below,
# which would otherwise link these against the world generator.
$(TEST_BIN)/cert_pin_digest_test: $(TEST_DIR)/cert_pin_digest_test.c $(COMMON_DIR)/cert_pin.c $(COMMON_DIR)/cert_spki.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) $(OPENSSL_CFLAGS) -o $@ $^ $(OPENSSL_LDFLAGS) -lm

$(TEST_BIN)/cert_pin_test: $(TEST_DIR)/cert_pin_test.c $(COMMON_DIR)/cert_pin.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=c11 -g -I$(COMMON_DIR) -o $@ $^

# Listed before the pattern rule below, which would otherwise link the packet
# dispatchers against the world generator.
# The rolling log file. No GLFW, no sockets; it writes to /tmp and cleans up.
$(TEST_BIN)/client_log_test: $(TEST_DIR)/client_log_test.c Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -I$(GAME_INCLUDE_DIR) -I$(COMMON_DIR) -o $@ $^ -pthread

# The name cache. Uses the hostcompat shim for g_net and the Winsock names,
# like the dispatcher tests; it never opens a socket.
$(TEST_BIN)/name_cache_test: $(TEST_DIR)/name_cache_test.c Game/src/network/name_cache.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ -lm -pthread

# The ping arithmetic: the window, the half trip and the packet. No sockets.
$(TEST_BIN)/net_ping_test: $(TEST_DIR)/net_ping_test.c Game/src/network/net_ping.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ -lm

# The ping watch against a real socket pair, with the frame reading late.
$(TEST_BIN)/net_ping_watch_test: $(TEST_DIR)/net_ping_watch_test.c Game/src/network/net_ping_watch.c \
                                 Game/src/network/net_ping.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ -lm -pthread

# The list of quests the server says may be taken: plain state, no GL.
$(TEST_BIN)/quest_available_test: $(TEST_DIR)/quest_available_test.c Game/src/ui/quest_available.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ -lm

# The quests the server says were finished: plain state, no GL.
$(TEST_BIN)/quest_finished_test: $(TEST_DIR)/quest_finished_test.c Game/src/ui/quest_finished.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ -lm

# The step a quest is on, and how it is worded: plain state, no GL.
$(TEST_BIN)/quest_steps_test: $(TEST_DIR)/quest_steps_test.c Game/src/ui/quest_steps.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ -lm

# The client-side race registry: no GLFW, no sockets, just the list the server
# sent and what the client derives from it.
$(TEST_BIN)/race_registry_test: $(TEST_DIR)/race_registry_test.c Game/src/core/race_registry.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=c11 -g -I$(GAME_INCLUDE_DIR) -I$(GAME_INCLUDE_DIR)/core -I$(COMMON_DIR) -o $@ $^ -lm

# Client-side reconciliation: which position a correction lands on, and that
# one the client would have agreed with lands on the one it is already at.
# player.c mixes the movement model with the code that draws it, so rendering
# is stubbed the way the dispatcher tests stub Winsock. GLFW_INCLUDE_NONE keeps
# glfw3.h (reached through game_types.h) from pulling in <GL/gl.h>, which a
# headless build does not have and this does not need.
$(TEST_BIN)/player_reconcile_test: $(TEST_DIR)/player_reconcile_test.c \
                                   Game/src/player/player.c \
                                   Game/src/player/move_refusal.c \
                                   Game/src/player/locomotion.c \
                                   Game/src/player/player_vertical.c \
                                   $(COMMON_DIR)/world_motion.c \
                                   $(COMMON_DIR)/world_spans.c \
                                   Game/src/camera/camera_math.c \
                                   Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm -pthread

# The character model: loads the shipped .glb, checks every clip the renderer
# plays is present, and skins a frame. Pure arithmetic, no GL; stb_image's
# implementation comes from the test itself, not texture.c.
$(TEST_BIN)/model_test: $(TEST_DIR)/model_test.c Game/src/model/json.c Game/src/model/glb.c \
                        Game/src/model/skinned_model_glb.c Game/src/model/skinned_pose.c \
                        Game/src/render/character_look.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The OBJ loader behind the city scene, on a small scene the test writes itself
# (the real export is untracked), including its binary cache.
$(TEST_BIN)/obj_mesh_test: $(TEST_DIR)/obj_mesh_test.c Game/src/model/obj_mesh.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The local player's GTA-style locomotion: walk/run, acceleration, turning.
$(TEST_BIN)/locomotion_test: $(TEST_DIR)/locomotion_test.c Game/src/player/locomotion.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The appearance word's layout and validation, from the shared protocol.h.
$(TEST_BIN)/appearance_test: $(TEST_DIR)/appearance_test.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Which meshes of the character kit a body wears, on a model that is only names.
$(TEST_BIN)/character_look_test: $(TEST_DIR)/character_look_test.c Game/src/render/character_look.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The kit's clips played on the players' models: matched by bone name, with the twist bones.
$(TEST_BIN)/character_retarget_test: $(TEST_DIR)/character_retarget_test.c Game/src/model/json.c Game/src/model/glb.c \
                                     Game/src/model/skinned_model_glb.c Game/src/model/skinned_pose.c \
                                     Game/src/render/character_pose.c Game/src/render/character_retarget.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The players' hair swinging with the body, on the shipped models.
$(TEST_BIN)/character_spring_test: $(TEST_DIR)/character_spring_test.c Game/src/model/json.c Game/src/model/glb.c \
                                   Game/src/model/skinned_model_glb.c Game/src/model/skinned_pose.c \
                                   Game/src/render/character_pose.c Game/src/render/character_spring.c \
                                   Game/src/render/character_spring_collide.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The hair and tail kept out of the body by colliders fitted to the shipped models.
$(TEST_BIN)/character_spring_collide_test: $(TEST_DIR)/character_spring_collide_test.c Game/src/model/json.c \
                                           Game/src/model/glb.c Game/src/model/skinned_model_glb.c \
                                           Game/src/model/skinned_pose.c Game/src/render/character_pose.c \
                                           Game/src/render/character_retarget.c Game/src/render/character_spring.c \
                                           Game/src/render/character_spring_collide.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# How a body picks, times and blends its clip from its drawn movement.
$(TEST_BIN)/character_motion_test: $(TEST_DIR)/character_motion_test.c Game/src/render/character_motion.c \
                                   Game/src/render/character_procedural.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The procedural offsets applied to the shipped rig, and masked blending.
$(TEST_BIN)/character_pose_test: $(TEST_DIR)/character_pose_test.c Game/src/model/json.c Game/src/model/glb.c \
                                 Game/src/model/skinned_model_glb.c Game/src/model/skinned_pose.c \
                                 Game/src/render/character_pose.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The two-bone leg solver and pelvis rule for stairs and slopes.
$(TEST_BIN)/character_foot_ik_test: $(TEST_DIR)/character_foot_ik_test.c \
                                    Game/src/render/character_foot_ik.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The legs of the shipped rig: found, read off a pose, bent to a solution.
$(TEST_BIN)/character_legs_test: $(TEST_DIR)/character_legs_test.c Game/src/model/json.c Game/src/model/glb.c \
                                 Game/src/model/skinned_model_glb.c Game/src/model/skinned_pose.c \
                                 Game/src/render/character_pose.c Game/src/render/character_legs.c \
                                 Game/src/render/character_foot_ik.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Where the feet and the body go on stairs, ramps and flat ground.
$(TEST_BIN)/character_feet_test: $(TEST_DIR)/character_feet_test.c Game/src/render/character_feet.c \
                                 Game/src/render/character_foot_ik.c Game/src/world/footing.c \
                                 $(COMMON_DIR)/world_spans.c Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -D_GNU_SOURCE -o $@ $^ -lm -pthread

# The 3D camera's projection, picking, culling bounds and movement rotation.
# Pure math over cglm, so it runs headless like the generator tests.
$(TEST_BIN)/camera_math_test: $(TEST_DIR)/camera_math_test.c Game/src/camera/camera_math.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -I$(GAME_INCLUDE_DIR)/camera -o $@ $^ -lm

# The slash-command table, with the network calls stubbed by the test.
$(TEST_BIN)/chat_commands_test: $(TEST_DIR)/chat_commands_test.c Game/src/states/chat_commands.c \
                                Game/src/states/chat_log.c Game/src/player/emote.c Game/src/player/emote_clips.c $(COMMON_DIR)/world_seats.c \
                                Game/src/core/client_log.c Game/src/core/session_status.c \
                                Game/src/world/world_clock.c Game/src/world/weather_now.c Game/src/world/weather_cells.c \
                                Game/src/world/weather_state.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm -pthread

# The player's status: its names, and what chat says when the server changes it.
$(TEST_BIN)/status_client_test: $(TEST_DIR)/status_client_test.c Game/src/states/status_client.c \
                                Game/src/core/session_status.c Game/src/states/chat_log.c \
                                Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm -pthread

# Inspecting another player: the reply decoded, and the window or chat line it earns.
$(TEST_BIN)/inspect_client_test: $(TEST_DIR)/inspect_client_test.c Game/src/states/inspect_client.c \
                                 Game/src/ui/character_view.c Game/src/states/chat_log.c \
                                 Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm -pthread

# The chat box: channels, wrapping, scrolling, the input line and the log.
$(TEST_BIN)/chat_ui_test: $(TEST_DIR)/chat_ui_test.c Game/src/states/chat_channels.c \
                          Game/src/states/chat_layout.c Game/src/states/chat_input.c \
                          Game/src/states/chat_log.c Game/src/ui/window_pos.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The emote state machine and registry behind /sit and /dance. Writes a seats
# file to /tmp.
$(TEST_BIN)/emote_test: $(TEST_DIR)/emote_test.c Game/src/player/emote.c Game/src/player/emote_clips.c \
                        $(COMMON_DIR)/world_seats.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The emote hub: the clip table, the six slots and their file, and where the boxes sit.
$(TEST_BIN)/emote_hub_test: $(TEST_DIR)/emote_hub_test.c Game/src/player/emote_clips.c \
                            Game/src/player/emote_slots.c Game/src/ui/emote_hub_layout.c \
                            Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm -pthread

# The character select cycle: characters, then the new character entry; no GL.
$(TEST_BIN)/char_select_nav_test: $(TEST_DIR)/char_select_nav_test.c Game/src/ui/char_select_nav.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^

# The sprite each bindable key gets on the key bindings page; no GL.
$(TEST_BIN)/key_icon_file_test: $(TEST_DIR)/key_icon_file_test.c Game/src/ui/key_icon_file.c \
                                Game/src/core/keybinds.c Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm -pthread

# The settings table the settings menu is built from; no GL.
$(TEST_BIN)/settings_fields_test: $(TEST_DIR)/settings_fields_test.c Game/src/ui/settings_fields.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The 16:9 box the game draws into and the cursor mapping through it; no GL.
$(TEST_BIN)/letterbox_test: $(TEST_DIR)/letterbox_test.c Game/src/render/letterbox.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The step queue the loading screen runs on; no GL.
$(TEST_BIN)/load_queue_test: $(TEST_DIR)/load_queue_test.c Game/src/states/load_queue.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^

# Which join packets have arrived; no GL.
$(TEST_BIN)/join_data_test: $(TEST_DIR)/join_data_test.c Game/src/states/join_data.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^

# The world clock between server syncs; no GL.
$(TEST_BIN)/world_clock_test: $(TEST_DIR)/world_clock_test.c Game/src/world/world_clock.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# Where the weather window puts things; no GL.
$(TEST_BIN)/weather_layout_test: $(TEST_DIR)/weather_layout_test.c Game/src/ui/weather_layout.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# What the weather drawer's map shows; no GL.
$(TEST_BIN)/weather_drawer_view_test: $(TEST_DIR)/weather_drawer_view_test.c Game/src/ui/weather_drawer_view.c Game/src/ui/weather_layout.c Game/src/ui/map_layout.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# How far a quest is from the player; no GL.
$(TEST_BIN)/quest_distance_test: $(TEST_DIR)/quest_distance_test.c Game/src/ui/quest_distance.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The weather blended over the map; no GL.
$(TEST_BIN)/weather_field_test: $(TEST_DIR)/weather_field_test.c Game/src/world/weather_field.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# Where a window sits and how it is dragged; no GL.
$(TEST_BIN)/window_pos_test: $(TEST_DIR)/window_pos_test.c Game/src/ui/window_pos.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# Which font bake suits a size on screen; no GL.
$(TEST_BIN)/font_levels_test: $(TEST_DIR)/font_levels_test.c Game/src/render/font_levels.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The forecast the client keeps between server packets; no GL.
$(TEST_BIN)/weather_state_test: $(TEST_DIR)/weather_state_test.c Game/src/world/weather_state.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The weather this moment, eased from the forecast; no GL.
$(TEST_BIN)/weather_now_test: $(TEST_DIR)/weather_now_test.c Game/src/world/weather_now.c Game/src/world/weather_cells.c \
                              Game/src/world/weather_state.c Game/src/world/sky.c \
                              Game/src/world/world_lighting.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The patches of weather over the ground, and the fronts that bring a new day's; no GL.
$(TEST_BIN)/weather_cells_test: $(TEST_DIR)/weather_cells_test.c Game/src/world/weather_cells.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# Gusts of wind sweeping over the city and leaning hair; no GL.
$(TEST_BIN)/wind_gust_test: $(TEST_DIR)/wind_gust_test.c Game/src/world/wind_gust.c Game/src/world/weather_cells.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The heights of the roofs over the city; no GL.
$(TEST_BIN)/cover_field_test: $(TEST_DIR)/cover_field_test.c Game/src/world/cover_field.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The weather sounds made from noise, the mixer that plays them, and the code that
# follows the sky with them; no sound card needed.
$(TEST_BIN)/weather_synth_test: $(TEST_DIR)/weather_synth_test.c Game/src/audio/weather_synth.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

$(TEST_BIN)/mixer_test: $(TEST_DIR)/mixer_test.c Game/src/audio/mixer.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

$(TEST_BIN)/weather_sound_test: $(TEST_DIR)/weather_sound_test.c Game/src/audio/weather_sound.c \
                                Game/src/audio/mixer.c Game/src/audio/weather_synth.c \
                                Game/src/world/weather_now.c Game/src/world/weather_cells.c Game/src/world/weather_state.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The sky's colours and lights by hour of day; no GL.
$(TEST_BIN)/sky_test: $(TEST_DIR)/sky_test.c Game/src/world/sky.c Game/src/world/world_clock.c \
                       Game/src/world/weather_now.c Game/src/world/weather_cells.c Game/src/world/weather_state.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The world's lighting by hour of day; no GL.
$(TEST_BIN)/world_lighting_test: $(TEST_DIR)/world_lighting_test.c Game/src/world/world_lighting.c Game/src/world/sky.c \
                                 Game/src/world/weather_now.c Game/src/world/weather_cells.c Game/src/world/weather_state.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The shadow map's matrices; no GL.
$(TEST_BIN)/shadow_math_test: $(TEST_DIR)/shadow_math_test.c Game/src/world/shadow_math.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The city's lamp list; no GL.
$(TEST_BIN)/city_lamps_test: $(TEST_DIR)/city_lamps_test.c Game/src/world/city_lamps.c Game/src/model/json.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The lamp shadow views; no GL.
$(TEST_BIN)/lamp_shadow_math_test: $(TEST_DIR)/lamp_shadow_math_test.c Game/src/world/lamp_shadow_math.c | $(TEST_BIN)
	$(CC) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -g -DGLFW_INCLUDE_NONE \
	  -I$(TEST_DIR) -I$(TEST_DIR)/hostcompat $(GAME_INCLUDES) -o $@ $^ -lm

# The seats module shared with the server, and the baked file the client reads.
$(TEST_BIN)/world_seats_test: $(TEST_DIR)/world_seats_test.c $(COMMON_DIR)/world_seats.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -D_GNU_SOURCE -DWORLD_SEATS_BAKED_PATH='"Game/assets/baked/test_town_seats.bin"' \
	  -o $@ $^ -lm

# The baked wall heights the camera stays out of. Reads Game/assets/baked/test_town_heights.bin.
$(TEST_BIN)/city_heights_test: $(TEST_DIR)/city_heights_test.c Game/src/world/city_heights.c \
                               Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -D_GNU_SOURCE -o $@ $^ -lm -pthread

# The client item database: the generated items.json, its parser and the tooltip text.
$(TEST_BIN)/item_db_test: $(TEST_DIR)/item_db_test.c Game/src/player/item_db.c \
                          Game/src/core/race_registry.c Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ -lm -pthread

# The fine floor heights under the feet: tiles, edges, ramps, levels, bad files.
$(TEST_BIN)/footing_test: $(TEST_DIR)/footing_test.c Game/src/world/footing.c \
                          $(COMMON_DIR)/world_spans.c Game/src/core/client_log.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -D_GNU_SOURCE -o $@ $^ -lm -pthread

# The span-column queries shared with the server (copy of its common/ files).
$(TEST_BIN)/world_spans_test: $(TEST_DIR)/world_spans_test.c $(COMMON_DIR)/world_spans.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The baked spans become the sidecar the span queries read back.
$(TEST_BIN)/worldgen_city_spans_test: $(TEST_DIR)/worldgen_city_spans_test.c $(WORLDGEN_SRC) \
    $(COMMON_DIR)/world_spans.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Picking a point on the surface under a ray, against the spans.
$(TEST_BIN)/world_pick_test: $(TEST_DIR)/world_pick_test.c Game/src/world/world_pick.c \
    $(COMMON_DIR)/world_spans.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The eased height bodies and the camera are drawn at.
$(TEST_BIN)/height_smooth_test: $(TEST_DIR)/height_smooth_test.c Game/src/render/height_smooth.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The client's fixed-step use of the shared vertical motion.
$(TEST_BIN)/player_vertical_test: $(TEST_DIR)/player_vertical_test.c \
    Game/src/player/player_vertical.c $(COMMON_DIR)/world_motion.c \
    $(COMMON_DIR)/world_spans.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Shared vertical motion (copy of the server's common/ files).
$(TEST_BIN)/world_motion_test: $(TEST_DIR)/world_motion_test.c $(COMMON_DIR)/world_motion.c \
    $(COMMON_DIR)/world_spans.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# Tileset gutter padding, so mipmapped ground tiles do not bleed into each other.
$(TEST_BIN)/texture_atlas_test: $(TEST_DIR)/texture_atlas_test.c Game/src/texture/texture_atlas.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

# The vertex data a chunk layer becomes for the ground shader. world.h pulls in
# camera.h and, through it, glfw3.h; GLFW_INCLUDE_NONE keeps that from needing
# <GL/gl.h>, which a headless build does not have.
$(TEST_BIN)/chunk_mesh_test: $(TEST_DIR)/chunk_mesh_test.c Game/src/world/chunk_mesh.c \
                             Game/src/texture/texture_atlas.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -DGLFW_INCLUDE_NONE $(GLFW_CFLAGS) -I$(GAME_INCLUDE_DIR)/camera -o $@ $^ -lm

# The 3D structures raised from the collision layer: that they stand exactly on
# the solid tiles, and have the shape they should. Same headless setup as the
# chunk mesh test.
$(TEST_BIN)/structure_mesh_test: $(TEST_DIR)/structure_mesh_test.c Game/src/world/structure_mesh.c | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -DGLFW_INCLUDE_NONE $(GLFW_CFLAGS) -I$(GAME_INCLUDE_DIR)/camera -o $@ $^ -lm

$(TEST_BIN)/net_dispatch_test: $(TEST_DIR)/net_dispatch_test.c $(NET_SRC) Game/src/player/inventory.c \
                               Game/src/player/item_db.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ -lm -pthread

# The connection state machine, including network.c itself: what this measures
# is wall-clock, so the real socket calls have to be the ones under test.
$(TEST_BIN)/net_connect_test: $(TEST_DIR)/net_connect_test.c \
                              Game/src/network/network.c \
                              Game/src/network/net_connect.c \
                              Game/src/network/net_reconnect.c \
                              Game/src/network/net_tls.c \
                              $(COMMON_DIR)/cert_pin.c \
                              $(COMMON_DIR)/cert_spki.c \
                              $(NET_SRC) | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -o $@ $^ $(OPENSSL_LDFLAGS) -lm -pthread

$(TEST_BIN)/%: $(TEST_DIR)/%.c $(WORLDGEN_SRC) | $(TEST_BIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ -lm

test: $(TESTS)
	@echo "=== Checking the wire format against PROTOCOL_VERSION ==="
	@./Game/tests/check_protocol_version.sh
	@echo "=== Checking client diagnostics go to the log, not a console ==="
	@./Game/tests/check_client_logging.sh
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; done

# ---------------------------------------------------------------------------
# Rebuild everything from scratch and reject every compiler warning.
#
# The server has had this since its own audit; the client had no equivalent, so
# a warning introduced here was only ever seen by whoever happened to scroll
# past it. Pairs with the pkg-config platform detection at the top: together
# they are what lets CI compile the whole game tree and fail on a warning,
# rather than only the handful of tests that avoid GLFW.
# ---------------------------------------------------------------------------
.PHONY: strict
strict:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -Werror" all

# The packet dispatchers under AddressSanitizer and UBSan.
#
# The plain build above proves the fuzz corpus runs. This is the build that
# proves it means something: -fsanitize=bounds-strict catches an index past a
# fixed array even when the array is a field inside a larger object, which is
# exactly the shape every one of these handlers has — a declared count from the
# wire indexing a fixed-size table. Plain ASan does not see that overflow,
# because it never leaves the enclosing struct.
#
# -fno-sanitize-recover=all makes the first violation the exit status rather
# than a line in the output nobody reads.
.PHONY: net-sanitize
net-sanitize: $(TEST_DIR)/net_dispatch_test.c $(NET_SRC) Game/src/player/inventory.c \
              Game/src/player/item_db.c | $(TEST_BIN)
	$(CC) $(NET_TEST_CFLAGS) -O0 \
	  -fsanitize=address,undefined -fsanitize=bounds-strict \
	  -fno-sanitize-recover=all -fno-omit-frame-pointer \
	  -o $(TEST_BIN)/net_dispatch_sanitize $^ -lm -pthread
	./$(TEST_BIN)/net_dispatch_sanitize

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
WORLDGEN_BIN     = Game/bin
SERVER_DATA_DIR ?= ../mmo_server/world_server/data

$(WORLDGEN_BIN):
	mkdir -p $(WORLDGEN_BIN)

$(WORLDGEN_BIN)/worldgen: Game/tools/worldgen/world_generator.c $(WORLDGEN_SRC) | $(WORLDGEN_BIN)
	$(CC) $(CFLAGS) -I$(GAME_INCLUDE_DIR) -I$(COMMON_DIR) -o $@ $^ -lm

world: $(WORLDGEN_BIN)/worldgen
	./$(WORLDGEN_BIN)/worldgen $(GAME_BIN_DIR)/world.dat
	@echo "Copying world.dat to the world server..."
	cp $(GAME_BIN_DIR)/world.dat $(SERVER_DATA_DIR)/world.dat
	cp $(GAME_BIN_DIR)/world_spans.bin $(SERVER_DATA_DIR)/world_spans.bin
	@# The fine floor mask sits beside the spans in both places; baked by export_city_spans.py.
	@if [ -f Game/assets/baked/test_town_footing.bin ]; then \
	  cp Game/assets/baked/test_town_footing.bin $(GAME_BIN_DIR)/world_footing.bin; \
	  cp Game/assets/baked/test_town_footing.bin $(SERVER_DATA_DIR)/world_footing.bin; \
	else \
	  echo "NOTE: no baked floor mask; removing any old one so it cannot outlive its spans"; \
	  rm -f $(GAME_BIN_DIR)/world_footing.bin $(SERVER_DATA_DIR)/world_footing.bin; \
	fi
	@# The baked seats (SIT_ markers) go to the server so it can check a sit.
	@[ -f Game/assets/baked/test_town_seats.bin ] && cp Game/assets/baked/test_town_seats.bin $(SERVER_DATA_DIR)/world_seats.bin || true
	@echo "world.dat and world_spans.bin installed to $(GAME_BIN_DIR) and $(SERVER_DATA_DIR)"

.PHONY: all launcher game dirs clean clean-launcher clean-game run run-launcher run-game list-sources list-objects test net-sanitize world items

# ---------------------------------------------------------------------------
# Client item data: the server's items.json cut down to the fields a player sees.
# Run after the server file changes; the output is Game/data/items.json.
# ---------------------------------------------------------------------------
PYTHON ?= python3

items:
	$(PYTHON) Game/tools/sync_items.py

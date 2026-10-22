// Projectiles in 3D: pitch toward the aim height, a height test on every hit,
// and a stop at solid ground and walls. Links projectile.c against stubs for
// the NPC pool, the player registry, interest and the socket layer.

#define _POSIX_C_SOURCE 200809L
#include "projectile.h"
#include "interest.h"
#include "loot.h"
#include "npc_mitigation.h"
#include "npc_triggers.h"
#include "party.h"
#include "player_data.h"
#include "player_effects.h"
#include "player_level.h"
#include "quest_system.h"
#include "utils.h"
#include "world_spans_fixture.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VIEWER_FD 7
#define DT 0.05
#define MAX_DESTROYS 16
#define TEST_NPCS 2

ActivePlayer active_players[MAX_PLAYERS];

// What the stubbed socket layer saw.
static int g_destroy_count;
static uint8_t g_destroy_reason[MAX_DESTROYS];
static int g_update_seen;
static ProjectilePositionData g_update;
static ProjectileSpawnPacket g_spawn;

// The NPCs the stubbed pool hands out, and the snapshot that lists them.
static NPCEntity g_npc[TEST_NPCS];
static int g_snap_slot[TEST_NPCS];
static uint32_t g_snap_id[TEST_NPCS];
static float g_snap_x[TEST_NPCS];
static float g_snap_y[TEST_NPCS];
static float g_snap_z[TEST_NPCS];
static float g_snap_r[TEST_NPCS];
static NPCTickSnapshot g_npcs;

// Players the stubbed tick grid hands out.
static TickSnapshot g_players;

static float g_shot_speed = 100.0f;
static char g_fixture_path[64];

ssize_t outbox_send(int fd, void* buf, size_t len) {
	const PacketHeader* h = buf;

	(void)fd;
	if(h->type == PACKET_PROJECTILE_DESTROY && g_destroy_count < MAX_DESTROYS) {
		const ProjectileDestroyPacket* p = buf;

		g_destroy_reason[g_destroy_count++] = p->reason;
	} else if(h->type == PACKET_PROJECTILE_UPDATE) {
		const ProjectileUpdatePacket* p = buf;

		if(p->count > 0) {
			g_update = p->projectiles[0];
			g_update_seen = 1;
		}
	} else if(h->type == PACKET_PROJECTILE_SPAWN) {
		memcpy(&g_spawn, buf, sizeof(g_spawn));
	}
	return (ssize_t)len;
}

int interest_collect_fds(float x, float y, float radius, int* out_fds,
                         int max_out) {
	(void)x;
	(void)y;
	(void)radius;
	if(max_out < 1)
		return 0;
	out_fds[0] = VIEWER_FD;
	return 1;
}

int npc_snapshot_query(NPCTickSnapshot* snapshot, float x, float y,
                       float radius, int* out_indices, int max_out) {
	int n = 0;

	(void)x;
	(void)y;
	(void)radius;
	for(int i = 0; i < snapshot->count && n < max_out; i++)
		out_indices[n++] = i;
	return n;
}

int tick_snapshot_query(TickSnapshot* snapshot, float x, float y, float radius,
                        int* out_indices, int max_out) {
	int n = 0;

	(void)x;
	(void)y;
	(void)radius;
	for(int i = 0; i < snapshot->count && n < max_out; i++)
		out_indices[n++] = i;
	return n;
}

NPCEntity* npc_world_acquire_slot(NPCWorld* world, int slot,
                                  uint32_t expected_id) {
	(void)world;
	if(slot < 0 || slot >= TEST_NPCS || g_npc[slot].id != expected_id)
		return NULL;
	return &g_npc[slot];
}

void npc_world_release(NPCWorld* world, NPCEntity* npc) {
	(void)world;
	(void)npc;
}

int npc_mitigation_apply(NPCWorld* world, int slot, NPCEntity* npc, int damage,
                         float from_x, float from_y, double now,
                         int* out_blocked) {
	(void)world;
	(void)slot;
	(void)npc;
	(void)from_x;
	(void)from_y;
	(void)now;
	if(out_blocked)
		*out_blocked = 0;
	return damage;
}

void npc_trigger_note_damage(NPCEntity* npc, float from_x, float from_y) {
	(void)npc;
	(void)from_x;
	(void)from_y;
}

void party_award_xp(uint32_t killer_id, uint64_t xp_amount) {
	(void)killer_id;
	(void)xp_amount;
}

ActivePlayer* player_acquire(uint32_t character_id) {
	(void)character_id;
	return NULL;
}

void player_release(ActivePlayer* player) {
	(void)player;
}

void player_registry_rdlock(void) {
}

void player_registry_unlock(void) {
}

void player_collect_modifiers(const ActivePlayer* player, int ally_count,
                              DamageModifiers* out) {
	(void)player;
	(void)ally_count;
	damage_mods_reset(out);
}

void player_add_rage(ActivePlayer* player, int damage_dealt, int damage_taken) {
	(void)player;
	(void)damage_dealt;
	(void)damage_taken;
}

void player_send_kill_reward_locked(int client_fd, ActivePlayer* player,
                                    uint32_t xp) {
	(void)client_fd;
	(void)player;
	(void)xp;
}

int loot_roll(uint16_t npc_type_id, float x, float y, float z,
			  uint32_t killer_id) {
	(void)npc_type_id;
	(void)x;
	(void)y;
	(void)z;
	(void)killer_id;
	return 0;
}

void quest_on_npc_kill(uint32_t character_id, int client_fd,
                       uint16_t npc_type_id) {
	(void)character_id;
	(void)client_fd;
	(void)npc_type_id;
}

static void reset(void) {
	int ok = projectile_init(16);

	assert(ok);
	g_shot_speed = 100.0f;
	g_destroy_count = 0;
	g_update_seen = 0;
	memset(&g_spawn, 0, sizeof(g_spawn));
	memset(g_npc, 0, sizeof(g_npc));
	memset(&g_npcs, 0, sizeof(g_npcs));
	memset(&g_players, 0, sizeof(g_players));
	memset(active_players, 0, sizeof(active_players));
	g_npcs.slot = g_snap_slot;
	g_npcs.id = g_snap_id;
	g_npcs.pos_x = g_snap_x;
	g_npcs.pos_y = g_snap_y;
	g_npcs.pos_z = g_snap_z;
	g_npcs.hitbox_radius = g_snap_r;
	g_npcs.max_hitbox_radius = 20.0f;
}

// Put an NPC with full health in slot i of both the pool and the snapshot.
static void place_npc(int i, float x, float y, float z) {
	g_npc[i].id = 100u + (uint32_t)i;
	g_npc[i].is_alive = 1;
	g_npc[i].health = 1000;
	g_npc[i].max_health = 1000;
	g_npc[i].pos_x = x;
	g_npc[i].pos_y = y;
	g_snap_slot[i] = i;
	g_snap_id[i] = g_npc[i].id;
	g_snap_x[i] = x;
	g_snap_y[i] = y;
	g_snap_z[i] = z;
	g_snap_r[i] = 10.0f;
	if(g_npcs.count < i + 1)
		g_npcs.count = i + 1;
}

// Put a loaded player with full health in active slot i and dense index i.
static void place_player(int i, float x, float y, float z) {
	active_players[i].is_loaded = 1;
	active_players[i].character_id = 500u + (uint32_t)i;
	active_players[i].client_fd = -1;
	active_players[i].health = 1000;
	active_players[i].pos_x = x;
	active_players[i].pos_y = y;
	active_players[i].pos_z = z;
	g_players.slot[i] = i;
	g_players.character_id[i] = active_players[i].character_id;
	g_players.pos_x[i] = x;
	g_players.pos_y[i] = y;
	g_players.pos_z[i] = z;
	if(g_players.count < i + 1)
		g_players.count = i + 1;
}

// Spawn one shot at g_shot_speed and check the pool took it.
static void shoot(uint8_t owner, float ox, float oy, float oz, float ax,
                  float ay, float az, float range) {
	ProjectileSpawnInfo info = {0};
	uint32_t id;

	info.owner_type = owner;
	info.owner_id = 1;
	info.owner_fd = -1;
	info.origin_x = ox;
	info.origin_y = oy;
	info.origin_z = oz;
	info.aim_x = ax;
	info.aim_y = ay;
	info.aim_z = az;
	info.speed = g_shot_speed;
	info.width = 4.0f;
	info.max_range = range;
	info.damage = 50;
	id = projectile_spawn(&info);
	assert(id != 0);
}

// Read the first live projectile's position through the batch broadcast.
static int peek(float* x, float* y, float* z) {
	BroadcastPlayer viewer = {0};
	BroadcastSnapshot bs = {0};

	viewer.client_fd = VIEWER_FD;
	viewer.pos_x = 300.0f;
	viewer.pos_y = 100.0f;
	bs.players = &viewer;
	bs.count = 1;
	g_update_seen = 0;
	projectile_broadcast(&bs, 0, 1);
	if(!g_update_seen)
		return 0;
	*x = g_update.pos_x;
	*y = g_update.pos_y;
	*z = g_update.pos_z;
	return 1;
}

// The shot is still in flight; *x, *y, *z is where.
static void peek_live(float* x, float* y, float* z) {
	int seen = peek(x, y, z);

	assert(seen);
}

// The shot is gone.
static void peek_gone(void) {
	float x;
	float y;
	float z;
	int seen = peek(&x, &y, &z);

	assert(!seen);
}

static void tick(int n) {
	for(int i = 0; i < n; i++)
		projectile_tick(NULL, &g_players, &g_npcs, DT);
}

static void test_level_shot(void) {
	float x;
	float y;
	float z;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 0, 300, 100, 0, 400);
	assert(g_spawn.pos_z == 24.0f && g_spawn.dir_z == 0.0f);
	assert(g_spawn.dir_x == 1.0f && g_spawn.dir_y == 0.0f);
	tick(10);
	peek_live(&x, &y, &z);
	assert(fabsf(x - 150.0f) < 0.01f && y == 100.0f && z == 24.0f);
	assert(g_destroy_count == 0);

	// A body starting at z 100 is under the shot's x/y but far above it.
	place_npc(0, 160, 100, 100);
	tick(4);
	assert(g_npc[0].health == 1000);
	assert(g_destroy_count == 0);

	// A body standing on the ground the shot was fired from.
	place_npc(1, 185, 100, 0);
	tick(4);
	assert(g_npc[1].health < 1000);
	assert(g_npc[0].health == 1000);
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_HIT);
}

static void test_level_shot_expires(void) {
	float x;
	float y;
	float z;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 0, 100, 300, 0, 50);
	tick(9);
	peek_live(&x, &y, &z);
	assert(x == 100.0f && fabsf(y - 145.0f) < 0.01f && z == 24.0f);
	tick(1);
	peek_gone();
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_EXPIRED);
}

static void test_uphill_shot(void) {
	float x;
	float y;
	float z;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	reset();

	// 48 up over 200 across: the chest line rises 0.24 per unit of run.
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 0, 300, 100, 48, 400);
	assert(g_spawn.pos_z == 24.0f && g_spawn.dir_z > 0.0f);
	tick(20);
	peek_live(&x, &y, &z);
	assert(fabsf(z - (24.0f + (x - 100.0f) * 0.24f)) < 0.01f);
	assert(x > 190.0f && x < 210.0f);
	assert(fabsf(z - 48.0f) < 2.0f);
}

static void test_into_the_floor(void) {
	float x;
	float y;
	float z;
	int ticks = 0;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	reset();

	// From a ledge at 60 down to the ground 100 across: chest 84 -> 24, so
	// the line crosses z 0 at x 240. Below the floor it would meet a body at
	// -20 (-9 at x 255), which only the floor stop keeps it from hitting.
	place_npc(0, 255, 100, -20);
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 60, 200, 100, 0, 400);
	assert(g_spawn.pos_z == 84.0f && g_spawn.dir_z < 0.0f);
	while(peek(&x, &y, &z) && ticks < 200) {
		assert(z >= 0.0f);
		tick(1);
		ticks++;
	}
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_EXPIRED);
	assert(ticks < 40);
	assert(x < 241.0f);
	assert(g_npc[0].health == 1000);
}

static void test_doorway(void) {
	float x;
	float y;
	float z;
	const float y5 = FIX_CX(5);
	int loaded;

	fix_write_city(g_fixture_path);
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	loaded = world_spans_load(g_fixture_path);
	assert(loaded);

	// Tile 22 is a doorway with a lintel at 30: chest height 24 goes under.
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, FIX_CX(21), y5, 0, FIX_CX(23), y5, 0,
	             30);
	tick(5);
	peek_live(&x, &y, &z);
	assert(x > 368.0f && z == 24.0f);
	assert(g_destroy_count == 0);

	// Fired from 10 up, the chest line is at 34 and meets the lintel.
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, FIX_CX(21), y5, 10, FIX_CX(23), y5,
	             10, 30);
	tick(5);
	peek_gone();
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_EXPIRED);
	world_spans_shutdown();
	unlink(g_fixture_path);
}

// One wall tile, 16 thick and 100 tall, at tile 20 of row 8: x 320 to 336.
#define WALL_Y FIX_CX(8)

static void load_wall(void) {
	static const fix_record recs[] = {{FIX_T(20, 8), 0, 1}};
	static const world_span spans[] = {{100, FIX_OPEN}};
	int loaded;

	fix_write(g_fixture_path, FIX_W, FIX_H, FIX_TILE, recs, 1, spans, 1,
	          WORLD_SPANS_VERSION, 0);
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	loaded = world_spans_load(g_fixture_path);
	assert(loaded);
	unlink(g_fixture_path);
}

static void test_wall_not_tunnelled(void) {
	load_wall();
	reset();
	g_shot_speed = 900.0f;

	// 45 units a tick: the ends of the moves (309, 354) straddle the wall.
	place_npc(0, 360, WALL_Y, 0);
	shoot(PROJECTILE_OWNER_PLAYER, 264, WALL_Y, 0, 500, WALL_Y, 0, 1000);
	tick(3);
	peek_gone();
	assert(g_npc[0].health == 1000);
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_EXPIRED);
	world_spans_shutdown();
}

static void test_target_hugging_wall(void) {
	load_wall();
	reset();
	g_shot_speed = 900.0f;

	// The move ends inside the wall (325); the body touching its face is hit.
	place_npc(0, 310, WALL_Y, 0);
	shoot(PROJECTILE_OWNER_PLAYER, 280, WALL_Y, 0, 500, WALL_Y, 0, 1000);
	tick(1);
	assert(g_npc[0].health < 1000);
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_HIT);
	world_spans_shutdown();
}

static void test_shooter_against_wall(void) {
	load_wall();

	// Standing 8 short of the face, the first move would end at 357.
	reset();
	g_shot_speed = 900.0f;
	place_npc(0, 360, WALL_Y, 0);
	shoot(PROJECTILE_OWNER_PLAYER, 312, WALL_Y, 0, 500, WALL_Y, 0, 1000);
	tick(2);
	peek_gone();
	assert(g_npc[0].health == 1000);
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_EXPIRED);

	// A spawn point inside the wall dies on its first tick.
	reset();
	g_shot_speed = 900.0f;
	place_npc(0, 360, WALL_Y, 0);
	shoot(PROJECTILE_OWNER_PLAYER, 328, WALL_Y, 0, 500, WALL_Y, 0, 1000);
	tick(1);
	peek_gone();
	assert(g_npc[0].health == 1000);
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_EXPIRED);
	world_spans_shutdown();
}

static void test_range_is_horizontal(void) {
	float x;
	float y;
	float z;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);

	// Level, range 100 at 5 a tick: gone on tick 20.
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 0, 300, 100, 0, 100);
	tick(19);
	peek_live(&x, &y, &z);
	tick(1);
	peek_gone();

	// Pitched 48 over 200: 4.86 across a tick, so it is still out at 97.
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 0, 300, 100, 48, 100);
	tick(20);
	peek_live(&x, &y, &z);
	assert(x > 195.0f && x < 200.0f);
	tick(1);
	peek_gone();
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_EXPIRED);

	// Straight up never covers ground; its path is capped at twice the range.
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 0, 100, 100, 100, 50);
	tick(20);
	peek_gone();
	assert(g_destroy_count == 1);
}

static void test_nan_origin(void) {
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	reset();
	place_npc(0, 105, 100, 0);
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, NAN, 300, 100, 0, 400);
	tick(1);
	peek_gone();
	assert(g_npc[0].health == 1000);
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_EXPIRED);
}

static void test_npc_shot_at_players(void) {
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	reset();

	// One player on a platform 100 up, one on the ground behind them.
	place_player(0, 150, 100, 100);
	place_player(1, 200, 100, 0);
	shoot(PROJECTILE_OWNER_NPC, 100, 100, 0, 300, 100, 0, 400);
	tick(20);
	assert(active_players[0].health == 1000);
	assert(active_players[1].health < 1000);
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_HIT);
}

// An NPC aims a full range out along the line to its target (npc_delivery.c),
// so the aim height is extrapolated from the target's height at its distance.
static void npc_shoot_player(float npc_z, float player_z, float dist) {
	const float range = 400.0f;
	float aim_z = projectile_extrapolate_aim_z(npc_z, player_z, range, dist);

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	reset();
	place_player(0, 100 + dist, 100, player_z);
	shoot(PROJECTILE_OWNER_NPC, 100, 100, npc_z, 100 + range, 100, aim_z,
	      range);
	tick(100);
	assert(active_players[0].health < 1000);
	assert(g_destroy_count == 1);
	assert(g_destroy_reason[0] == PROJECTILE_DESTROY_HIT);
}

static void test_npc_pitch(void) {
	// 50 up at half range, 60 up at a quarter, 50 down at a third.
	npc_shoot_player(0, 50, 200);
	npc_shoot_player(0, 60, 100);
	npc_shoot_player(50, 0, 400.0f / 3.0f);
	assert(projectile_extrapolate_aim_z(0, 50, 400, 0) == 20000.0f);
	assert(projectile_extrapolate_aim_z(0, NAN, 400, 100) == 0.0f);
}

static void test_bad_aim(void) {
	float x;
	float y;
	float z;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);

	// A non-finite aim height falls back to a level shot along the same line.
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 0, 100, 300, NAN, 400);
	assert(g_spawn.dir_x == 0.0f && g_spawn.dir_y == 1.0f);
	assert(g_spawn.dir_z == 0.0f);
	tick(2);
	peek_live(&x, &y, &z);
	assert(z == 24.0f);

	// Aim at the origin itself: today's +x fallback, level.
	reset();
	shoot(PROJECTILE_OWNER_PLAYER, 100, 100, 0, 100, 100, 0, 400);
	assert(g_spawn.dir_x == 1.0f && g_spawn.dir_z == 0.0f);
}

int main(void) {
	snprintf(g_fixture_path, sizeof(g_fixture_path),
	         "/tmp/projectile_test_spans_%d.bin", (int)getpid());
	test_level_shot();
	test_level_shot_expires();
	test_uphill_shot();
	test_into_the_floor();
	test_doorway();
	test_npc_shot_at_players();
	test_wall_not_tunnelled();
	test_target_hugging_wall();
	test_shooter_against_wall();
	test_range_is_horizontal();
	test_nan_origin();
	test_npc_pitch();
	test_bad_aim();
	projectile_cleanup();
	printf("projectile_test: ok\n");
	return 0;
}

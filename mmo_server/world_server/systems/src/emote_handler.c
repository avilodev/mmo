// Apply emote requests to the player table, and end emotes the server must.

#include "emote_handler.h"
#include "outbox.h"
#include "emote_validator.h"
#include "combat_stats.h"
#include "log.h"
#include "player_data.h"
#include "utils.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>

extern ActivePlayer active_players[];

// Held around "is this seat free" and "take it"; taken before any other lock.
static pthread_mutex_t g_seat_mutex = PTHREAD_MUTEX_INITIALIZER;

// The facts about the asking player, copied under their lock.
typedef struct {
	emote_actor actor;
	uint8_t emote;
	uint8_t arg;
} asker_facts;

// One emote the upkeep ended, reported once no lock is held.
typedef struct {
	int client_fd;
	uint32_t character_id;
	uint8_t was;
	uint8_t result;
} ended_emote;

void emote_send_state(int client_fd, uint32_t character_id, uint8_t emote,
		uint8_t arg, uint8_t result, uint8_t requested) {
	EmoteStatePacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_EMOTE_STATE;
	pkt.header.player_id = htonl(character_id);
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.emote = emote;
	pkt.arg = arg;
	pkt.result = result;
	pkt.requested = requested;
	outbox_send(client_fd, &pkt, sizeof(pkt));
}

// Returns 0 when the player is not in the world.
static int read_asker(uint32_t character_id, int player_slot, asker_facts* out) {
	ActivePlayer* p = player_acquire_hint(character_id, player_slot);

	if(!p)
		return 0;
	if(!p->is_loaded) {
		player_release(p);
		return 0;
	}

	out->actor.x = p->pos_x;
	out->actor.y = p->pos_y;
	out->actor.dead = p->is_dead;
	out->actor.in_combat =
		get_time_seconds() - p->last_combat_time < OOC_THRESHOLD_SECONDS;
	out->emote = p->emote;
	out->arg = p->emote_arg;
	player_release(p);

	return 1;
}

// Walks the online players one slot lock at a time, as the snapshot does.
static int seat_held_by_another(void* ctx, int seat) {
	uint32_t self = *(const uint32_t*)ctx;
	int taken = 0;
	int online_count = 0;

	player_registry_rdlock();

	const int* online = player_active_list_locked(&online_count);

	for(int n = 0; n < online_count && !taken; n++) {
		ActivePlayer* p = &active_players[online[n]];

		if(!p->is_loaded)
			continue;

		pthread_mutex_lock(&p->lock);
		taken = p->character_id != self && p->emote == EMOTE_SIT &&
			p->emote_arg == seat;
		pthread_mutex_unlock(&p->lock);
	}
	player_registry_unlock();

	return taken;
}

// A refusal changes nothing.
static void commit(uint32_t character_id, int player_slot, EmoteResult verdict,
		uint8_t emote, uint8_t arg) {
	ActivePlayer* p = player_acquire_hint(character_id, player_slot);

	if(!p)
		return;

	if(verdict == EMOTE_RESULT_STARTED) {
		p->emote = emote;
		p->emote_arg = arg;
		p->emote_started_at = get_time_seconds();
	} else if(verdict == EMOTE_RESULT_STOPPED) {
		p->emote = EMOTE_NONE;
		p->emote_arg = 0;
	}
	player_release(p);
}

void handle_emote_request(int client_fd, uint32_t character_id, int player_slot,
		const EmoteRequestPacket* pkt) {
	asker_facts facts;
	uint8_t emote = pkt->emote;
	uint8_t arg = pkt->arg;
	EmoteResult verdict;
	uint8_t now_emote;
	uint8_t now_arg;

	if(!read_asker(character_id, player_slot, &facts))
		return;

	pthread_mutex_lock(&g_seat_mutex);
	verdict = emote_validate(emote, arg, &facts.actor, seat_held_by_another,
	                         &character_id);
	if(verdict == EMOTE_RESULT_STARTED || verdict == EMOTE_RESULT_STOPPED)
		commit(character_id, player_slot, verdict, emote, arg);
	pthread_mutex_unlock(&g_seat_mutex);

	// What the player is in now: the new emote if it started, else the old.
	now_emote = facts.emote;
	now_arg = facts.arg;
	if(verdict == EMOTE_RESULT_STARTED) {
		now_emote = emote;
		now_arg = arg;
	} else if(verdict == EMOTE_RESULT_STOPPED) {
		now_emote = EMOTE_NONE;
		now_arg = 0;
	}
	emote_send_state(client_fd, character_id, now_emote, now_arg,
	                 (uint8_t)verdict, emote);
	LOG_DEBUG("[EMOTE] character %u asked for %u (arg %u): result %u",
	          character_id, emote, arg, (unsigned)verdict);
}

void emote_upkeep_tick(void) {
	static ended_emote ended[MAX_PLAYERS];
	int count = 0;
	int online_count = 0;

	player_registry_rdlock();

	const int* online = player_active_list_locked(&online_count);

	for(int n = 0; n < online_count; n++) {
		ActivePlayer* p = &active_players[online[n]];

		if(!p->is_loaded)
			continue;

		pthread_mutex_lock(&p->lock);
		if(p->emote != EMOTE_NONE) {
			EmoteResult why = emote_upkeep(p->is_dead, p->emote_started_at,
			                               p->last_combat_time);

			if(why != EMOTE_RESULT_STARTED && count < MAX_PLAYERS) {
				ended[count].client_fd = p->client_fd;
				ended[count].character_id = p->character_id;
				ended[count].was = p->emote;
				ended[count].result = (uint8_t)why;
				count++;
				p->emote = EMOTE_NONE;
				p->emote_arg = 0;
			}
		}
		pthread_mutex_unlock(&p->lock);
	}
	player_registry_unlock();

	for(int i = 0; i < count; i++) {
		emote_send_state(ended[i].client_fd, ended[i].character_id, EMOTE_NONE,
		                 0, ended[i].result, ended[i].was);
	}
}

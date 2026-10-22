// Send the forecast to players at join and at the start of each weather day.

#include "weather_service.h"
#include "weather_model.h"
#include "outbox.h"
#include "player_data.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

extern ActivePlayer active_players[];

// The weather day the last broadcast was for; unset until the first upkeep
static int64_t g_last_day;
static int g_last_day_set;

// Who the broadcast in progress goes to; only the upkeep uses it
typedef struct {
	int client_fd;
	uint32_t character_id;
} Recipient;

static Recipient g_recipients[MAX_PLAYERS];

static int64_t unix_ms_now(void) {
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);

	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void weather_service_send(int client_fd, uint32_t character_id) {
	WeatherForecastPacket pkt;

	weather_build_packet(&pkt, character_id, unix_ms_now());
	outbox_send(client_fd, &pkt, sizeof(pkt));
}

// Copy out who is in the world, so nothing is sent while a lock is held
static int collect_recipients(void) {
	int online_count = 0;
	int count = 0;
	const int* online;

	player_registry_rdlock();
	online = player_active_list_locked(&online_count);

	for(int n = 0; n < online_count && count < MAX_PLAYERS; n++) {
		ActivePlayer* p = &active_players[online[n]];

		if(!p->is_loaded)
			continue;

		pthread_mutex_lock(&p->lock);
		if(p->client_fd >= 0) {
			g_recipients[count].client_fd = p->client_fd;
			g_recipients[count].character_id = p->character_id;
			count++;
		}
		pthread_mutex_unlock(&p->lock);
	}
	player_registry_unlock();

	return count;
}

void weather_service_upkeep(void) {
	int64_t today = weather_day_index(unix_ms_now(), weather_epoch_s(), weather_day_length_ms());
	int count;

	if(!g_last_day_set) {
		g_last_day = today;
		g_last_day_set = 1;
		return;
	}
	if(today == g_last_day)
		return;
	g_last_day = today;

	count = collect_recipients();
	for(int i = 0; i < count; i++)
		weather_service_send(g_recipients[i].client_fd, g_recipients[i].character_id);
}

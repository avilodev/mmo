/**
 * @file
 * Handle movement, entity, dialogue, chat, party, quest, and zone packets.
 *
 * One of the four domain dispatchers split out of network.c's single
 * seventy-case switch. See net_internal.h for how they fit together.
 */

#include "net_internal.h"
#include "network/name_cache.h"
#include "ability_bar.h"
#include "combat_system.h"
#include "combat_render.h"
#include "inventory.h"
#include "npc_types.h"
#include "audio/audio.h"
#include "ui/quest_log.h"
#include "states/chat_log.h"
#include "states/join_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

// Join packets that arrive before the gameplay state exists, held in order.
//
// The server sends them as soon as the world connection opens, which is during
// character select: one millisecond before gameplay is entered in the logs.
// Dropping them left the quest log, the marks and the available list empty,
// and the stats and ability bars with them.
#define EARLY_QUEST_CAP (256 * 1024)

static unsigned char* g_early_quest;
static size_t         g_early_quest_len;
static size_t         g_early_quest_cap;

// Whether a packet is one the server pushes at join rather than on request.
static int is_join_push_packet(uint8_t type) {
	switch(type) {
	case PACKET_PLAYER_STATS:
	case PACKET_ABILITY_DATA:
	// The first nearby lists: an empty one is sent once, so losing it would be for good
	case PACKET_NPC_POSITIONS:
	case PACKET_PLAYER_POSITIONS:
	case PACKET_QUEST_ACCEPT:
	case PACKET_QUEST_PROGRESS:
	case PACKET_NPC_QUEST_MARKS:
	case PACKET_AVAILABLE_QUESTS_RESET:
	case PACKET_AVAILABLE_QUEST:
	case PACKET_QUEST_HISTORY:
		return 1;
	default:
		return 0;
	}
}

// Whether the gameplay state exists to receive quest packets.
static int playing_is_ready(void) {
	return g_current_game && g_current_game->playing;
}

// Keep one packet, length first, for replay once gameplay starts.
static void stash_early_packet(const char* data, int length) {
	size_t need = g_early_quest_len + sizeof(int) + (size_t)length;

	if(length <= 0 || need > EARLY_QUEST_CAP) {
		CLOG_WARN("[NET] Early join packet dropped: %d bytes, %d already held", length,
		          (int)g_early_quest_len);
		return;
	}
	if(need > g_early_quest_cap) {
		size_t grown = g_early_quest_cap ? g_early_quest_cap * 2 : 4096;
		unsigned char* bigger;

		while(grown < need)
			grown *= 2;
		bigger = realloc(g_early_quest, grown);
		if(!bigger) {
			CLOG_WARN("[NET] Early join packet dropped: out of memory");
			return;
		}
		g_early_quest = bigger;
		g_early_quest_cap = grown;
	}
	memcpy(g_early_quest + g_early_quest_len, &length, sizeof(int));
	memcpy(g_early_quest + g_early_quest_len + sizeof(int), data, (size_t)length);
	g_early_quest_len = need;
}

// Show the popup a finished quest or step earns, and refresh balances and XP.
static void show_quest_reward(uint32_t xp, uint32_t coin, uint8_t currency_id) {
	PlayingState* ps = g_current_game->playing;

	// reuse the kill-reward popup
	for(int i = 0; i < MAX_REWARD_POPUPS; i++) {
		if(ps->reward_notifications[i].active)
			continue;
		ps->reward_notifications[i].xp_gained = xp;
		ps->reward_notifications[i].currency_gained = coin;
		ps->reward_notifications[i].currency_id = currency_id;
		ps->reward_notifications[i].age = 0.0f;
		ps->reward_notifications[i].active = 1;
		break;
	}
	network_request_player_data_refresh();
}

void net_world_forget_early_quest(void) {
	g_early_quest_len = 0;
}

void network_replay_early_quest_packets(void) {
	unsigned char* held = g_early_quest;
	size_t held_len = g_early_quest_len;
	size_t at = 0;
	int replayed = 0;

	// Taken first: a replayed packet must never be stashed again.
	g_early_quest = NULL;
	g_early_quest_len = 0;
	g_early_quest_cap = 0;
	while(held && at + sizeof(int) <= held_len) {
		int length;

		memcpy(&length, held + at, sizeof(int));
		at += sizeof(int);
		if(length <= 0 || at + (size_t)length > held_len)
			break;
		net_dispatch_packet(held[at], (const char*)(held + at), length);
		at += (size_t)length;
		replayed++;
	}
	free(held);
	if(replayed > 0)
		CLOG_INFO("[NET] Replayed %d join packet(s) that arrived before gameplay", replayed);
}

/**
 * Dispatch one world packet.
 *
 * @param type  Opcode from the packet header.
 * @param data  Buffer holding one complete packet.
 * @param length  Available packet length in bytes.
 * @return 1 when this domain handled the opcode, otherwise 0.
 */
int net_dispatch_world(uint8_t type, const char* data, int length) {
    (void)data; (void)length;

	if(is_join_push_packet(type) && !playing_is_ready()) {
		stash_early_packet(data, length);
		return 1;
	}

    switch (type) {
        case PACKET_PLAYER_MOVE_ACK:
            if (length >= (int)sizeof(PlayerMoveAckPacket)) {
                PlayerMoveAckPacket* ack = (PlayerMoveAckPacket*)data;
                EnterCriticalSection(&g_net.response_lock);
				g_net.correction.data.x = ack->pos_x;
				g_net.correction.data.y = ack->pos_y;
				g_net.correction.data.z = ack->pos_z;
				g_net.correction.data.vz = ack->vel_z;
				g_net.correction.data.grounded = ack->grounded != 0;
				g_net.correction.data.sequence = ntohl(ack->sequence);
				g_net.correction.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
            }
            break;

		case PACKET_EMOTE_STATE:
			if(length >= (int)sizeof(EmoteStatePacket)) {
				const EmoteStatePacket* pkt = (const EmoteStatePacket*)data;

				EnterCriticalSection(&g_net.response_lock);
				int slot;
				if(g_net.emote_events.count == 8) {   // full: drop the oldest
					g_net.emote_events.head = (g_net.emote_events.head + 1) % 8;
					g_net.emote_events.count--;
				}
				slot = (g_net.emote_events.head + g_net.emote_events.count) % 8;
				g_net.emote_events.entries[slot][0] = pkt->emote;
				g_net.emote_events.entries[slot][1] = pkt->arg;
				g_net.emote_events.entries[slot][2] = pkt->result;
				g_net.emote_events.entries[slot][3] = pkt->requested;
				g_net.emote_events.count++;
				LeaveCriticalSection(&g_net.response_lock);
				NET_LOG("[NET] Emote state: emote %u arg %u result %u\n",
				        pkt->emote, pkt->arg, pkt->result);
			}
			break;

		case PACKET_STATUS_STATE:
			if(length >= (int)sizeof(StatusStatePacket)) {
				const StatusStatePacket* pkt = (const StatusStatePacket*)data;

				EnterCriticalSection(&g_net.response_lock);
				g_net.status_event.status = pkt->status;
				g_net.status_event.ready = TRUE;
				LeaveCriticalSection(&g_net.response_lock);
				NET_LOG("[NET] Status state: %u\n", pkt->status);
			}
			break;

		case PACKET_WORLD_TIME:
			if(length >= (int)sizeof(WorldTimePacket)) {
				const WorldTimePacket* pkt = (const WorldTimePacket*)data;
				uint32_t time_of_day_ms = ntohl(pkt->time_of_day_ms);
				uint32_t day_length_ms = ntohl(pkt->day_length_ms);

				EnterCriticalSection(&g_net.response_lock);
				g_net.world_time_event.time_of_day_ms = time_of_day_ms;
				g_net.world_time_event.day_length_ms = day_length_ms;
				g_net.world_time_event.ready = TRUE;
				LeaveCriticalSection(&g_net.response_lock);
				NET_LOG("[NET] World time: %u of %u ms\n", (unsigned)time_of_day_ms,
				        (unsigned)day_length_ms);
			}
			break;

		case PACKET_WEATHER_FORECAST:
			if(length >= (int)sizeof(WeatherForecastPacket)) {
				const WeatherForecastPacket* pkt = (const WeatherForecastPacket*)data;
				WeatherForecast forecast;

				memset(&forecast, 0, sizeof(forecast));
				forecast.day_index = ntohl(pkt->day_index);
				forecast.day_length_ms = ntohl(pkt->day_length_ms);
				forecast.ms_to_next_day = ntohl(pkt->ms_to_next_day);
				forecast.region_count = pkt->region_count;
				memcpy(forecast.days, pkt->days, sizeof(forecast.days));

				EnterCriticalSection(&g_net.response_lock);
				g_net.weather_event.forecast = forecast;
				g_net.weather_event.ready = TRUE;
				LeaveCriticalSection(&g_net.response_lock);
				NET_LOG("[NET] Weather forecast: day %u, %d regions\n",
				        (unsigned)forecast.day_index, forecast.region_count);
			}
			break;

        case PACKET_NPC_POSITIONS: {
            size_t base_size = offsetof(NPCPositionPacket, npcs);

            if (length < (int)base_size) {
                NET_WARN("[NET] ❌ NPC_POSITIONS packet too small: %d < %zu\n", length, base_size);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            NPCPositionPacket* pkt = (NPCPositionPacket*)data;
            uint8_t claimed_count = pkt->npc_count;

            size_t expected_size = base_size + (claimed_count * sizeof(NPCPositionData));

            if (length < (int)expected_size) {
                NET_WARN("[NET] ❌ NPC_POSITIONS incomplete: have %d bytes, need %zu for %d NPCs\n",
                       length, expected_size, claimed_count);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            if (claimed_count > MAX_NPCS_PER_PACKET) {
                NET_WARN("[NET] ⚠️ Server claims %d NPCs, clamping to %d\n",
                       claimed_count, MAX_NPCS_PER_PACKET);
                claimed_count = MAX_NPCS_PER_PACKET;
            }

            NET_LOG("[NET] NPC positions received: %d NPCs\n", claimed_count);

            if (g_current_game && g_current_game->playing) {
				join_data_note(JOIN_NPCS);
                int old_count = g_current_game->playing->visible_npc_count;
				// The list comes nearest-first, so an NPC changes slot as the player
				// moves. Reading the old state from a copy keeps each NPC's own name
				// and animation offset, not those of whoever held its slot before.
				VisibleNPC old_npcs[MAX_VISIBLE_NPCS];

				memcpy(old_npcs, g_current_game->playing->visible_npcs, sizeof(old_npcs));
                g_current_game->playing->visible_npc_count = claimed_count;

                for (int i = 0; i < claimed_count && i < MAX_VISIBLE_NPCS; i++) {
                    NPCPositionData* src = &pkt->npcs[i];
                    uint32_t npc_id = ntohl(src->npc_id);
                    float new_x = src->pos_x;
                    float new_y = src->pos_y;
                    float new_z = src->pos_z;

                    // Check if this NPC existed before to interpolate
                    int found = 0;
					VisibleNPC* slot = &g_current_game->playing->visible_npcs[i];

					memset(slot, 0, sizeof(*slot));
                    for (int j = 0; j < old_count && j < MAX_VISIBLE_NPCS; j++) {
                        if (old_npcs[j].npc_id == npc_id) {
							// Existing NPC: carry its state, interpolate from where it was drawn
							*slot = old_npcs[j];
							slot->prev_x = old_npcs[j].pos_x;
							slot->prev_y = old_npcs[j].pos_y;
							slot->prev_z = old_npcs[j].pos_z;
                            found = 1;
                            break;
                        }
                    }

                    g_current_game->playing->visible_npcs[i].npc_id = npc_id;
                    g_current_game->playing->visible_npcs[i].target_x = new_x;
                    g_current_game->playing->visible_npcs[i].target_y = new_y;
                    g_current_game->playing->visible_npcs[i].target_z = new_z;

                    if (!found) {
                        // New NPC: snap to position immediately
                        g_current_game->playing->visible_npcs[i].pos_x = new_x;
                        g_current_game->playing->visible_npcs[i].pos_y = new_y;
                        g_current_game->playing->visible_npcs[i].pos_z = new_z;
                        g_current_game->playing->visible_npcs[i].prev_x = new_x;
                        g_current_game->playing->visible_npcs[i].prev_y = new_y;
                        g_current_game->playing->visible_npcs[i].prev_z = new_z;
                        g_current_game->playing->visible_npcs[i].interp_t = 1.0f;
                    } else {
                        // Reset interpolation timer for smooth movement
                        g_current_game->playing->visible_npcs[i].interp_t = 0.0f;
                    }

                    g_current_game->playing->visible_npcs[i].health = ntohl(src->health);
                    g_current_game->playing->visible_npcs[i].max_health = ntohl(src->max_health);
                    g_current_game->playing->visible_npcs[i].is_alive = src->is_alive;
                    g_current_game->playing->visible_npcs[i].category = src->category;
                    g_current_game->playing->visible_npcs[i].is_interactable = src->is_interactable;
                    g_current_game->playing->visible_npcs[i].npc_type_id = src->npc_type_id;
                    if (!found) {
                        const char* type_name = npc_type_get_name(src->npc_type_id);
                        if (type_name) {
                            snprintf(g_current_game->playing->visible_npcs[i].name, 32, "%s", type_name);
                        } else {
                            snprintf(g_current_game->playing->visible_npcs[i].name, 32, "NPC_%u", npc_id);
                        }
                    }
                }
            }
            break;
        }

		case PACKET_INSPECT_RESPONSE:
			if(length >= (int)sizeof(InspectResponsePacket)) {
				EnterCriticalSection(&g_net.response_lock);
				memcpy(&g_net.inspect.data, data, sizeof(InspectResponsePacket));
				g_net.inspect.data.name[sizeof(g_net.inspect.data.name) - 1] = '\0';
				g_net.inspect.ready = TRUE;
				LeaveCriticalSection(&g_net.response_lock);
				NET_LOG("[NET] Inspect answer: target %u result %u\n",
				        (unsigned)ntohl(g_net.inspect.data.target_id),
				        g_net.inspect.data.result);
			}
			break;

        case PACKET_NPC_INTERACT_RESPONSE:
            if (length >= (int)sizeof(NPCInteractResponsePacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.npc_interact.data, data, sizeof(NPCInteractResponsePacket));
                g_net.npc_interact.data.npc_name[31] = '\0';
                g_net.npc_interact.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
                NET_LOG("[NET] NPC Interact Response: NPC %u, Dialogue %u, Page %u\n",
                       (uint32_t)ntohl(g_net.npc_interact.data.npc_id),
                       (uint32_t)ntohl(g_net.npc_interact.data.dialogue_id),
                       g_net.npc_interact.data.page_num);
            }
            break;

        case PACKET_DIALOGUE_UPDATE:
            if (length >= (int)sizeof(DialogueUpdatePacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.dialogue_update.data, data, sizeof(DialogueUpdatePacket));
                g_net.dialogue_update.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
                NET_LOG("[NET] Dialogue Update: Dialogue %u, Page %u\n",
                       (uint32_t)ntohl(g_net.dialogue_update.data.dialogue_id),
                       g_net.dialogue_update.data.page_num);
            }
            break;

        case PACKET_DIALOGUE_CLOSE:
            if (length >= (int)sizeof(DialogueClosePacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.dialogue_close.data, data, sizeof(DialogueClosePacket));
                g_net.dialogue_close.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
                NET_LOG("[NET] Dialogue Close: NPC %u\n",
                       (uint32_t)ntohl(g_net.dialogue_close.data.npc_id));
            }
            break;

        case PACKET_PLAYER_POSITIONS: {
            size_t base_size = offsetof(PlayerPositionBroadcastPacket, players);

            if (length < (int)base_size) {
                NET_WARN("[NET] ❌ PLAYER_POSITIONS packet too small: %d < %zu\n", length, base_size);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            PlayerPositionBroadcastPacket* pkt = (PlayerPositionBroadcastPacket*)data;
            uint8_t claimed_count = pkt->count;

			// TEMPORARY DIAGNOSTIC: how evenly the position broadcasts arrive
			{
				static double last_at, window_start, max_gap;
				static int arrived, over_100, over_250, over_500;
				double at = net_now();
				double gap = last_at > 0.0 ? at - last_at : 0.0;

				last_at = at;
				arrived++;
				if(gap > max_gap)
					max_gap = gap;
				if(gap > 0.5)
					over_500++;
				else if(gap > 0.25)
					over_250++;
				else if(gap > 0.1)
					over_100++;
				if(at - window_start >= 1.0) {
					CLOG_INFO("[DIAG] me=%u positions: %d in %.2fs, gaps >100ms %d, >250ms %d, >500ms %d, max %.0fms",
					          g_net.character_id, arrived, at - window_start, over_100, over_250, over_500, max_gap * 1000.0);
					window_start = at;
					arrived = over_100 = over_250 = over_500 = 0;
					max_gap = 0.0;
				}
			}

            size_t expected_size = base_size + (claimed_count * sizeof(NearbyPlayerData));

            if (length < (int)expected_size) {
                NET_WARN("[NET] ❌ PLAYER_POSITIONS incomplete: have %d bytes, need %zu for %d players\n",
                       length, expected_size, claimed_count);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            if (claimed_count > MAX_NEARBY_PLAYERS) {
                NET_WARN("[NET] ⚠️ Server claims %d players, clamping to %d\n",
                       claimed_count, MAX_NEARBY_PLAYERS);
                claimed_count = MAX_NEARBY_PLAYERS;
            }

            if (g_current_game && g_current_game->playing) {
				join_data_note(JOIN_PLAYERS);
                int old_count = g_current_game->playing->nearby_player_count;
                NearbyPlayer* nearby = g_current_game->playing->nearby_players;

                /* Where each player was being drawn before this broadcast, so
                 * the new position can be interpolated toward rather than
                 * snapped to. Read before the array is overwritten: the list
                 * is rebuilt in broadcast order, and a player can move to a
                 * different index between two broadcasts. */
                float prev_x[MAX_NEARBY_PLAYERS], prev_y[MAX_NEARBY_PLAYERS], prev_z[MAX_NEARBY_PLAYERS];
				emote_state prev_emote[MAX_NEARBY_PLAYERS];
				float prev_speed[MAX_NEARBY_PLAYERS];
                uint32_t prev_id[MAX_NEARBY_PLAYERS];
                int prev_count = old_count;
                if (prev_count > MAX_NEARBY_PLAYERS) prev_count = MAX_NEARBY_PLAYERS;
                for (int j = 0; j < prev_count; j++) {
                    prev_id[j] = nearby[j].player_id;
                    prev_x[j]  = nearby[j].pos_x;
                    prev_y[j]  = nearby[j].pos_y;
                    prev_z[j]  = nearby[j].pos_z;
					prev_emote[j] = nearby[j].emote;
					prev_speed[j] = nearby[j].speed;
                }

                g_current_game->playing->nearby_player_count = claimed_count;
                for (int i = 0; i < claimed_count; i++) {
                    NearbyPlayerData* src = &pkt->players[i];
                    uint32_t id = ntohl(src->player_id);
                    g_current_game->playing->nearby_players[i].player_id = id;

                    /* Interpolate from where this player was last drawn, the
                     * same way VisibleNPC does. A player who was not on screen
                     * a moment ago snaps into place instead: interpolating
                     * from wherever the slot happened to hold would slide them
                     * in from another part of the map. */
                    int seen = 0;
					memset(&nearby[i].emote, 0, sizeof(nearby[i].emote));
                    for (int j = 0; j < prev_count; j++) {
                        if (prev_id[j] != id) continue;
						nearby[i].emote = prev_emote[j];
                        nearby[i].prev_x = prev_x[j];
                        nearby[i].prev_y = prev_y[j];
                        nearby[i].prev_z = prev_z[j];
                        nearby[i].pos_x  = prev_x[j];
                        nearby[i].pos_y  = prev_y[j];
                        nearby[i].pos_z  = prev_z[j];
                        nearby[i].interp_t = 0.0f;
                        seen = 1;
                        break;
                    }
                    if (!seen) {
                        nearby[i].prev_x = src->pos_x;
                        nearby[i].prev_y = src->pos_y;
                        nearby[i].prev_z = src->pos_z;
                        nearby[i].pos_x  = src->pos_x;
                        nearby[i].pos_y  = src->pos_y;
                        nearby[i].pos_z  = src->pos_z;
                        nearby[i].interp_t = 1.0f;
                    }
                    nearby[i].target_x = src->pos_x;
                    nearby[i].target_y = src->pos_y;
                    nearby[i].target_z = src->pos_z;
                    nearby[i].airborne = src->airborne;
					// Their emote as the server sees it; first sight appears mid-emote
					emote_observe(&nearby[i].emote, src->emote, src->emote_arg, !seen);
                    g_current_game->playing->nearby_players[i].health = (int32_t)ntohl(src->health);
                    g_current_game->playing->nearby_players[i].max_health = (int32_t)ntohl(src->max_health);
                    g_current_game->playing->nearby_players[i].player_class = src->player_class;
                    g_current_game->playing->nearby_players[i].player_race = src->player_race;
					g_current_game->playing->nearby_players[i].appearance = ntohl(src->appearance);
					g_current_game->playing->nearby_players[i].speed = (float)ntohs(src->speed);
					g_current_game->playing->nearby_players[i].heading = src->heading;
					g_current_game->playing->nearby_players[i].idle_fidget = src->idle_fidget;
					// TEMPORARY DIAGNOSTIC: when a player's reported speed starts or stops
					for(int j = 0; j < prev_count; j++) {
						if(prev_id[j] != id)
							continue;
						if((prev_speed[j] > 0.0f) != (ntohs(src->speed) > 0))
							CLOG_INFO("[DIAG] me=%u sees player %u speed %.0f -> %u", g_net.character_id, id,
							          prev_speed[j], (unsigned)ntohs(src->speed));
						break;
					}
                    g_current_game->playing->nearby_players[i].level = src->level;
                    g_current_game->playing->nearby_players[i].is_dead = src->is_dead;
                    g_current_game->playing->nearby_players[i].ping_ms = ntohs(src->ping_ms);
                    /* The name comes from the cache, which asks the server
                     * for identifiers it has not seen before and keeps the
                     * answers for the session.
                     *
                     * This used to write "Player_<id>" -- a number where a
                     * name goes, over every stranger in the world -- because
                     * NearbyPlayerData carries no name. It still carries no
                     * name, on purpose: a name never changes, and putting one
                     * in this packet would resend 32 constant bytes per player
                     * twenty times a second. */
                    snprintf(g_current_game->playing->nearby_players[i].name, 32,
                             "%s", name_cache_lookup(id));
                }
            }
            break;
        }

        case PACKET_NAME_QUERY_RESPONSE: {
            size_t base_size = offsetof(NameQueryResponsePacket, entries);
            if (length < (int)base_size) {
                NET_WARN("[NET] NAME_QUERY_RESPONSE too short: %d bytes\n", length);
                return 1;
            }

            NameQueryResponsePacket* pkt = (NameQueryResponsePacket*)data;
            uint8_t claimed = pkt->count;
            if (claimed > MAX_NAME_QUERY) {
                NET_WARN("[NET] NAME_QUERY_RESPONSE claims %u names, clamping to %d\n",
                         claimed, MAX_NAME_QUERY);
                claimed = MAX_NAME_QUERY;
            }

            /* The declared count must be backed by bytes that arrived. */
            size_t needed = base_size + (size_t)claimed * sizeof(NameQueryEntry);
            if (length < (int)needed) {
                NET_WARN("[NET] NAME_QUERY_RESPONSE claims %u names but carries %d bytes\n",
                         claimed, length);
                return 1;
            }

            for (uint8_t i = 0; i < claimed; i++) {
                pkt->entries[i].name[sizeof(pkt->entries[i].name) - 1] = '\0';
                name_cache_store(ntohl(pkt->entries[i].character_id),
                                 pkt->entries[i].name);
            }
            NET_LOG("[NET] Name query resolved %u name(s)\n", claimed);
            break;
        }

        case PACKET_CHAT_MESSAGE:
            if (length >= (int)sizeof(ChatMessagePacket)) {
                ChatMessagePacket* pkt = (ChatMessagePacket*)data;
                pkt->sender_name[sizeof(pkt->sender_name) - 1] = '\0';
                pkt->message[MAX_CHAT_MESSAGE - 1] = '\0';
                NET_LOG("[CHAT] [ch%u] %s: %s\n",
                       pkt->channel, pkt->sender_name, pkt->message);
				if(g_current_game && g_current_game->playing)
					chat_log_receive(&g_current_game->playing->chat, pkt->channel,
							pkt->flags, pkt->sender_name, pkt->message);
            }
            break;

        case PACKET_PARTY_INVITE_NOTIFY:
            if (length >= (int)sizeof(PartyInviteNotifyPacket)) {
                PartyInviteNotifyPacket* pkt = (PartyInviteNotifyPacket*)data;
                pkt->from_name[31] = '\0';
                NET_LOG("[NET] Party invite from: %s\n", pkt->from_name);
                if (g_current_game && g_current_game->playing) {
                    g_current_game->playing->party.has_pending_invite = 1;
                    g_current_game->playing->party.invite_from_id = ntohl(pkt->from_id);
                    snprintf(g_current_game->playing->party.invite_from_name,
                             sizeof(g_current_game->playing->party.invite_from_name),
                             "%s", pkt->from_name);
                    g_current_game->playing->party.invite_timer = 30.0f;
                }
            }
            break;

        case PACKET_PARTY_UPDATE: {
            size_t base_size = offsetof(PartyUpdatePacket, members);

            if (length < (int)base_size) {
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            PartyUpdatePacket* pkt = (PartyUpdatePacket*)data;
            uint8_t claimed_count = pkt->member_count;

            size_t member_size = sizeof(pkt->members[0]);
            size_t expected_size = base_size + (claimed_count * member_size);

            if (length < (int)expected_size) {
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            if (claimed_count > MAX_PARTY_SIZE) {
                claimed_count = MAX_PARTY_SIZE;
            }

            NET_LOG("[NET] Party update: %u members\n", claimed_count);
            if (g_current_game && g_current_game->playing) {
                PartyState* ps = &g_current_game->playing->party;
                ps->party_id = ntohl(pkt->party_id);
                ps->leader_id = ntohl(pkt->leader_id);
                ps->member_count = claimed_count;
                ps->has_party = (claimed_count > 0) ? 1 : 0;
                ps->has_pending_invite = 0;
                for (int i = 0; i < claimed_count; i++) {
                    ps->members[i].id = ntohl(pkt->members[i].character_id);
                    strncpy(ps->members[i].name, pkt->members[i].name, 31);
                    ps->members[i].name[31] = '\0';
                    ps->members[i].level = pkt->members[i].level;
                    ps->members[i].player_class = pkt->members[i].player_class;
                    ps->members[i].health = (int32_t)ntohl(pkt->members[i].health);
                    ps->members[i].max_health = (int32_t)ntohl(pkt->members[i].max_health);
                    ps->members[i].mana = (int32_t)ntohl(pkt->members[i].mana);
                    ps->members[i].max_mana = (int32_t)ntohl(pkt->members[i].max_mana);
                }
            }
            break;
        }

        case PACKET_PARTY_DISBAND:
            NET_LOG("[NET] Party disbanded\n");
            if (g_current_game && g_current_game->playing) {
                memset(&g_current_game->playing->party, 0, sizeof(PartyState));
            }
            break;

        case PACKET_QUEST_ACCEPT:
            if (length >= (int)sizeof(QuestAcceptPacket) && g_current_game && g_current_game->playing) {
                QuestAcceptPacket* pkt = (QuestAcceptPacket*)data;
                uint32_t quest_id = ntohl(pkt->quest_id);
                pkt->title[sizeof(pkt->title) - 1] = '\0';

                uint8_t obj_count = pkt->obj_count;
                if (obj_count > MAX_QUEST_OBJECTIVES) obj_count = MAX_QUEST_OBJECTIVES;

                for (int i = 0; i < obj_count; i++)
                    pkt->objectives[i].description[63] = '\0';

                /* The objective records go across whole: they carry the target
                 * and the marker the tracker, badge and map all read. */
                quest_log_add(&g_current_game->playing->quest_log,
                              quest_id, pkt->title, obj_count, pkt->objectives);
				pkt->step_title[sizeof(pkt->step_title) - 1] = '\0';
				quest_log_set_step(&g_current_game->playing->quest_log, quest_id, pkt->step,
				                   pkt->step_count, pkt->step_title);
                NET_LOG("[NET] Quest accepted: id=%u '%s' step %u of %u\n", quest_id,
				        pkt->title, (unsigned)pkt->step, (unsigned)pkt->step_count);
            }
            break;

		case PACKET_QUEST_HISTORY:
			if(length >= (int)sizeof(QuestHistoryPacket) && g_current_game && g_current_game->playing) {
				QuestHistoryPacket* pkt = (QuestHistoryPacket*)data;

				pkt->title[sizeof(pkt->title) - 1] = '\0';
				quest_log_finished_add(&g_current_game->playing->quest_log,
				                       ntohl(pkt->quest_id), pkt->title);
			}
			break;

        case PACKET_NPC_QUEST_MARKS:
            if (length >= (int)sizeof(NPCQuestMarksPacket) && g_current_game && g_current_game->playing) {
                NPCQuestMarksPacket* pkt = (NPCQuestMarksPacket*)data;
                PlayingState* ps = g_current_game->playing;
                int count = pkt->count > MAX_NPC_QUEST_MARKS ? MAX_NPC_QUEST_MARKS : pkt->count;

                /* A replacement, not an addition: a type left out has nothing. */
                memset(ps->npc_quest_marks, 0, sizeof(ps->npc_quest_marks));
                for (int i = 0; i < count; i++) {
                    ps->npc_quest_marks[i] = pkt->entries[i];
                    ps->npc_quest_marks[i].title[sizeof(ps->npc_quest_marks[i].title) - 1] = '\0';
                }
                ps->npc_quest_mark_count = count;
                NET_LOG("[NET] Quest marks: %d NPC types\n", count);
            }
            break;

		case PACKET_AVAILABLE_QUESTS_RESET:
			if(length >= (int)sizeof(AvailableQuestsResetPacket) && g_current_game && g_current_game->playing) {
				// A replacement: the quests it lists follow, in order.
				quest_log_available_reset(&g_current_game->playing->quest_log);
				join_data_available_reset(ntohl(((AvailableQuestsResetPacket*)data)->count));
				CLOG_INFO("[NET] Available quests: %u follow",
				          (unsigned)ntohl(((AvailableQuestsResetPacket*)data)->count));
			}
			break;

		case PACKET_AVAILABLE_QUEST:
			if(length >= (int)sizeof(AvailableQuestPacket) && g_current_game && g_current_game->playing) {
				quest_log_available_add(&g_current_game->playing->quest_log,
				                        (const AvailableQuestPacket*)data);
				join_data_available_entry();
				CLOG_INFO("[NET] Available quest %u listed (%d held)",
				          (unsigned)ntohl(((const AvailableQuestPacket*)data)->quest_id),
				          g_current_game->playing->quest_log.available_count);
			} else {
				CLOG_WARN("[NET] Available quest packet too short: %d bytes, need %d", length,
				          (int)sizeof(AvailableQuestPacket));
			}
			break;

        case PACKET_QUEST_PROGRESS:
            if (length >= (int)sizeof(QuestProgressPacket) && g_current_game && g_current_game->playing) {
                QuestProgressPacket* pkt = (QuestProgressPacket*)data;
                uint32_t quest_id = ntohl(pkt->quest_id);
                int32_t current  = (int32_t)ntohl((uint32_t)pkt->current);
                int32_t required = (int32_t)ntohl((uint32_t)pkt->required);
                quest_log_update_progress(&g_current_game->playing->quest_log,
                                          quest_id, pkt->obj_index,
                                          current, required);
                NET_LOG("[NET] Quest progress: id=%u obj=%u %d/%d\n",
                       quest_id, pkt->obj_index, current, required);
            }
            break;

		case PACKET_QUEST_STEP_DONE:
			if(length >= (int)sizeof(QuestStepDonePacket) && g_current_game &&
			   g_current_game->playing) {
				QuestStepDonePacket* pkt = (QuestStepDonePacket*)data;

				// the quest goes on; the accept packet that follows describes the next step
				show_quest_reward(ntohl(pkt->xp_reward), ntohl(pkt->currency_reward),
				                  pkt->currency_id);
				NET_LOG("[NET] Quest step done: id=%u step %u of %u +%u XP\n",
				        ntohl(pkt->quest_id), (unsigned)pkt->step, (unsigned)pkt->step_count,
				        ntohl(pkt->xp_reward));
			}
			break;

        case PACKET_QUEST_COMPLETE:
            if (length >= (int)sizeof(QuestCompletePacket) && g_current_game && g_current_game->playing) {
                QuestCompletePacket* pkt = (QuestCompletePacket*)data;
                uint32_t quest_id  = ntohl(pkt->quest_id);
                uint32_t xp_reward       = ntohl(pkt->xp_reward);
                uint32_t currency_reward = ntohl(pkt->currency_reward);
                uint8_t  currency_id     = pkt->currency_id;

                quest_log_complete(&g_current_game->playing->quest_log, quest_id);
				show_quest_reward(xp_reward, currency_reward, currency_id);

                NET_LOG("[NET] Quest complete: id=%u +%u XP +%u %s\n",
                       quest_id, xp_reward, currency_reward,
                       world_currency_name(currency_id));
            }
            break;

        case PACKET_QUEST_ABANDONED:
            if (length >= (int)sizeof(QuestAbandonPacket) && g_current_game && g_current_game->playing) {
                QuestAbandonPacket* pkt = (QuestAbandonPacket*)data;
                uint32_t quest_id = ntohl(pkt->quest_id);

                /* Removed here rather than when the button was clicked: the
                 * server decides whether the quest actually went, and a refused
                 * abandon must leave the log showing it is still there. */
                quest_log_remove(&g_current_game->playing->quest_log, quest_id);
                NET_LOG("[NET] Quest abandoned: id=%u\n", quest_id);
            }
            break;

        case PACKET_ZONE_CHANGE:
            if (length >= (int)sizeof(ZoneChangePacket) && g_current_game && g_current_game->playing) {
                ZoneChangePacket* pkt = (ZoneChangePacket*)data;
                PlayingState* ps = g_current_game->playing;
                strncpy(ps->current_zone_name, pkt->zone_name, sizeof(ps->current_zone_name) - 1);
                ps->current_zone_name[sizeof(ps->current_zone_name) - 1] = '\0';
                strncpy(ps->zone_banner_name, pkt->zone_name, sizeof(ps->zone_banner_name) - 1);
                ps->zone_banner_name[sizeof(ps->zone_banner_name) - 1] = '\0';
                ps->zone_banner_timer = 4.0f;
                NET_LOG("[NET] Zone change: %s (id=%u type=%u)\n",
                       pkt->zone_name, pkt->zone_id, pkt->zone_type);
            }
            break;

        default:
            return 0;   // not ours; the next domain gets a look
    }

    return 1;
}

/**
 * Send the current player position and velocity.
 *
 * @return      Nonzero when the complete packet is sent; otherwise zero.
 */
uint32_t network_next_move_sequence(void) {
    /* Read under the response lock for the same reason the correction is
     * written under it: the two are compared against each other, and the
     * counter is touched from the frame that sends and read by nothing else.
     * Starts at 1, so zero can mean "no move has been sent yet" in the ring
     * the caller keeps. */
    EnterCriticalSection(&g_net.response_lock);
    uint32_t next = ++g_net.move_sequence;
    LeaveCriticalSection(&g_net.response_lock);
    return next;
}

int network_send_player_move(float x, float y, float z, uint8_t flags,
                             float speed, float vel_x, float vel_y,
                             uint32_t sequence, uint8_t heading) {
    if (!g_net.connected) return 0;

    /* `speed` is accepted and not sent. PlayerMovePacket used to carry it and
     * the server always ignored it, using its own record of the player's move
     * speed -- a client cannot be allowed to choose how fast it moves. The
     * field left the wire at PROTOCOL_VERSION 5; the parameter stays so the
     * dozen call sites do not all have to change for a value nobody reads. */
    (void)speed;

    PlayerMovePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PLAYER_MOVE;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(PlayerMovePacket) - sizeof(PacketHeader));
    pkt.pos_x = x;
    pkt.pos_y = y;
    pkt.pos_z = z;
    pkt.move_flags = flags;
    pkt.heading = heading;
    pkt.vel_x = vel_x;
    pkt.vel_y = vel_y;
    pkt.sequence = htonl(sequence);
	EnterCriticalSection(&g_net.response_lock);
	pkt.correction_seq = htonl(g_net.correction_echo);
	LeaveCriticalSection(&g_net.response_lock);

    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

void network_send_emote_request(uint8_t emote, uint8_t arg) {
	if(!g_net.connected)
		return;

	EmoteRequestPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_EMOTE_REQUEST;
	pkt.header.player_id = htonl(g_net.character_id);
	pkt.header.payload_size = htons(sizeof(EmoteRequestPacket) - sizeof(PacketHeader));
	pkt.emote = emote;
	pkt.arg = arg;

	net_send((char*)&pkt, sizeof(pkt));
	NET_LOG("[NET] Emote request sent: emote %u arg %u\n", emote, arg);
}

void network_send_inspect_request(uint32_t target_id) {
	if(!g_net.connected)
		return;

	InspectRequestPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_INSPECT_REQUEST;
	pkt.header.player_id = htonl(g_net.character_id);
	pkt.header.payload_size = htons(sizeof(InspectRequestPacket) - sizeof(PacketHeader));
	pkt.target_id = htonl(target_id);

	net_send((char*)&pkt, sizeof(pkt));
	NET_LOG("[NET] Inspect request sent: target %u\n", target_id);
}

int network_get_inspect_response(InspectResponsePacket* out) {
	EnterCriticalSection(&g_net.response_lock);
	if(!g_net.inspect.ready) {
		LeaveCriticalSection(&g_net.response_lock);
		return 0;
	}
	memcpy(out, &g_net.inspect.data, sizeof(InspectResponsePacket));
	g_net.inspect.ready = FALSE;
	LeaveCriticalSection(&g_net.response_lock);

	return 1;
}

void network_send_status_request(uint8_t status) {
	if(!g_net.connected)
		return;

	StatusRequestPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_STATUS_REQUEST;
	pkt.header.player_id = htonl(g_net.character_id);
	pkt.header.payload_size = htons(sizeof(StatusRequestPacket) - sizeof(PacketHeader));
	pkt.status = status;

	net_send((char*)&pkt, sizeof(pkt));
	NET_LOG("[NET] Status request sent: %u\n", status);
}

int network_get_status_state(uint8_t* status) {
	int ready;

	EnterCriticalSection(&g_net.response_lock);
	ready = g_net.status_event.ready;
	if(ready) {
		*status = g_net.status_event.status;
		g_net.status_event.ready = FALSE;
	}
	LeaveCriticalSection(&g_net.response_lock);

	return ready;
}

int network_get_world_time(uint32_t* time_of_day_ms, uint32_t* day_length_ms) {
	int ready;

	EnterCriticalSection(&g_net.response_lock);
	ready = g_net.world_time_event.ready;
	if(ready) {
		*time_of_day_ms = g_net.world_time_event.time_of_day_ms;
		*day_length_ms = g_net.world_time_event.day_length_ms;
		g_net.world_time_event.ready = FALSE;
	}
	LeaveCriticalSection(&g_net.response_lock);

	return ready;
}

int network_get_weather(WeatherForecast* out) {
	int ready;

	EnterCriticalSection(&g_net.response_lock);
	ready = g_net.weather_event.ready;
	if(ready) {
		*out = g_net.weather_event.forecast;
		g_net.weather_event.ready = FALSE;
	}
	LeaveCriticalSection(&g_net.response_lock);

	return ready;
}

int network_get_emote_state(uint8_t* emote, uint8_t* arg, uint8_t* result,
                            uint8_t* requested) {
	EnterCriticalSection(&g_net.response_lock);
	if(g_net.emote_events.count == 0) {
		LeaveCriticalSection(&g_net.response_lock);
		return 0;
	}
	const uint8_t* e = g_net.emote_events.entries[g_net.emote_events.head];
	*emote = e[0];
	*arg = e[1];
	*result = e[2];
	*requested = e[3];
	g_net.emote_events.head = (g_net.emote_events.head + 1) % 8;
	g_net.emote_events.count--;
	LeaveCriticalSection(&g_net.response_lock);

	return 1;
}

/**
 * Consume the most recent server position correction.
 *
 * @return      Nonzero when a correction is copied; otherwise zero.
 */
int network_get_server_correction(move_correction* out) {
	EnterCriticalSection(&g_net.response_lock);
	if(!g_net.correction.ready) {
		LeaveCriticalSection(&g_net.response_lock);
		return 0;
	}
	*out = g_net.correction.data;
	// taken now, so every move from here on says so
	g_net.correction_echo = out->sequence;
	g_net.correction.ready = FALSE;
	LeaveCriticalSection(&g_net.response_lock);

	return 1;
}

/**
 * Update the last nonzero movement-facing angle.
 */
void network_update_facing_direction(float vel_x, float vel_y) {
    if (vel_x != 0.0f || vel_y != 0.0f) {
        g_net.last_facing_angle = atan2f(vel_y, vel_x);
    }
}

/**
 * Send an interaction request for an NPC.
 */
void network_send_npc_interact_request(uint32_t npc_id) {
    if (!g_net.connected) return;

    NPCInteractRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_NPC_INTERACT_REQUEST;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(NPCInteractRequestPacket) - sizeof(PacketHeader));
    pkt.npc_id = htonl(npc_id);

    if (net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt)) {
        NET_LOG("[NET] NPC interact request sent for NPC %u\n", npc_id);
    }
}

/**
 * Send the chosen option for the current NPC dialogue page.
 *
 * @param option_id  One of the identifiers the page arrived with, never a row
 *                   number: the server filters options per player.
 */
void network_send_dialogue_option_select(uint32_t npc_id, uint32_t dialogue_id, uint8_t current_page, uint8_t option_id) {
    if (!g_net.connected) return;

    DialogueOptionSelectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_DIALOGUE_OPTION_SELECT;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(DialogueOptionSelectPacket) - sizeof(PacketHeader));
    pkt.npc_id = htonl(npc_id);
    pkt.dialogue_id = htonl(dialogue_id);
    pkt.current_page = current_page;
    pkt.option_id = option_id;

    if (net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt)) {
        NET_LOG("[NET] Dialogue option %u chosen (page %u, dialogue %u)\n",
               option_id, current_page, dialogue_id);
    }
}

/**
 * Ask the server to drop a quest from this character's log.
 */
void network_send_quest_abandon(uint32_t quest_id) {
    if (!g_net.connected) return;

    QuestAbandonPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_QUEST_ABANDON;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(QuestAbandonPacket) - sizeof(PacketHeader));
    pkt.quest_id = htonl(quest_id);

    if (net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt))
        NET_LOG("[NET] Quest abandon requested: id=%u\n", quest_id);
}

/**
 * Consume the pending NPC interaction response.
 *
 * @return      Nonzero when a response is copied; otherwise zero.
 */
int network_get_npc_interact_response(NPCInteractResponsePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.npc_interact.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.npc_interact.data, sizeof(NPCInteractResponsePacket));
    g_net.npc_interact.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Consume the pending dialogue-page update.
 *
 * @return      Nonzero when an update is copied; otherwise zero.
 */
int network_get_dialogue_update(DialogueUpdatePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.dialogue_update.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.dialogue_update.data, sizeof(DialogueUpdatePacket));
    g_net.dialogue_update.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Consume the pending dialogue-close notification.
 *
 * @return      Nonzero when a notification is copied; otherwise zero.
 */
int network_get_dialogue_close(DialogueClosePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.dialogue_close.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.dialogue_close.data, sizeof(DialogueClosePacket));
    g_net.dialogue_close.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Send a chat message on a protocol channel.
 *
 * @param target   NUL-terminated whisper recipient, "" on other channels.
 * @param message  NUL-terminated text truncated to MAX_CHAT_MESSAGE minus one bytes.
 */
void network_send_chat(uint8_t channel, const char* target, const char* message) {
    if (!g_net.connected) return;

    ChatSendPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_CHAT_SEND;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(ChatSendPacket) - sizeof(PacketHeader));
    pkt.channel = channel;
    strncpy(pkt.target, target, MAX_CHAT_TARGET - 1);
    strncpy(pkt.message, message, MAX_CHAT_MESSAGE - 1);

    net_send((char*)&pkt, sizeof(pkt));
}

/**
 * Send a party invitation to a character name.
 *
 * @param target_name  NUL-terminated name truncated to 31 bytes on the wire.
 */
void network_send_party_invite(const char* target_name) {
    if (!g_net.connected) return;

    PartyInvitePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_INVITE;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(PartyInvitePacket) - sizeof(PacketHeader));
    strncpy(pkt.target_name, target_name, 31);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Party invite sent to: %s\n", target_name);
}

/**
 * Accept the pending party invitation.
 */
void network_send_party_accept(void) {
    if (!g_net.connected) return;

    PartyAcceptPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_ACCEPT;
    pkt.header.player_id = htonl(g_net.character_id);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Party accept sent\n");
}

/**
 * Decline the pending party invitation.
 */
void network_send_party_decline(void) {
    if (!g_net.connected) return;

    PartyDeclinePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_DECLINE;
    pkt.header.player_id = htonl(g_net.character_id);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Party decline sent\n");
}

/**
 * Request departure from the current party.
 */
void network_send_party_leave(void) {
    if (!g_net.connected) return;

    PartyLeavePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_LEAVE;
    pkt.header.player_id = htonl(g_net.character_id);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Party leave sent\n");
}

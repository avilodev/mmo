// Movement rules: the server accepts or corrects every proposed position.

#include "movement_system.h"
#include "outbox.h"
#include "move_validator.h"
#include "anim_speed.h"
#include "world_collision.h"
#include "log.h"
#include "player_data.h"
#include "player_effects.h"
#include "player_level.h"
#include "item_registry.h"
#include "loot.h"
#include "str_fixed.h"
#include "utils.h"
#include "zone_system.h"
#include "emote_handler.h"
#include "npc_query.h"
#include "friends.h"
#include "party.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arpa/inet.h>

/** Refuse one proposal and say where the player actually is.
 *
 * @param sequence  The PlayerMovePacket being refused, in host order. Echoing
 *                  it is what lets the client rewind to (pos_x, pos_y) and
 *                  replay only the moves it sent after this one, rather than
 *                  assigning the corrected position to wherever it has got to
 *                  by the time this arrives.
 */
static void send_move_correction(int client_fd, uint32_t sequence,
                                 float pos_x, float pos_y,
                                 const vertical_state* vs) {
    PlayerMoveAckPacket correction;
    memset(&correction, 0, sizeof(correction));
    correction.header.type = PACKET_PLAYER_MOVE_ACK;
    correction.header.payload_size = htons(sizeof(PlayerMoveAckPacket) - sizeof(PacketHeader));
    correction.pos_x = pos_x;
    correction.pos_y = pos_y;
	correction.pos_z = vs->z;
	correction.vel_z = vs->vz;
	correction.grounded = vs->grounded ? 1 : 0;
    correction.sequence = htonl(sequence);
    outbox_send(client_fd, &correction, sizeof(correction));
}

/** Validate and apply a client movement proposal. */
void movement_system_propose(int client_fd, uint32_t character_id, int player_slot,
                             const PlayerMovePacket* pkt) {
    ActivePlayer* player = player_acquire_hint(character_id, player_slot);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    float client_x = pkt->pos_x;
    float client_y = pkt->pos_y;
    float client_z = pkt->pos_z;
	// Jumping is off for now: a MOVE_FLAG_JUMP from the client is ignored
	int jump = 0;
    vertical_state vs = { player->pos_z, player->vel_z, player->grounded };
	vertical_state held = vs;

    /* Carried through untouched but for the byte order: the server never acts
     * on it, it only hands it back so the client knows which of its proposals
     * a refusal answers. Nothing here trusts it, and it cannot be wrong in a
     * way that matters -- a client that echoes nonsense corrects itself
     * badly and affects nobody else. */
    uint32_t sequence = ntohl(pkt->sequence);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

	// Sent before the client took our last correction: it only spends time
	if(move_drop_stale(&player->move_budget, ntohl(pkt->correction_seq),
	                   player->pos_x, player->pos_y, player->move_speed, &now, &held)) {
		player->pos_z = held.z;
		player->vel_z = held.vz;
		player->grounded = held.grounded;
		player->is_dirty = 1;
		player_release(player);
		return;
	}

	float vdrift_before = player->move_budget.vdrift;

    // use server-owned movement speed
    MoveVerdict verdict = move_validate(&player->move_budget,
                                        player->pos_x, player->pos_y,
                                        client_x, client_y, client_z, jump,
                                        player->move_speed, &now, &vs);

    /* Fear refuses movement *toward* what caused it. The player keeps control,
     * they simply cannot close the distance -- which is what running scared
     * looks like from the server's side, and needs nothing of the client beyond
     * the correction it already handles.
     *
     * The source is an NPC identifier, so its position comes from the shared
     * query index rather than from a pool scan on a network thread. */
    if (verdict == MOVE_ACCEPT) {
        uint32_t fear_src = player_fear_source(player);
        NpcQueryHit src;
        if (fear_src && npc_query_lookup(fear_src, &src)) {
            float was   = hypotf(player->pos_x - src.pos_x, player->pos_y - src.pos_y);
            float now_d = hypotf(client_x - src.pos_x, client_y - src.pos_y);
			if(now_d < was) {
				verdict = MOVE_REJECT_SPEED;
				player->move_budget.vdrift = vdrift_before;
			}
        }
    }

    if (verdict != MOVE_ACCEPT) {
        LOG_WARN_RL(5, 60,
                    "[MOVE] rejected (%s) character %u: (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)",
                    move_verdict_name(verdict), character_id,
                    player->pos_x, player->pos_y, player->pos_z,
                    client_x, client_y, client_z);

		// The body falls where the server holds it; the client is told how
		float server_x = player->pos_x;
		float server_y = player->pos_y;

		player->anim_speed = 0.0f;
		player->anim_speed_at = get_time_seconds();
		move_hold(&player->move_budget, server_x, server_y, &held);
		player->move_budget.correction_seq = sequence;
		player->move_budget.stale_time = 0.0f;
		player->pos_z = held.z;
		player->vel_z = held.vz;
		player->grounded = held.grounded;
		player->is_dirty = 1;
		player_release(player);
		send_move_correction(client_fd, sequence, server_x, server_y, &held);
		return;
    }

	// A move that goes nowhere only reports the body has stopped
	int went_somewhere = client_x != player->pos_x || client_y != player->pos_y;

	player->anim_speed = anim_speed_reported(pkt->vel_x, pkt->vel_y, player->move_speed);
	player->anim_speed_at = get_time_seconds();
	player->heading = pkt->heading;
	// 0 standing, else 1 + which idle variation
	player->idle_fidget = (pkt->move_flags & MOVE_FLAG_FIDGET) ?
		(uint8_t)(1 + ((pkt->move_flags & MOVE_FIDGET_KIND_MASK) >> MOVE_FIDGET_KIND_SHIFT)) : 0;
    player->pos_x = client_x;
    player->pos_y = client_y;
    player->pos_z = vs.z;
    player->vel_z = vs.vz;
    player->grounded = vs.grounded;
    player->is_dirty = 1;

	// A move ends an emote; reported after release, never under the lock.
	uint8_t ended_emote = went_somewhere ? player->emote : EMOTE_NONE;

	if(ended_emote != EMOTE_NONE) {
		player->emote = EMOTE_NONE;
		player->emote_arg = 0;
	}

    // notify only on zone transitions
    const WorldZone* zone = zone_lookup(client_x, client_y);
    uint8_t new_zone_id = zone ? zone->id : 0;
    if (new_zone_id != player->current_zone_id) {
        player->current_zone_id = new_zone_id;
        player_release(player);

        if (zone) {
            ZoneChangePacket zpkt = {0};
            zpkt.header.type         = PACKET_ZONE_CHANGE;
            zpkt.header.player_id    = htonl(character_id);
            zpkt.header.payload_size = htons(sizeof(ZoneChangePacket) - sizeof(PacketHeader));
            zpkt.zone_id   = zone->id;
            zpkt.zone_type = zone->type;
            STR_COPY_FIELD(zpkt.zone_name, zone->name);
            outbox_send(client_fd, &zpkt, sizeof(zpkt));
        }
    } else {
        player_release(player);
    }

	if(ended_emote != EMOTE_NONE) {
		emote_send_state(client_fd, character_id, EMOTE_NONE, 0,
		                 EMOTE_ENDED_MOVED, ended_emote);
	}
}

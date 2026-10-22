/**
 * @file
 * Implement character listing, creation, deletion, and world-entry transitions.
 */

#include "state_handler.h"
#include "network/net_connect.h"
#include "network/net_reconnect.h"
#include "game.h"
#include "renderer.h"
#include "input/input.h"
#include "network/network.h"
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include "core/client_log.h"
#include "ui/char_select_view.h"

/** Seconds to hold off an automatic character-list retry after an attempt.
 *
 * The realm prices a character list at 5 query tokens against a bucket that
 * holds 20 and refills at 8/s, and it closes a connection that is refused 20
 * times in 10 seconds. One attempt per hold-off cannot reach either limit.
 */
#define CHAR_LIST_RETRY_SECONDS 2.0f

/** Time left before the character list may be asked for again, in seconds. */
static float s_char_list_retry_in = 0.0f;

// The last frame's length, for the picture of the selected character
static float s_frame_dt = 0.0f;

/* --- Retrying a refused world entry --------------------------------------
 *
 * The realm answers a request it could not afford with PACKET_RATE_LIMITED,
 * carrying the milliseconds until it could. Nothing used that number: the
 * request was abandoned, an error was put on screen, and the player was left
 * to click the character again. Entering a world always follows something else
 * -- a character list, and usually a character creation, which costs the
 * realm's whole burst allowance -- so the first click was routinely the one
 * that got refused. Every world entry in the client log was preceded by
 * exactly one refusal, roughly a second before it.
 *
 * Waiting out the delay the server named and sending the same request again is
 * what the field is for. Once only: a second refusal means something other
 * than a bucket that had not refilled, and telling the player is then the
 * right answer.
 */

/** Seconds until the refused world entry is resent; 0 when none is pending. */
static float    s_enter_retry_in = 0.0f;

/** The request to resend, captured when it was first sent. */
static uint32_t s_enter_retry_char_id  = 0;
static uint32_t s_enter_retry_world_id = 0;

/** Nonzero once this screen has already retried; reset on entry. */
static int      s_enter_retried = 0;

/** Margin added to the server's advertised delay, in seconds.
 *
 * The delay names the moment the bucket reaches the cost exactly, and the two
 * clocks are not the same clock. Landing a frame early would spend the retry
 * on another refusal. */
#define ENTER_RETRY_MARGIN_SECONDS 0.15f

// Resends of one refused list, create or delete before the player is told
#define REFUSAL_MAX_RESENDS 4

// Packet type awaiting a resend; 0 when none
static uint8_t  s_resend_type  = 0;
static float    s_resend_in    = 0.0f;
static int      s_resend_count = 0;

// What a create or delete asked for, kept so it can be resent and matched
static uint32_t s_op_world_id = 0;
static uint32_t s_op_char_id  = 0;
static uint32_t s_op_race_id  = 0;
static uint32_t s_op_appearance = 0;
static char     s_op_name[32] = {0};

// Race list retry delay and time spent waiting on the current request
static float    s_race_retry_in = 0.0f;
static float    s_race_wait     = 0.0f;

/** The identifier of the world the player picked, or 0 when none is valid.
 *
 * Every use of server_list.selected_index goes through this. Two of the three
 * read it straight out of the worlds array with no check at all, while the
 * third tested it for >= 0 -- so the file already knew the index could be -1
 * and disagreed with itself about whether that mattered. It is also the one
 * place a world_id is decided now: character creation used to take it from the
 * selection while deletion took it from the character list's echo of the same
 * request, which is two sources of truth for one value.
 */
static uint32_t char_select_world_id(const GameState* game) {
    int index = game->server_list.selected_index;
    if (index < 0 || index >= MAX_WORLDS) return 0;
    if (index >= game->server_list.list.count) return 0;
    return ntohl(game->server_list.list.worlds[index].world_id);
}

static void char_select_enter(GameState* game) {
    CLOG_INFO("[STATE] Entering character select");
	csv_open();
    game->char_select.loaded = 0;
    game->char_select.selected_index = -1;
    game->char_select.show_creation = 0;
    /* Race and class fuse into one identifier, so there is one choice to make. It
     * stays 0 until the race list arrives, because the client does not know which
     * races exist until the server says. */
    game->char_select.selected_race = 0;
	game->char_select.selected_body = APPEARANCE_BODY_MALE;
    game->char_select.races_loaded = 0;
    game->char_select.races_requested = 0;
    game->char_select.pending_delete = 0;
    game->char_select.pending_delete_index = -1;
    memset(game->char_select.new_name, 0, sizeof(game->char_select.new_name));
    memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));
    game->net_state = NET_STATE_IDLE;
    s_char_list_retry_in = 0.0f;   /* the first attempt is immediate */

    /* A world entry left half-retried belongs to the visit that started it.
     * Carrying one into a fresh visit would fire a request for a character on
     * a world the player may no longer have selected. */
    s_enter_retry_in       = 0.0f;
    s_enter_retry_char_id  = 0;
    s_enter_retry_world_id = 0;
    s_enter_retried        = 0;

	s_resend_type   = 0;
	s_resend_in     = 0.0f;
	s_resend_count  = 0;
	s_race_retry_in = 0.0f;
	s_race_wait     = 0.0f;
}

static void char_select_exit(GameState* game) {
    (void)game;
    CLOG_INFO("[STATE] Exiting character select");
	csv_close();
}

/** The wait state a refused packet type is answered in, or IDLE if none. */
static NetworkState state_for_refused_type(uint8_t type) {
	switch(type) {
		case PACKET_CHARACTER_LIST_REQUEST:
			return NET_STATE_WAITING_FOR_CHARACTERS;
		case PACKET_CHARACTER_CREATE_REQUEST:
			return NET_STATE_CREATING_CHARACTER;
		case PACKET_CHARACTER_DELETE_REQUEST:
			return NET_STATE_DELETING_CHARACTER;
		default:
			return NET_STATE_IDLE;
	}
}

/**
 * Route a realm refusal to the request it names.
 *
 * A refused list, create or delete stays on its loading state and is resent
 * after the delay the realm gave. Only when the resends run out is the player
 * told. The notice is read every frame, not only while a request is waiting,
 * so a refused race list cannot be mistaken for the character list's.
 */
static void handle_rate_limit_notice(GameState* game) {
	uint8_t  rejected_type = 0;
	uint16_t retry_ms      = 0;

	if(!network_get_rate_limit_notice(&rejected_type, &retry_ms))
		return;

	float delay = retry_ms / 1000.0f + ENTER_RETRY_MARGIN_SECONDS;

	if(rejected_type == PACKET_RACE_LIST_REQUEST) {
		game->char_select.races_requested = 0;
		s_race_retry_in = delay;
		return;
	}

	if(rejected_type == PACKET_ENTER_WORLD) {
		// Resent once after the named delay instead of asking for a click
		if(game->net_state != NET_STATE_WAITING_FOR_ENTER_WORLD || s_enter_retried)
			return;

		s_enter_retry_in       = delay;
		s_enter_retry_char_id  = game->pending_character_id;
		s_enter_retry_world_id = game->pending_world_id;
		s_enter_retried        = 1;
		game->net_state        = NET_STATE_IDLE;
		game->net_wait_seconds = 0.0f;

		CLOG_INFO("[CHAR_SELECT] World entry refused for %ums; resending once",
		          (unsigned)retry_ms);
		return;
	}

	NetworkState waiting_in = state_for_refused_type(rejected_type);

	// Not a request this screen is waiting on; any real loss times out
	if(waiting_in == NET_STATE_IDLE || game->net_state != waiting_in)
		return;

	if(s_resend_count >= REFUSAL_MAX_RESENDS) {
		s_resend_type          = 0;
		game->net_state        = NET_STATE_IDLE;
		game->net_wait_seconds = 0.0f;
		snprintf(game->char_select.error_message,
		         sizeof(game->char_select.error_message),
		         "Too many requests - try again in %.1fs", retry_ms / 1000.0f);
		return;
	}

	s_resend_type          = rejected_type;
	s_resend_in            = delay;
	s_resend_count++;
	game->net_wait_seconds = 0.0f;

	CLOG_INFO("[CHAR_SELECT] Request %u refused for %ums; resending (%d)",
	          (unsigned)rejected_type, (unsigned)retry_ms, s_resend_count);
}

/** Send a refused request again once its delay has passed. */
static void resend_refused_request(GameState* game, float delta_time) {
	if(s_resend_type == 0)
		return;

	// A response or a timeout already settled it
	if(game->net_state != state_for_refused_type(s_resend_type)) {
		s_resend_type = 0;
		return;
	}

	s_resend_in -= delta_time;
	if(s_resend_in > 0.0f)
		return;

	uint32_t world_id = char_select_world_id(game);
	int      sent     = 0;

	switch(s_resend_type) {
		case PACKET_CHARACTER_LIST_REQUEST:
			sent = world_id != 0 && network_request_character_list(world_id);
			break;
		case PACKET_CHARACTER_CREATE_REQUEST:
			sent = network_create_character(s_op_world_id, s_op_name,
			                                s_op_race_id, s_op_race_id, s_op_appearance);
			break;
		case PACKET_CHARACTER_DELETE_REQUEST:
			sent = network_delete_character(s_op_world_id, s_op_char_id);
			break;
		default:
			break;
	}

	s_resend_type = 0;

	if(sent) {
		game->net_wait_seconds = 0.0f;
		return;
	}

	game->net_state = NET_STATE_IDLE;
	snprintf(game->char_select.error_message,
	         sizeof(game->char_select.error_message),
	         "Could not reach the server - please try again");
}

/**
 * Release a pending character request after rejection or timeout.
 *
 * @param delta_time  Elapsed frame time in seconds.
 * @param error_out  Destination for a user-facing failure message.
 * @param error_size  Capacity of error_out in bytes.
 */
static void resolve_stalled_request(GameState* game, float delta_time,
                                    char* error_out, size_t error_size) {
    if (game->net_state == NET_STATE_IDLE) return;

    /* The world handshake keeps its own deadlines -- five seconds for the
     * connect, five more for the acknowledgement -- and reports
     * NET_CONNECT_FAILED when either expires. Timing it here as well raced
     * those at exactly their sum: at ten seconds this abandoned the request
     * and returned to the list, while leaving the attempt in flight with a
     * live socket and no owner. The handshake reports its own failures. */
    if (game->net_state == NET_STATE_CONNECTING_TO_WORLD) return;

    game->net_wait_seconds += delta_time;

    if (game->net_wait_seconds > NET_REQUEST_TIMEOUT_SECONDS) {
        s_resend_type          = 0;
        game->net_state        = NET_STATE_IDLE;
        game->net_wait_seconds = 0.0f;
        snprintf(error_out, error_size, "Server did not respond - please try again");
    }
}

/**
 * Drive deferred character operations and their network response state machine.
 *
 * @param delta_time  Elapsed frame time in seconds.
 */
static void char_select_update(GameState* game, float delta_time) {
	s_frame_dt = delta_time;

    if (s_char_list_retry_in > 0.0f) s_char_list_retry_in -= delta_time;

	handle_rate_limit_notice(game);

    resolve_stalled_request(game, delta_time,
                            game->char_select.error_message,
                            sizeof(game->char_select.error_message));

	resend_refused_request(game, delta_time);

    /* Resend a world entry the realm refused, once its stated delay is up.
     *
     * Only from idle: the player may have gone somewhere else in the meantime
     * -- into the creation panel, or back to the world list -- and a request
     * fired into the middle of that would answer a screen nobody is on. */
    if (s_enter_retry_in > 0.0f) {
        s_enter_retry_in -= delta_time;

        if (s_enter_retry_in <= 0.0f) {
            s_enter_retry_in = 0.0f;

            if (game->net_state == NET_STATE_IDLE && s_enter_retry_world_id != 0 &&
                network_request_enter_world(s_enter_retry_char_id,
                                            s_enter_retry_world_id)) {
                game->pending_character_id = s_enter_retry_char_id;
                game->pending_world_id     = s_enter_retry_world_id;
                game->net_state            = NET_STATE_WAITING_FOR_ENTER_WORLD;
                game->net_wait_seconds     = 0.0f;
                CLOG_INFO("[CHAR_SELECT] Resent the world entry for character %u",
                          s_enter_retry_char_id);
            } else {
                /* Could not be sent, or the player moved on. Say so rather
                 * than leaving the screen claiming it is still entering. */
                snprintf(game->char_select.error_message,
                         sizeof(game->char_select.error_message),
                         "Could not enter the world - please try again");
            }

            s_enter_retry_char_id  = 0;
            s_enter_retry_world_id = 0;
        }
    }

    // Handle deferred character creation
    if (game->char_select.pending_create) {
        game->char_select.pending_create = 0;

        if (strlen(game->char_select.new_name) < 3) {
            snprintf(game->char_select.error_message, sizeof(game->char_select.error_message), "Name must be at least 3 characters");
        } else if (game->char_select.selected_race == 0) {
            snprintf(game->char_select.error_message, sizeof(game->char_select.error_message),
                     "Choose a race");
        } else if (char_select_world_id(game) == 0) {
            snprintf(game->char_select.error_message,
                     sizeof(game->char_select.error_message),
                     "No world selected");
        } else if (game->net_state == NET_STATE_IDLE) {
            /* Both wire fields carry the same fused identifier; the realm server
             * rejects a mismatch, and rejects a race that is not playable. */
            uint32_t appearance = appearance_pack(game->char_select.selected_body);

            if (network_create_character(char_select_world_id(game),
                                         game->char_select.new_name,
                                         game->char_select.selected_race,
                                         game->char_select.selected_race, appearance)) {
                game->net_state = NET_STATE_CREATING_CHARACTER;
                game->net_wait_seconds = 0.0f;
                memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));

				s_op_world_id  = char_select_world_id(game);
				s_op_race_id   = game->char_select.selected_race;
				s_op_appearance = appearance;
				s_resend_count = 0;
				snprintf(s_op_name, sizeof(s_op_name), "%s", game->char_select.new_name);
            } else {
				snprintf(game->char_select.error_message,
				         sizeof(game->char_select.error_message),
				         "Could not reach the server - please try again");
			}
        }
    }

    // Handle deferred character deletion
    if (game->char_select.pending_delete) {
        game->char_select.pending_delete = 0;
        int idx = game->char_select.pending_delete_index;
        uint32_t world_id = char_select_world_id(game);
        if (world_id != 0 && idx >= 0 && idx < game->char_select.list.count &&
            game->net_state == NET_STATE_IDLE) {
            uint32_t char_id  = ntohl(game->char_select.list.characters[idx].character_id);
            if (network_delete_character(world_id, char_id)) {
                game->net_state = NET_STATE_DELETING_CHARACTER;
                game->net_wait_seconds = 0.0f;
                memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));

				s_op_world_id  = world_id;
				s_op_char_id   = char_id;
				s_resend_count = 0;
            } else {
				snprintf(game->char_select.error_message,
				         sizeof(game->char_select.error_message),
				         "Could not reach the server - please try again");
			}
        }
        game->char_select.pending_delete_index = -1;
    }

    /* The race list is fetched outside the state machine: it is small, needed
     * only by the creation panel, and must not block the character list. It is
     * asked for again when refused or unanswered, so a lost request cannot
     * leave the panel on "Loading races..." for the rest of the visit. */
	if(!game->char_select.races_loaded) {
		if(s_race_retry_in > 0.0f)
			s_race_retry_in -= delta_time;

		if(!game->char_select.races_requested) {
			if(s_race_retry_in <= 0.0f) {
				game->char_select.races_requested = network_request_race_list();
				s_race_wait = 0.0f;
			}
		} else if(network_get_race_list(&game->char_select.races)) {
            game->char_select.races_loaded = 1;
            CLOG_INFO("[CHAR_SELECT] Received %d races", game->char_select.races.count);

            /* Preselect the first playable race so the panel opens on a valid choice. */
            for (int i = 0; i < game->char_select.races.count; i++) {
                if (!game->char_select.races.races[i].playable) continue;
                game->char_select.selected_race = ntohl(game->char_select.races.races[i].race_id);
                break;
            }
		} else {
			s_race_wait += delta_time;
			if(s_race_wait > NET_REQUEST_TIMEOUT_SECONDS)
				game->char_select.races_requested = 0;
		}
	}

    // State machine
    switch (game->net_state) {
        case NET_STATE_IDLE: {
            /* Held off between attempts, not retried on the very next frame.
             *
             * resolve_stalled_request() returns to this state on a refusal or
             * a timeout, and the only condition guarding the request is
             * `!loaded` -- which a refused request never changes. So a realm
             * that answered PACKET_RATE_LIMITED got the same request back one
             * frame later, sixty times a second, until it had refused twenty
             * of them and closed the connection for abuse. The client then
             * reconnected, walked back to this screen, and did it again. It is
             * the same defect state_server_list.c was fixed for; the hold-off
             * here is that fix, in the shape this screen needs -- the list is
             * fetched once, not on an interval.
             *
             * Armed on both outcomes, so a request that could not even be sent
             * waits too. */
            uint32_t world_id = char_select_world_id(game);
            if (!game->char_select.loaded && world_id != 0 &&
                s_char_list_retry_in <= 0.0f) {
                if (network_request_character_list(world_id)) {
                    game->net_state = NET_STATE_WAITING_FOR_CHARACTERS;
                    game->net_wait_seconds = 0.0f;
					s_resend_count = 0;
                }
                s_char_list_retry_in = CHAR_LIST_RETRY_SECONDS;
            }
            break;
        }

        case NET_STATE_WAITING_FOR_CHARACTERS: {
			CharacterListResponsePacket incoming_list;

			if(network_get_character_list(&incoming_list)) {
				// Only the selected world's list answers this screen
				if(ntohl(incoming_list.world_id) != char_select_world_id(game)) {
					CLOG_WARN("[CHAR_SELECT] Dropped a list for world %u",
					          (unsigned)ntohl(incoming_list.world_id));
					break;
				}

				game->char_select.list = incoming_list;
                game->char_select.loaded = 1;
                game->net_state = NET_STATE_IDLE;
                CLOG_INFO("[CHAR_SELECT] Received %d characters", game->char_select.list.count);
            }
            break;
		}

        case NET_STATE_CREATING_CHARACTER:
            {
                CharacterCreateResponsePacket response;
                if (network_get_character_create_response(&response)) {
                    if (response.success) {
                        game->char_select.show_creation = 0;
                        memset(game->char_select.new_name, 0, sizeof(game->char_select.new_name));

                        // The list is about to change, and the old highlight
                        // refers to a position in the OLD list. Drop it rather
                        // than let it point at a different character.
                        game->char_select.selected_index = -1;

                        // The server pushes a refreshed list immediately after
                        // this response, so wait for THAT instead of asking for
                        // another one. Going through NET_STATE_IDLE would have
                        // issued a second request whose first act is to discard
                        // the pushed list -- a wasted round-trip, and a window in
                        // which the screen showed the pre-create list. Waiting
                        // here consumes the push, usually already buffered from
                        // the same read, so the new character appears in the
                        // same frame the response is handled.
                        game->char_select.loaded = 0;
                        game->net_state = NET_STATE_WAITING_FOR_CHARACTERS;
                        game->net_wait_seconds = 0.0f;
                    } else {
                        game->net_state = NET_STATE_IDLE;
                        strncpy(game->char_select.error_message, response.message, 127);
                        game->char_select.error_message[127] = '\0';
                    }
                }
            }
            break;

        case NET_STATE_DELETING_CHARACTER:
            {
                CharacterDeleteResponsePacket response;
                if (network_get_character_delete_response(&response)) {
					// A stale answer for another character is not this one's
					if(ntohl(response.character_id) != s_op_char_id ||
					   ntohl(response.world_id) != s_op_world_id) {
						CLOG_WARN("[CHAR_SELECT] Dropped a delete answer for %u",
						          (unsigned)ntohl(response.character_id));
						break;
					}

                    if (response.success) {
                        // Same as create: the server pushes the refreshed list,
                        // so consume that rather than issuing a request that
                        // would only discard it.
                        game->char_select.loaded = 0;
                        game->char_select.selected_index = -1;
                        game->net_state = NET_STATE_WAITING_FOR_CHARACTERS;
                        game->net_wait_seconds = 0.0f;
                    } else {
                        game->net_state = NET_STATE_IDLE;
                        strncpy(game->char_select.error_message, response.message, 127);
                        game->char_select.error_message[127] = '\0';
                    }
                }
            }
            break;

        case NET_STATE_WAITING_FOR_ENTER_WORLD:
            {
                EnterWorldResponsePacket response;
                if (network_get_enter_world_response(&response)) {
                    game->net_state = NET_STATE_IDLE;

                    if (response.success) {
                        /* Copied at the field's own width. It is a hostname or
                         * an address literal now and the client resolves it;
                         * the fixed 16 here truncated anything longer than a
                         * dotted quad into an address that does not exist. */
                        char world_host[sizeof(response.world_host)];
                        snprintf(world_host, sizeof(world_host), "%s",
                                 response.world_host);
                        uint16_t world_port = ntohs(response.world_port);

                        /* The character this ticket was minted for, recorded
                         * when the request went out. */
                        uint32_t char_id = game->pending_character_id;

                        /* Started, not waited on. The frame below advances it.
                         * The blocking form froze the window for the connect
                         * plus up to five seconds of acknowledgement polling. */
                        if (network_begin_world_connect(world_host, world_port,
                                                        response.game_ticket, char_id)) {
                            game->net_state = NET_STATE_CONNECTING_TO_WORLD;
                            game->net_wait_seconds = 0.0f;
                        } else {
                            CLOG_WARN("[CHAR_SELECT] Could not start the world connection: %s",
                                   network_connect_message());
                        }
                    } else {
                        CLOG_WARN("[CHAR_SELECT] Enter world denied: %s", response.message);
                    }
                }
            }
            break;

        case NET_STATE_CONNECTING_TO_WORLD:
            switch (network_connect_poll()) {
                case NET_CONNECT_SUCCEEDED: {
                    game->net_state          = NET_STATE_IDLE;
                    game->network_connected  = 1;
                    game->player.info_loaded = 0;
                    game_change_state(game, GAME_MODE_PLAYING);

                    network_set_character_id(game->pending_character_id);
                    network_request_character_data(game->pending_character_id,
                                                   game->pending_world_id);

                    /* Everything a recovery needs, recorded at the one moment
                     * all of it is known together. Without this a dropped
                     * world connection ended the session. */
                    net_reconnect_remember_world(game->realm_ip,
                                                 (uint16_t)game->realm_port,
                                                 game->account_id,
                                                 game->session_key,
                                                 game->pending_world_id,
                                                 game->pending_character_id);
                    break;
                }
                case NET_CONNECT_FAILED:
                    game->net_state = NET_STATE_IDLE;
                    game->network_connected = 0;
                    strncpy(game->char_select.error_message, network_connect_message(), 127);
                    game->char_select.error_message[127] = '\0';
                    CLOG_WARN("[CHAR_SELECT] World connection failed: %s",
                           network_connect_message());
                    break;
                case NET_CONNECT_IDLE:
                    /* No attempt in flight while this state is waiting on one.
                     *
                     * This state has no timeout of its own -- the handshake
                     * keeps those, which is why resolve_stalled_request() steps
                     * over it -- so an attempt that vanished without reporting
                     * would leave the screen on "Entering world..." with
                     * nothing left to advance it. That is precisely what
                     * happened while the frame loop was polling world
                     * handshakes and consuming their one-shot result. It cannot
                     * now, and if it ever does again this says so instead of
                     * hanging. */
                    game->net_state = NET_STATE_IDLE;
                    game->network_connected = 0;
                    snprintf(game->char_select.error_message,
                             sizeof(game->char_select.error_message),
                             "The world connection was lost - please try again");
                    CLOG_WARN("[CHAR_SELECT] World connect attempt went idle unpolled");
                    break;

                case NET_CONNECT_PENDING:
                    break;
            }
            break;

        default:
            break;
    }
}

/**
 * Render and operate the character-creation overlay.
 */
/** Height of one race row, and how many rows the creation panel shows. */
#define RACE_ROW_H        44.0f
#define RACE_ROWS_VISIBLE 10

/** Map a CombatRole to its display name. */
static const char* role_display_name(uint8_t role) {
    switch (role) {
        case ROLE_TANK:   return "Tank";
        case ROLE_DPS:    return "DPS";
        case ROLE_HEALER: return "Healer";
        default:          return "Unknown";
    }
}

// The body toggle, in a column beside the race list
static void char_select_render_look(GameState* game, float x, float y) {
	static const char* const BODY_NAMES[APPEARANCE_BODY_COUNT] = { "Male", "Female" };

	renderer_draw_text(x, y + 140, "Body:");

	for(int body = 0; body < APPEARANCE_BODY_COUNT; body++) {
		float bx = x + body * 120.0f;
		int selected = game->char_select.selected_body == body;
		int hovered = input_mouse_in_rect(&game->input, bx, y + 150, 110, 36);
		float c = selected ? 0.40f : (hovered ? 0.30f : 0.20f);

		renderer_draw_rect(bx, y + 150, 110, 36, c, c, c + 0.08f, 1.0f);
		renderer_draw_text(bx + 10, y + 174, BODY_NAMES[body]);

		if(hovered && game->input.mouse_left_clicked)
			game->char_select.selected_body = (uint8_t)body;
	}
}

static void char_select_render_creation(GameState* game) {
    int vw = game->camera.viewport_width;
    int vh = game->camera.viewport_height;

    float panel_w = 860;
    float panel_h = 660;
    float panel_x = (vw - panel_w) / 2;
    float panel_y = (vh - panel_h) / 2;

    renderer_draw_rect(panel_x, panel_y, panel_w, panel_h, 0.15f, 0.15f, 0.2f, 0.95f);
    renderer_draw_rect(panel_x, panel_y, panel_w, 60, 0.2f, 0.3f, 0.4f, 1.0f);
    renderer_draw_text(panel_x + 20, panel_y + 40, "CREATE CHARACTER");

    // Name input
    renderer_draw_rect(panel_x + 50, panel_y + 80, panel_w - 100, 40, 0.1f, 0.1f, 0.15f, 1.0f);
    renderer_draw_text(panel_x + 60, panel_y + 105, "Name:");
    renderer_draw_text(panel_x + 130, panel_y + 105, game->char_select.new_name);

    /* One list, not two: race and class are the same choice under the Blessed model.
     * Everything drawn here comes from the server's race registry, so a new race
     * reaches the player without this file changing at all. */
    renderer_draw_text(panel_x + 50, panel_y + 140, "Race:");

    if (!game->char_select.races_loaded) {
        renderer_draw_text(panel_x + 50, panel_y + 170, "Loading races...");
    } else {
        const RaceListResponsePacket* races = &game->char_select.races;

        for (int i = 0; i < races->count && i < RACE_ROWS_VISIBLE; i++) {
            const RaceInfo* race = &races->races[i];
            uint32_t race_id = ntohl(race->race_id);

            float y = panel_y + 160 + i * RACE_ROW_H;
            int selected = (game->char_select.selected_race == race_id);

            /* A race with no designed kit is shown but cannot be picked. Greying it
             * out is the whole of that rule: nothing here special-cases a name. */
            int selectable = race->playable;
            int hovered = selectable &&
                          input_mouse_in_rect(&game->input, panel_x + 50, y, 460, RACE_ROW_H - 4);

            float c = selected ? 0.40f : (hovered ? 0.30f : 0.20f);
            if (!selectable) c = 0.13f;
            renderer_draw_rect(panel_x + 50, y, 460, RACE_ROW_H - 4, c, c, c + 0.08f, 1.0f);

            char label[160];
            if (selectable) {
                snprintf(label, sizeof(label), "%s (%s) - %s",
                         race->name, race->latin, role_display_name(race->default_role));
            } else {
                snprintf(label, sizeof(label), "%s (%s) - coming soon",
                         race->name, race->latin);
            }
            renderer_draw_text(panel_x + 60, y + 17, label);

            if (race->passive_name[0]) {
                /* Sized from the two fields it concatenates rather than
                 * guessed at: passive_name is 32 and passive_desc is 192 (see
                 * RaceInfo in protocol.h), plus the two-space indent, the
                 * ": " and a terminator. At 200 this was a truncation the
                 * compiler could prove and nobody could see, and it would have
                 * cut the end off the longest passive descriptions the server
                 * is allowed to send. */
                char passive[sizeof(race->passive_name) + sizeof(race->passive_desc) + 8];
                snprintf(passive, sizeof(passive), "  %s: %s",
                         race->passive_name, race->passive_desc);
                renderer_draw_text(panel_x + 60, y + 33, passive);
            }

            if (hovered && game->input.mouse_left_clicked) {
                game->char_select.selected_race = race_id;
            }
        }
    }

	char_select_render_look(game, panel_x + 560, panel_y);

    // Create button
    float btn_y = panel_y + panel_h - 100;
    int create_hovered = input_mouse_in_rect(&game->input, panel_x + 150, btn_y, 200, 45);
    int creating = (game->net_state == NET_STATE_CREATING_CHARACTER);

    renderer_draw_rect(panel_x + 150, btn_y, 200, 45,
                      creating ? 0.15f : (create_hovered ? 0.3f : 0.2f),
                      creating ? 0.4f : (create_hovered ? 0.7f : 0.5f),
                      0.2f, 1.0f);
    renderer_draw_text(panel_x + 220, btn_y + 30, "CREATE");

    if (create_hovered && game->input.mouse_left_clicked && !creating) {
        game->char_select.pending_create = 1;
    }

    // Error message
    if (game->char_select.error_message[0] != '\0') {
        renderer_draw_rect(panel_x + 50, panel_y + panel_h - 50, panel_w - 100, 30,
                          0.8f, 0.2f, 0.2f, 1.0f);
        renderer_draw_text(panel_x + 60, panel_y + panel_h - 30, game->char_select.error_message);
    }

    // Cancel button
    int cancel_hovered = input_mouse_in_rect(&game->input, panel_x + 50, btn_y, 80, 45);
    renderer_draw_rect(panel_x + 50, btn_y, 80, 45,
                      cancel_hovered ? 0.7f : 0.5f, 0.2f, 0.2f, 1.0f);
    renderer_draw_text(panel_x + 55, btn_y + 30, "CANCEL");

    if (cancel_hovered && game->input.mouse_left_clicked) {
        game->char_select.show_creation = 0;
        memset(game->char_select.new_name, 0, sizeof(game->char_select.new_name));
        memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));
    }
}

/** Ask the realm to enter the world as the character at an index in the list. */
static void start_enter_world(GameState* game, int index) {
	uint32_t char_id;
	uint32_t world_id = char_select_world_id(game);

	if(index < 0 || index >= game->char_select.list.count)
		return;

	char_id = ntohl(game->char_select.list.characters[index].character_id);

	/* Recorded with the request, not re-derived from the selection
	 * when the answer comes back.
	 *
	 * The ticket the realm mints names one character, and the world
	 * ignores the identifier in the connect packet and trusts the
	 * ticket -- so if the list moved under selected_index between
	 * the request and the response, the player entered the world as
	 * one character while the client spent the session addressing
	 * another, silently. The request is the only moment both halves
	 * are known to agree. */
	if(world_id != 0 && network_request_enter_world(char_id, world_id)) {
		game->pending_character_id = char_id;
		game->pending_world_id     = world_id;
		game->net_state = NET_STATE_WAITING_FOR_ENTER_WORLD;
		game->net_wait_seconds = 0.0f;
	}
}

/** A request is in flight. A pending retry counts as busy: without it the
 *  screen invites the very click this whole path exists to make unnecessary,
 *  and the two requests would race for one ticket. */
static int char_select_busy(const GameState* game) {
	return (game->net_state != NET_STATE_IDLE) || (s_enter_retry_in > 0.0f);
}

/**
 * Render the character select scene, or the creation panel over its backdrop.
 */
static void char_select_render(GameState* game) {
	int vw = game->camera.viewport_width;
	int vh = game->camera.viewport_height;
	const char* status = NULL;

	// The picture of the character is drawn first: it uses its own target
	if(!game->char_select.show_creation)
		csv_render_preview(game, s_frame_dt);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, vw, vh, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// Character creation overlay
	if(game->char_select.show_creation) {
		if(game->textures.background != 0)
			renderer_draw_sprite(0, 0, vw, vh, game->textures.background);
		else
			renderer_draw_rect(0, 0, vw, vh, 0.1f, 0.1f, 0.2f, 1.0f);

		char_select_render_creation(game);
		return;
	}

	// Shown across the retry too: a blank gap there invites a second click
	if(game->net_state == NET_STATE_WAITING_FOR_ENTER_WORLD || s_enter_retry_in > 0.0f)
		status = "Entering world...";
	else if(game->net_state == NET_STATE_DELETING_CHARACTER)
		status = "Deleting...";

	csv_draw(game, status);
}

/**
 * Handle character-select escape and creation-name keyboard input.
 */
static void char_select_input(GameState* game, GLFWwindow* window, float delta_time) {
    (void)delta_time;

    /* One press, one screen.
     *
     * This tested the key's level rather than its edge, and a keypress spans
     * many frames at any frame rate: the frame that closed the creation panel
     * was followed by another with the key still down, which left character
     * select as well. Escape always went back two screens. */
    if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
        if (game->char_select.show_creation) {
            game->char_select.show_creation = 0;
        } else if(!csv_escape()) {
            game_change_state(game, GAME_MODE_SERVER_LIST);
        }
    }

	if(!game->char_select.show_creation) {
		switch(csv_update(game, char_select_busy(game))) {
		case CSV_JOIN:
			start_enter_world(game, game->char_select.selected_index);
			break;
		case CSV_DELETE:
			game->char_select.pending_delete = 1;
			game->char_select.pending_delete_index = game->char_select.selected_index;
			break;
		case CSV_NEW:
			game->char_select.show_creation = 1;
			break;
		case CSV_BACK:
			game_change_state(game, GAME_MODE_SERVER_LIST);
			break;
		default:
			break;
		}
	}

    // Handle text input for character creation
    if (game->char_select.show_creation) {
        int shift_held = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                         glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        for (int key = GLFW_KEY_A; key <= GLFW_KEY_Z; key++) {
            if (input_key_just_pressed(&game->input, key)) {
                size_t len = strlen(game->char_select.new_name);
                if (len < 31) {
                    int capitalize = shift_held || (len == 0);
                    char c = capitalize ? ('A' + (key - GLFW_KEY_A)) : ('a' + (key - GLFW_KEY_A));
                    game->char_select.new_name[len] = c;
                }
            }
        }

        if (input_key_just_pressed(&game->input, GLFW_KEY_BACKSPACE)) {
            size_t len = strlen(game->char_select.new_name);
            if (len > 0) {
                game->char_select.new_name[len - 1] = '\0';
            }
        }
    }
}

/** State-handler table for GAME_MODE_CHARACTER_SELECT. */
const StateHandler g_state_character_select = {
    .enter = char_select_enter,
    .exit = char_select_exit,
    .update = char_select_update,
    .render = char_select_render,
    .handle_input = char_select_input
};

/**
 * @file
 * Define token-bucket packet budgets for world and realm traffic.
 */

#include "limit_profiles.h"
#include "protocol.h"

/** Budget world traffic by high-rate movement and lower-rate stateful operations. */
static const PacketLimitProfile WORLD_PROFILE = {
    .name = "world",

    .overall = { .rate = 200.0, .capacity = 400.0 },

    .classes = {
        [LIMIT_CLASS_MOVEMENT] = { .rate = 150.0, .capacity = 300.0 },
        [LIMIT_CLASS_COMBAT]   = { .rate =  30.0, .capacity =  60.0 },
        [LIMIT_CLASS_ITEM]     = { .rate =  20.0, .capacity =  40.0 },
        [LIMIT_CLASS_SOCIAL]   = { .rate =   5.0, .capacity =  15.0 },
        [LIMIT_CLASS_QUERY]    = { .rate =  20.0, .capacity =  40.0 },
    },

    // Movement and combat supersede themselves — the next packet replaces the
    // dropped one, so silence is correct. The rest can leave UI waiting.
    .notify_on_drop = {
        [LIMIT_CLASS_MOVEMENT] = 0,
        [LIMIT_CLASS_COMBAT]   = 0,
        [LIMIT_CLASS_ITEM]     = 1,
        [LIMIT_CLASS_SOCIAL]   = 1,
        [LIMIT_CLASS_QUERY]    = 1,
    },

    .opcodes = {
        // Movement — highest volume, cheapest to serve.
        [PACKET_PLAYER_MOVE]           = { LIMIT_CLASS_MOVEMENT, 1 },
        [PACKET_PING]                  = { LIMIT_CLASS_MOVEMENT, 1 },

        // Combat.
        [PACKET_ATTACK_INTENT]         = { LIMIT_CLASS_COMBAT,   1 },
        [PACKET_CAST_CANCEL]           = { LIMIT_CLASS_COMBAT,   1 },
        [PACKET_ABILITY_CAST_INTENT]   = { LIMIT_CLASS_COMBAT,   2 },
        [PACKET_ABILITY_CAST_CANCEL]   = { LIMIT_CLASS_COMBAT,   1 },

        // Inventory — each one mutates persistent state.
        [PACKET_EQUIP_ITEM]            = { LIMIT_CLASS_ITEM,     2 },
        [PACKET_UNEQUIP_ITEM]          = { LIMIT_CLASS_ITEM,     2 },
        [PACKET_USE_ITEM]              = { LIMIT_CLASS_ITEM,     2 },
        [PACKET_DROP_ITEM]             = { LIMIT_CLASS_ITEM,     2 },
        [PACKET_MOVE_ITEM]             = { LIMIT_CLASS_ITEM,     2 },
        [PACKET_LOOT_PICKUP_REQUEST]   = { LIMIT_CLASS_ITEM,     2 },

        // Social — chat costs more because one packet fans out to many.
        [PACKET_CHAT_SEND]             = { LIMIT_CLASS_SOCIAL,   3 },
        [PACKET_PARTY_INVITE]          = { LIMIT_CLASS_SOCIAL,   2 },
        [PACKET_PARTY_ACCEPT]          = { LIMIT_CLASS_SOCIAL,   2 },
        [PACKET_PARTY_DECLINE]         = { LIMIT_CLASS_SOCIAL,   2 },
        [PACKET_PARTY_LEAVE]           = { LIMIT_CLASS_SOCIAL,   2 },
        [PACKET_PARTY_KICK]            = { LIMIT_CLASS_SOCIAL,   2 },

        // Queries — priced by what they actually do.
        [PACKET_LOGOUT]                = { LIMIT_CLASS_QUERY,    1 },
        [PACKET_REQUEST_PLAYER_DATA]   = { LIMIT_CLASS_QUERY,    2 },
        [PACKET_REQUEST_PLAYER_STATS]  = { LIMIT_CLASS_QUERY,    2 },
        [PACKET_NPC_INTERACT_REQUEST]  = { LIMIT_CLASS_QUERY,    2 },
        [PACKET_DIALOGUE_OPTION_SELECT]= { LIMIT_CLASS_QUERY,    2 },
        [PACKET_DIALOGUE_CLOSE]        = { LIMIT_CLASS_QUERY,    1 },
        [PACKET_SHOP_BUY]              = { LIMIT_CLASS_QUERY,    5 },
        [PACKET_SHOP_SELL]             = { LIMIT_CLASS_QUERY,    5 },
        [PACKET_SESSION_LIST_REQUEST]  = { LIMIT_CLASS_QUERY,    5 },
    },

    .violation_limit  = 200,
    .violation_window = 10.0,
};

/** Budget realm traffic for infrequent database-backed character operations. */
static const PacketLimitProfile REALM_PROFILE = {
    .name = "realm",

    .overall = { .rate = 10.0, .capacity = 25.0 },

    .classes = {
        [LIMIT_CLASS_MOVEMENT] = { .rate =  5.0, .capacity = 10.0 },  // ping only
        [LIMIT_CLASS_COMBAT]   = { .rate =  1.0, .capacity =  2.0 },  // unused here
        [LIMIT_CLASS_ITEM]     = { .rate =  1.0, .capacity =  2.0 },  // unused here
        [LIMIT_CLASS_SOCIAL]   = { .rate =  1.0, .capacity =  2.0 },  // unused here
        [LIMIT_CLASS_QUERY]    = { .rate =  8.0, .capacity = 20.0 },
    },

    .notify_on_drop = {
        [LIMIT_CLASS_MOVEMENT] = 0,
        [LIMIT_CLASS_COMBAT]   = 0,
        [LIMIT_CLASS_ITEM]     = 1,
        [LIMIT_CLASS_SOCIAL]   = 1,
        [LIMIT_CLASS_QUERY]    = 1,
    },

    .opcodes = {
        [PACKET_PING]                     = { LIMIT_CLASS_MOVEMENT,  1 },

        [PACKET_WORLD_LIST_REQUEST]       = { LIMIT_CLASS_QUERY,     3 },
        [PACKET_CHARACTER_LIST_REQUEST]   = { LIMIT_CLASS_QUERY,     5 },
        [PACKET_ENTER_WORLD]              = { LIMIT_CLASS_QUERY,    10 },

        // Character creation and deletion are the most expensive things this
        // server does, and the ones most worth making tedious to repeat.
        [PACKET_CHARACTER_CREATE_REQUEST] = { LIMIT_CLASS_QUERY,    20 },
        [PACKET_CHARACTER_DELETE_REQUEST] = { LIMIT_CLASS_QUERY,    20 },
    },

    .violation_limit  = 20,
    .violation_window = 10.0,
};

/** Return the immutable world-server packet-limit profile. */
const PacketLimitProfile* limit_profile_world(void) { return &WORLD_PROFILE; }
/** Return the immutable realm-server packet-limit profile. */
const PacketLimitProfile* limit_profile_realm(void) { return &REALM_PROFILE; }

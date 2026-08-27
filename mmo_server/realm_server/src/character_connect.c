/**
 * @file
 * Handle realm character listing, creation, and deletion requests.
 */
#include "types.h"
#include "log.h"
#include "players_database.h"
#include "character_connect.h"
#include "session.h"
#include "world_database_manager.h"
#include "race_registry.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stddef.h>
#include "tls.h"

extern PGconn* g_pg;

#define MAX_CHARACTERS_PER_WORLD 10

/**
 * Send the loaded race registry to a client.
 *
 * Non-playable races are included on purpose: the creation screen greys them out, so
 * a player can see what is coming and the client never needs its own list.
 */
void handle_race_list_request(int client_fd, uint32_t account_id) {
    RaceListResponsePacket response = {0};
    response.header.type = PACKET_RACE_LIST_RESPONSE;
    response.header.player_id = htonl(account_id);

    int count = race_registry_count();
    if (count > MAX_RACE_LIST) count = MAX_RACE_LIST;
    response.count = (uint8_t)count;

    for (int i = 0; i < count; i++) {
        const RaceDef* race = race_at(i);
        RaceInfo* out = &response.races[i];

        out->race_id  = htonl(race->id);
        snprintf(out->key,          sizeof(out->key),          "%s", race->key);
        snprintf(out->name,         sizeof(out->name),         "%s", race->name);
        snprintf(out->latin,        sizeof(out->latin),        "%s", race->latin);
        snprintf(out->passive_name, sizeof(out->passive_name), "%s", race->passive.name);
        snprintf(out->passive_desc, sizeof(out->passive_desc), "%s", race->passive.description);

        out->playable = race->playable;

        const RaceSpec* def = race_default_spec(race);
        out->default_role = (uint8_t)(def ? def->role : ROLE_DPS);

        int specs = race->spec_count;
        if (specs > (int)(sizeof(out->spec_roles) / sizeof(out->spec_roles[0]))) {
            specs = (int)(sizeof(out->spec_roles) / sizeof(out->spec_roles[0]));
        }
        out->spec_count = (uint8_t)specs;
        for (int s = 0; s < specs; s++) {
            const RaceSpec* spec = race_spec_at(race, s);
            out->spec_roles[s]  = (uint8_t)spec->role;
            out->spec_unlock[s] = spec->unlock_level;
        }
    }

    /* Send only the races actually present rather than the whole fixed array. */
    size_t send_size = offsetof(RaceListResponsePacket, races) +
                       (size_t)count * sizeof(RaceInfo);
    response.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));

    tls_send(client_fd, &response, send_size, 0);
    LOG_INFO("Sent %d races to account %u", count, account_id);
}

/**
 * Query and send an account's characters for one world.
 *
 * Response identifiers and numeric character fields are encoded in network byte order.
 */
void handle_character_list_request(int client_fd, uint32_t account_id, uint32_t world_id) {
    CharacterInfo characters[MAX_CHARACTERS_PER_WORLD];

    int count = world_character_get_list(account_id, world_id,
                                              characters, MAX_CHARACTERS_PER_WORLD);

    /* A database that could not be reached is not an account with no
     * characters, and answering with an empty list says it is -- the player
     * reads that as "my characters are gone" and the obvious next move is to
     * create a replacement. Say nothing instead: the client's own request
     * timeout reports "Server did not respond", which is what happened.
     *
     * The clamp below is not reachable from world_character_get_list(), which
     * bounds its own writes; it is here because `count` becomes a uint8_t and
     * send_size is computed from it, so a negative or oversized value would
     * be a read past the end of a stack packet rather than a wrong number. */
    if (count < 0) {
        LOG_ERROR("Character list for account %u on world %u is unavailable; "
                  "sending nothing rather than an empty list", account_id, world_id);
        return;
    }
    if (count > MAX_CHARACTERS_PER_WORLD) count = MAX_CHARACTERS_PER_WORLD;

    // Build response packet
    CharacterListResponsePacket response = {0};
    response.header.type = PACKET_CHARACTER_LIST_RESPONSE;
    response.header.player_id = htonl(account_id);
    response.world_id = htonl(world_id);
    response.count = count;

    for (int i = 0; i < count; i++) {
        response.characters[i].character_id = htonl(characters[i].character_id);
        strncpy(response.characters[i].name, characters[i].name, 31);
        response.characters[i].name[31] = '\0';
        response.characters[i].level = htonl(characters[i].level);
        /* Race and class fuse into one identifier, so both wire fields carry it. */
        response.characters[i].class_id = htonl(characters[i].race_id);
        response.characters[i].race_id = htonl(characters[i].race_id);
    }

    size_t send_size = offsetof(CharacterListResponsePacket, characters) +
                       (size_t)response.count * sizeof(response.characters[0]);
    response.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
    tls_send(client_fd, &response, send_size, 0);
    LOG_INFO("Sent %d characters to account %u for world %u", count, account_id, world_id);
}

/**
 * Validate and create a character, then send the result and refreshed list.
 *
 * The supplied name must contain 3 through 31 ASCII letters.
 */
void handle_character_create_request(int client_fd, uint32_t account_id,
                                      uint32_t world_id, const char* name,
                                      int class_id, int race_id) {
    CharacterCreateResponsePacket response = {0};
    response.header.type = PACKET_CHARACTER_CREATE_RESPONSE;
    response.header.player_id = htonl(account_id);
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    response.world_id = htonl(world_id);

    // Check character limit per world
    int current_count = world_character_count(account_id, world_id);
    if (current_count < 0) {
        response.success = 0;
        strncpy(response.message, "Character database unavailable", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }
    if (current_count >= MAX_CHARACTERS_PER_WORLD) {
        response.success = 0;
        strncpy(response.message, "Character limit reached for this world", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }

    // Validate name (basic validation)
    size_t name_len = strlen(name);
    if (name_len < 3 || name_len > 31) {
        response.success = 0;
        strncpy(response.message, "Name must be 3-31 characters", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }

    // Check for valid characters in name (letters only)
    for (size_t i = 0; i < name_len; i++) {
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= 'A' && name[i] <= 'Z'))) {
            response.success = 0;
            strncpy(response.message, "Name must contain only letters", 127);
            tls_send(client_fd, &response, sizeof(response), 0);
            return;
        }
    }

    /* Validate the race before touching the database. These values come straight
     * from the client and were previously passed into SQL unchecked, which let a
     * crafted packet create a character of a race that does not exist or of one
     * that exists but has no designed kit. */
    if (class_id != race_id) {
        response.success = 0;
        strncpy(response.message, "Race and class identifiers must match", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }
    if (!race_is_playable((uint32_t)class_id)) {
        const RaceDef* race = race_get((uint32_t)class_id);
        response.success = 0;
        strncpy(response.message,
                race ? "That race is not yet playable" : "Unknown race", 127);
        LOG_ERROR("Refused character create for account %u: race %d is %s",
                 account_id, class_id, race ? "not playable" : "unknown");
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }

    // Create character
    uint32_t character_id;
    int success = world_character_create(account_id, world_id, name,
                                            class_id, race_id, &character_id);

    if (success) {
        response.success = 1;
        response.character_id = htonl(character_id);
        strncpy(response.character_name, name, 31);
        strncpy(response.message, "Character created successfully", 127);
        LOG_INFO("Created character '%s' (ID:%u) for account %u in world %u",
                 name, character_id, account_id, world_id);
    } else {
        response.success = 0;
        strncpy(response.message, "Character creation failed (name may be taken)", 127);
    }

    tls_send(client_fd, &response, sizeof(response), 0);

    // If successful, send updated character list
    if (success) {
        handle_character_list_request(client_fd, account_id, world_id);
    }
}

/** Delete an owned character and send the result and refreshed list. */
void handle_character_delete_request(int client_fd, uint32_t account_id,
                                      uint32_t character_id, uint32_t world_id) {
    CharacterDeleteResponsePacket response = {0};
    response.header.type = PACKET_CHARACTER_DELETE_RESPONSE;
    response.header.player_id = htonl(account_id);
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    response.character_id = htonl(character_id);
    response.world_id = htonl(world_id);

    /* Refuse while the character is live in a world.
     *
     * Deleting one mid-session did not stop the session: the world went on
     * playing a character whose row was gone, its next save UPDATEd zero rows
     * and reported success, and its item writes re-INSERTed rows for a
     * character that no longer existed. The world records a mark with a TTL
     * while a character is in play and renews it, so a world that stops
     * running stops blocking deletion on its own.
     *
     * The check is not a lock -- a character could enter a world in the
     * moment after it passes -- but the world's own writes fail closed now
     * (a save that matches no row is reported as a failure), so the window
     * costs a session rather than a corrupted database. */
    uint32_t live_in = world_session_world(character_id);
    if (live_in != 0) {
        response.success = 0;
        snprintf(response.message, sizeof(response.message),
                 "Character is currently in a world. Log out and try again.");
        LOG_ERROR("Refused deletion of character %u for account %u: live in world %u",
                 character_id, account_id, live_in);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }

    /* character_items and character_currencies are removed with the character
     * by ON DELETE CASCADE (see character_database_init), so this is the whole
     * deletion rather than the part of it that used to leave orphans owning
     * item-instance identifiers forever. */
    int success = world_character_delete(account_id, character_id, world_id);

    if (success) {
        response.success = 1;
        strncpy(response.message, "Character deleted", 127);
        LOG_INFO("Deleted character %u for account %u from world %u",
                 character_id, account_id, world_id);
    } else {
        response.success = 0;
        strncpy(response.message, "Character deletion failed", 127);
    }

    tls_send(client_fd, &response, sizeof(response), 0);

    // If successful, send updated character list
    if (success) {
        handle_character_list_request(client_fd, account_id, world_id);
    }
}

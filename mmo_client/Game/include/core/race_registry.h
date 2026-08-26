#ifndef CLIENT_RACE_REGISTRY_H
#define CLIENT_RACE_REGISTRY_H

/** @file The race list the server sent, kept for whoever needs it later.
 *
 * The client already receives a complete race registry -- id, key, display
 * name, Latin name, passive, specs -- and then threw it away as soon as the
 * character-creation screen had finished drawing with it. Everything after
 * that fell back to hardcoded tables indexed by a small integer:
 *
 *   * state_playing.c coloured nearby players from a switch over classes 1-4.
 *   * character_screen.c held an eleven-entry colour table by race id.
 *   * hud.c singled out "class 2" for a yellow resource bar.
 *
 * Each of those is the client keeping its own copy of a fact the server is
 * already sending, and each one goes silently wrong the moment races.json
 * gains a race, loses one, or reorders. The registry is meant to make the
 * client know nothing about which races exist; these tables put that knowledge
 * back in.
 *
 * So the list is kept. It is filled once, from the RACE_LIST_RESPONSE the
 * character-creation screen was going to consume anyway, and read from
 * anywhere afterwards.
 *
 * Thread safety: filled on the network thread, read on the render thread.
 * Entries are written before the count is published and never mutated
 * afterwards, so a reader sees either no registry or a complete one.
 */

#include "protocol.h"

#include <stdint.h>

/** Store the race list, replacing anything held. Safe with a NULL packet. */
void client_races_store(const RaceListResponsePacket* packet);

/** Races currently known; zero before the server has answered. */
int client_races_count(void);

/** Look up a race by its fused race/class identifier.
 *
 * @return The entry, or NULL when the registry is empty or has no such race.
 */
const RaceInfo* client_race_find(uint32_t race_id);

/**
 * The colour that represents a race, everywhere the client draws one.
 *
 * Derived from the race's stable `key` rather than looked up in a table, so a
 * race added to races.json gets a distinct, consistent colour with no client
 * change at all -- which is the whole point of the registry being sent.
 *
 * The derivation is a hash of the key spread around the hue circle at fixed
 * saturation and value, so every colour it can produce is legible against the
 * world and distinguishable from its neighbours. Stable for a given key across
 * runs and machines.
 *
 * An unknown race, or a registry that has not arrived yet, gets a neutral grey
 * rather than nothing: this is presentation, and a missing colour must never
 * be a reason a player is not drawn.
 */
void client_race_color(uint32_t race_id, float* r, float* g, float* b);

/**
 * Whether a race's starting spec uses a non-mana resource.
 *
 * Replaces `if (player_class == 2)` in the party frames. The role comes from
 * the registry, so a second energy-using race needs no client edit.
 */
int client_race_uses_energy(uint32_t race_id);

#endif // CLIENT_RACE_REGISTRY_H

#ifndef CHARACTER_LOOK_H
#define CHARACTER_LOOK_H

/** @file Which meshes of the character kit one body wears.
 *
 * The character model is one file holding every body and every race's parts on
 * one skeleton. A mesh's name says what it is:
 *
 *   Body_M, Body_F     the two bodies; one is worn
 *   Ears_<key>         that race's ears (<key> is the races.json key)
 *   Tail_<key>         that race's tail
 *   Horns_<key>        that race's horns
 *   Muzzle_<key>       that race's muzzle or beak
 *   Claws_<key>        that race's claws; worn only with CHARACTER_LOOK_CLAWS
 *
 * Any other name (the single "Mannequin" mesh of today's file, a prop) is
 * always worn, so a model that has not been split into a kit draws as it
 * always has. The client never lists races: the key comes from the registry.
 *
 * No GL and no registry, so it can be tested on a hand-built model.
 */

#include "model/skinned_model.h"

#include <stdint.h>

/** Wear the claws as well; the angry state, which nothing sets yet. */
#define CHARACTER_LOOK_CLAWS 0x1u

/**
 * The meshes a body with this look wears.
 *
 * @param race_key    The race's stable key, or NULL for an NPC or an unknown
 *                    race, which wears a body and no race parts.
 * @param appearance  The packed word from protocol.h; only the body is read here.
 * @param flags       CHARACTER_LOOK_* bits.
 *
 * A body is never left without one: asking for a body the model does not have
 * falls back to the first body it does have.
 */
SkinMeshMask character_look_mask(const SkinnedModel* model, const char* race_key,
                                 uint32_t appearance, unsigned flags);

/**
 * A mask with the animal features taken off: the race parts of the kit scheme
 * (ears, tail, horns, muzzle, claws) and any mesh whose material is an animal
 * accessory of a model that is not split into a kit (the player models' cat ears
 * and fox tail). Everything else, hair included, is left as it was.
 */
SkinMeshMask character_look_hide_animal(const SkinnedModel* model, SkinMeshMask mask);

#endif // CHARACTER_LOOK_H

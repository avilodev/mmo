/**
 * @file
 * Choose the meshes of the character kit a body wears.
 */

#include "render/character_look.h"

#include "protocol.h"

#include <string.h>

// The mesh-name prefix of each race part, and whether it needs the claws flag
typedef struct {
	const char* prefix;
	unsigned required_flag;
} part_slot;

static const part_slot PART_SLOTS[] = {
	{ "Ears_", 0 },
	{ "Tail_", 0 },
	{ "Horns_", 0 },
	{ "Muzzle_", 0 },
	{ "Claws_", CHARACTER_LOOK_CLAWS },
};

#define PART_SLOT_COUNT ((int)(sizeof(PART_SLOTS) / sizeof(PART_SLOTS[0])))

// Parts of a material name that mark an animal accessory on a model with no kit
// meshes: the player models carry them as Accessory_CatEar_* and Accessory_FoxTail_*
static const char* const ANIMAL_MATERIALS[] = { "CatEar", "FoxTail" };

#define ANIMAL_MATERIAL_COUNT ((int)(sizeof(ANIMAL_MATERIALS) / sizeof(ANIMAL_MATERIALS[0])))

// 1 when `name` starts with `prefix`, with the rest in `rest`
static int has_prefix(const char* name, const char* prefix, const char** rest) {
	size_t length = strlen(prefix);

	if(strncmp(name, prefix, length) != 0)
		return 0;

	*rest = name + length;
	return 1;
}

// The body a mesh named Body_x stands for, or -1
static int body_of(const char* suffix) {
	if(strcmp(suffix, "M") == 0)
		return APPEARANCE_BODY_MALE;
	if(strcmp(suffix, "F") == 0)
		return APPEARANCE_BODY_FEMALE;
	return -1;
}

SkinMeshMask character_look_mask(const SkinnedModel* model, const char* race_key,
                                 uint32_t appearance, unsigned flags) {
	SkinMeshMask mask = 0;
	SkinMeshMask bodies = 0;
	int first_body = -1;
	int wanted = (int)appearance_body(appearance);

	if(!model)
		return 0;

	for(int m = 0; m < model->mesh_count; m++) {
		const char* name = model->meshes[m].name;
		const char* rest = NULL;
		SkinMeshMask bit = (SkinMeshMask)1 << m;

		if(has_prefix(name, "Body_", &rest)) {
			bodies |= bit;
			if(first_body < 0)
				first_body = m;
			if(body_of(rest) == wanted)
				mask |= bit;
			continue;
		}

		int slot = -1;

		for(int s = 0; s < PART_SLOT_COUNT && slot < 0; s++)
			if(has_prefix(name, PART_SLOTS[s].prefix, &rest))
				slot = s;

		if(slot < 0) {
			// Not part of the kit scheme: always worn
			mask |= bit;
			continue;
		}

		if(race_key && race_key[0] != '\0' && strcmp(rest, race_key) == 0 &&
		   (flags & PART_SLOTS[slot].required_flag) == PART_SLOTS[slot].required_flag)
			mask |= bit;
	}

	// Asked for a body this model lacks: wear the first one rather than none
	if(bodies != 0 && (mask & bodies) == 0 && first_body >= 0)
		mask |= bodies & ((SkinMeshMask)1 << first_body);

	return mask;
}

// 1 when the mesh is an animal feature, by its name in the kit scheme or its material
static int is_animal_mesh(const SkinnedMesh* mesh) {
	const char* rest = NULL;

	for(int s = 0; s < PART_SLOT_COUNT; s++)
		if(has_prefix(mesh->name, PART_SLOTS[s].prefix, &rest))
			return 1;

	for(int a = 0; a < ANIMAL_MATERIAL_COUNT; a++)
		if(strstr(mesh->material, ANIMAL_MATERIALS[a]))
			return 1;

	return 0;
}

SkinMeshMask character_look_hide_animal(const SkinnedModel* model, SkinMeshMask mask) {
	if(!model)
		return mask;

	for(int m = 0; m < model->mesh_count; m++)
		if(is_animal_mesh(&model->meshes[m]))
			mask &= ~((SkinMeshMask)1 << m);

	return mask;
}

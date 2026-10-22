/**
 * @file
 * Check which meshes of the character kit a body wears, on a hand-built model.
 *
 * The rule is read from mesh names only, so no file or GL is needed: a model is
 * a list of names.
 */

#include "render/character_look.h"

#include "protocol.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
	do {                                                                    \
		if(cond) {                                                          \
			printf("  ok   %s\n", (what));                                  \
		} else {                                                            \
			printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
			g_failures++;                                                   \
		}                                                                   \
	} while(0)

// The meshes of a small kit; each body is two primitives, like the mannequin
static const char* const KIT[] = {
	"Body_M", "Body_M", "Body_F", "Body_F", "Ears_wolf", "Tail_wolf",
	"Ears_cat", "Tail_cat", "Claws_wolf", "Prop",
};

#define BIT(i) ((SkinMeshMask)1 << (i))

static void set_names(SkinnedModel* model, const char* const* names, int count) {
	memset(model, 0, sizeof(*model));
	model->mesh_count = count;
	for(int i = 0; i < count; i++)
		snprintf(model->meshes[i].name, sizeof(model->meshes[i].name), "%s", names[i]);
}

int main(void) {
	static SkinnedModel kit;

	set_names(&kit, KIT, (int)(sizeof(KIT) / sizeof(KIT[0])));

	printf("TEST 1: a male wolf wears the male body, wolf parts and the prop\n");
	CHECK(character_look_mask(&kit, "wolf", appearance_pack(APPEARANCE_BODY_MALE), 0) ==
	      (BIT(0) | BIT(1) | BIT(4) | BIT(5) | BIT(9)), "exactly those meshes");

	printf("\nTEST 2: the female body swaps the body and nothing else\n");
	CHECK(character_look_mask(&kit, "wolf", appearance_pack(APPEARANCE_BODY_FEMALE), 0) ==
	      (BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(9)), "female body, same parts");

	printf("\nTEST 3: the race decides the parts\n");
	CHECK(character_look_mask(&kit, "cat", 0, 0) == (BIT(0) | BIT(1) | BIT(6) | BIT(7) | BIT(9)),
	      "a cat wears cat ears and tail");
	CHECK(character_look_mask(&kit, "otter", 0, 0) == (BIT(0) | BIT(1) | BIT(9)),
	      "a race with no parts in the kit wears a body and none");
	CHECK(character_look_mask(&kit, NULL, 0, 0) == (BIT(0) | BIT(1) | BIT(9)),
	      "an NPC (no race) wears a body and none");
	CHECK(character_look_mask(&kit, "", 0, 0) == (BIT(0) | BIT(1) | BIT(9)),
	      "an empty key is the same");
	CHECK(character_look_mask(&kit, "wol", 0, 0) == (BIT(0) | BIT(1) | BIT(9)),
	      "a key matches whole, not by prefix");

	printf("\nTEST 4: claws are worn only when asked for\n");
	CHECK(!(character_look_mask(&kit, "wolf", 0, 0) & BIT(8)), "not by default");
	CHECK(character_look_mask(&kit, "wolf", 0, CHARACTER_LOOK_CLAWS) & BIT(8), "with the flag");
	CHECK(character_look_mask(&kit, "cat", 0, CHARACTER_LOOK_CLAWS) ==
	      character_look_mask(&kit, "cat", 0, 0), "a race with no claws mesh gains none");

	printf("\nTEST 5: a body is never missing\n");
	{
		static SkinnedModel male_only;
		const char* const names[] = { "Body_M", "Ears_wolf" };

		set_names(&male_only, names, 2);
		CHECK(character_look_mask(&male_only, "wolf", appearance_pack(APPEARANCE_BODY_FEMALE), 0) ==
		      (BIT(0) | BIT(1)), "a female asked of a male-only model wears its body");
	}

	printf("\nTEST 6: a model that is not a kit draws everything, as it always did\n");
	{
		static SkinnedModel legacy;
		const char* const names[] = { "Mannequin", "Mannequin" };

		set_names(&legacy, names, 2);
		CHECK(character_look_mask(&legacy, "wolf", appearance_pack(APPEARANCE_BODY_FEMALE), 0) ==
		      (BIT(0) | BIT(1)), "both meshes, whatever the look");
		CHECK(character_look_mask(&legacy, NULL, 0, 0) == (BIT(0) | BIT(1)), "and for an NPC");
	}

	printf("\nTEST 7: names that only look like the scheme are left alone\n");
	{
		static SkinnedModel odd;
		const char* const names[] = { "Body_M", "Earsplus_wolf", "Body", "Tail" };

		set_names(&odd, names, 4);
		CHECK(character_look_mask(&odd, "cat", 0, 0) == (BIT(0) | BIT(1) | BIT(2) | BIT(3)),
		      "no underscore, no slot: always worn");
	}

	printf("\nTEST 8: no model\n");
	CHECK(character_look_mask(NULL, "wolf", 0, 0) == 0, "wears nothing");

	printf("\nTEST 9: the animal features come off, the hair stays\n");
	{
		static SkinnedModel player;
		const char* const names[] = { "Body (merged)", "Body (merged)", "Body (merged)",
		                              "Body (merged)", "Hair001 (merged)" };
		const char* const materials[] = { "N00_000_00_Body_00_SKIN", "N00_000_00_HairBack_00_HAIR",
		                                  "Accessory_CatEar_01_CLOTH", "Accessory_FoxTail_01_CLOTH",
		                                  "N00_000_Hair_00_HAIR" };
		SkinMeshMask all = BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4);

		set_names(&player, names, 5);
		for(int i = 0; i < 5; i++)
			snprintf(player.meshes[i].material, sizeof(player.meshes[i].material), "%s",
			         materials[i]);

		CHECK(character_look_hide_animal(&player, all) == (BIT(0) | BIT(1) | BIT(4)),
		      "ears and tail go by material, body and both hairs stay");
		CHECK(character_look_hide_animal(&player, BIT(0) | BIT(2)) == BIT(0),
		      "only what was worn is considered");
	}
	{
		SkinMeshMask worn = character_look_mask(&kit, "wolf", 0, CHARACTER_LOOK_CLAWS);

		CHECK(character_look_hide_animal(&kit, worn) == (BIT(0) | BIT(1) | BIT(9)),
		      "a kit loses its ears, tail and claws, keeps the body and the prop");
	}
	CHECK(character_look_hide_animal(NULL, 5) == 5, "no model leaves the mask alone");

	printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
	return g_failures ? 1 : 0;
}

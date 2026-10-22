/**
 * @file
 * Check the shipped character model loads, poses and skins, without OpenGL.
 *
 * The model is data the client reads at startup, and every way it can go
 * wrong -- a missing clip, a rig that no longer fits the height the renderer
 * scales it to, skinning that throws a vertex across the map -- shows up only
 * as a character that is invisible, frozen or exploded. Pinning those here
 * catches them before anybody has to log in to see it.
 *
 * Run from mmo_client/, like the game, so the asset path is the game's.
 */

#include "model/skinned_model.h"
#include "protocol.h"
#include "render/character_look.h"
#include "render/character_tuning.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "utils/stb_image.h"

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/** Largest distance any skinned vertex sits from the model's origin. */
static float max_vertex_reach(const SkinnedInstance* inst) {
    float reach = 0.0f;
    for (int m = 0; m < inst->mesh_count; m++) {
        const SkinnedMeshVertices* v = &inst->meshes[m];
        for (int i = 0; i < v->vertex_count; i++) {
            const float* p = v->vertices + (size_t)i * SKIN_VERTEX_FLOATS;
            if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return INFINITY;
            float d = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (d > reach) reach = d;
        }
    }
    return reach;
}

/** Whether two instances differ anywhere by more than `eps`. */
static int instances_differ(const SkinnedInstance* a, const SkinnedInstance* b, float eps) {
    for (int m = 0; m < a->mesh_count; m++) {
        for (int i = 0; i < a->meshes[m].vertex_count * SKIN_VERTEX_FLOATS; i++) {
            if (fabsf(a->meshes[m].vertices[i] - b->meshes[m].vertices[i]) > eps) return 1;
        }
    }
    return 0;
}

// Write a one-joint skinned glb with two meshes: "Body_M", and "Ears_wolf.001"
// (a Blender copy name) that stands 5 units tall against the body's 1.
static int write_kit_glb(const char* path) {
	static const char json[] =
		"{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,1,2]}],"
		"\"nodes\":[{\"name\":\"root\"},{\"name\":\"Body_M\",\"mesh\":0,\"skin\":0},"
		"{\"name\":\"Ears_wolf.001\",\"mesh\":1,\"skin\":0}],"
		"\"skins\":[{\"joints\":[0],\"inverseBindMatrices\":5}],"
		"\"meshes\":["
		"{\"name\":\"m0\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
		"\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":4,\"material\":0}]},"
		"{\"name\":\"m1\",\"primitives\":[{\"attributes\":{\"POSITION\":6,\"NORMAL\":1,"
		"\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":4,\"material\":1}]}],"
		"\"materials\":[{\"name\":\"M_Main.001\"},{\"name\":\"M_Claw_wolf\","
		"\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.1,0.05,1]}}],"
		"\"accessors\":["
		"{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
		"{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
		"{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
		"{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
		"{\"bufferView\":4,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
		"{\"bufferView\":5,\"componentType\":5126,\"count\":1,\"type\":\"MAT4\"},"
		"{\"bufferView\":6,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
		"\"bufferViews\":["
		"{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
		"{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
		"{\"buffer\":0,\"byteOffset\":72,\"byteLength\":12},"
		"{\"buffer\":0,\"byteOffset\":84,\"byteLength\":48},"
		"{\"buffer\":0,\"byteOffset\":132,\"byteLength\":6},"
		"{\"buffer\":0,\"byteOffset\":140,\"byteLength\":64},"
		"{\"buffer\":0,\"byteOffset\":204,\"byteLength\":36}],"
		"\"buffers\":[{\"byteLength\":240}]}";

	unsigned char bin[240];
	float* f = (float*)bin;
	unsigned short* idx = (unsigned short*)(bin + 132);
	static const float body[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
	static const float ears[9] = { 0, 0, 0, 1, 0, 0, 0, 5, 0 };

	memset(bin, 0, sizeof(bin));
	memcpy(f, body, sizeof(body));
	for(int v = 0; v < 3; v++)
		f[9 + v * 3 + 2] = 1.0f;
	for(int v = 0; v < 3; v++)
		f[21 + v * 4] = 1.0f;
	idx[0] = 0;
	idx[1] = 1;
	idx[2] = 2;
	f[35] = f[35 + 5] = f[35 + 10] = f[35 + 15] = 1.0f;
	memcpy(bin + 204, ears, sizeof(ears));

	unsigned int json_length = (unsigned int)(sizeof(json) - 1);
	unsigned int json_padded = (json_length + 3u) & ~3u;
	unsigned int total = 12 + 8 + json_padded + 8 + (unsigned int)sizeof(bin);
	unsigned int header[3] = { 0x46546C67u, 2u, total };
	unsigned int json_head[2] = { json_padded, 0x4E4F534Au };
	unsigned int bin_head[2] = { (unsigned int)sizeof(bin), 0x004E4942u };

	FILE* out = fopen(path, "wb");

	if(!out)
		return 0;

	fwrite(header, sizeof(header), 1, out);
	fwrite(json_head, sizeof(json_head), 1, out);
	fwrite(json, json_length, 1, out);
	for(unsigned int i = json_length; i < json_padded; i++)
		fputc(' ', out);
	fwrite(bin_head, sizeof(bin_head), 1, out);
	fwrite(bin, sizeof(bin), 1, out);
	fclose(out);
	return 1;
}

int main(void) {
    printf("=== character model ===\n");

    static SkinnedModel model;
    char error[256] = "";

    printf("\nTEST 1: a missing file fails with a reason, not a crash\n");
    {
        static SkinnedModel missing;
        int ok = skinned_model_load_glb(&missing, "Game/assets/models/nope.glb",
                                        error, (int)sizeof(error));
        CHECK(!ok, "load of a missing file fails");
        CHECK(error[0] != '\0', "and says why");
        CHECK(missing.mesh_count == 0 && missing.clip_count == 0, "and leaves the model empty");
    }

    printf("\nTEST 2: the shipped mannequin loads\n");
    int loaded = skinned_model_load_glb(&model, CHARACTER_MODEL_PATH, error, (int)sizeof(error));
    if (!loaded) printf("  (%s)\n", error);
    CHECK(loaded, CHARACTER_MODEL_PATH " loads");
    if (!loaded) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    CHECK(model.mesh_count >= 1, "has at least one mesh");
    CHECK(model.skeleton.joint_count > 0 && model.skeleton.joint_count <= SKIN_MAX_JOINTS,
          "has a skeleton that fits");
    CHECK(model.max_y - model.min_y > 1.5f && model.max_y - model.min_y < 2.2f,
          "stands about 1.8 model units tall (the renderer scales from this)");

    printf("\nTEST 3: every clip the characters play is in the file\n");
    {
        const char* needed[] = {
            CHARACTER_CLIP_IDLE, CHARACTER_CLIP_WALK, CHARACTER_CLIP_JOG, CHARACTER_CLIP_RUN,
            CHARACTER_CLIP_TALK, CHARACTER_CLIP_DEATH,
			CHARACTER_CLIP_SIT_ENTER, CHARACTER_CLIP_SIT_IDLE, CHARACTER_CLIP_SIT_EXIT,
			CHARACTER_CLIP_DANCE, CHARACTER_CLIP_FIDGET,
			CHARACTER_CLIP_PIVOT_LEFT, CHARACTER_CLIP_PIVOT_RIGHT,
			CHARACTER_CLIP_LEAN_ENTER, CHARACTER_CLIP_LEAN_IDLE, CHARACTER_CLIP_LEAN_EXIT,
			CHARACTER_CLIP_LEAN_FOOT_ENTER, CHARACTER_CLIP_LEAN_FOOT_IDLE, CHARACTER_CLIP_LEAN_FOOT_EXIT,
			CHARACTER_CLIP_FIDGET_GLANCE, CHARACTER_CLIP_FIDGET_KNUCKLES, CHARACTER_CLIP_FIDGET_SLEEVE,
        };
        for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
            int clip = skinned_model_find_clip(&model, needed[i]);
            char what[96];
            snprintf(what, sizeof(what), "%s is present and has length", needed[i]);
            CHECK(clip >= 0 && model.clips[clip].duration > 0.0f, what);
        }
        CHECK(skinned_model_find_clip(&model, "No_Such_Clip") == -1, "an unknown clip is -1");
    }

    printf("\nTEST 4: skinning moves the body and keeps it in one piece\n");
    {
        SkinnedInstance bind, walking;
        int ok = skinned_instance_init(&bind, &model) && skinned_instance_init(&walking, &model);
        CHECK(ok, "two instances initialise");

        static Pose pose;
        skinned_model_bind_pose(&model, &pose);
        skinned_model_skin(&model, &pose, &bind);

        int walk = skinned_model_find_clip(&model, CHARACTER_CLIP_WALK);
        skinned_model_sample(&model, walk, model.clips[walk].duration * 0.25f, &pose);
        skinned_model_skin(&model, &pose, &walking);

        CHECK(instances_differ(&bind, &walking, 1e-3f), "a walk frame differs from the bind pose");
        float reach = max_vertex_reach(&walking);
        CHECK(isfinite(reach) && reach < 3.0f, "no vertex is thrown away from the body");

        /* Blending halfway lands between the two, not on either. */
        static Pose bind_pose, blended;
        skinned_model_bind_pose(&model, &bind_pose);
        skinned_model_blend(&model, &bind_pose, &pose, 0.5f, &blended);
        SkinnedInstance half;
        ok = skinned_instance_init(&half, &model);
        skinned_model_skin(&model, &blended, &half);
        CHECK(ok && instances_differ(&half, &bind, 1e-3f) && instances_differ(&half, &walking, 1e-3f),
              "a half blend is neither end");

        skinned_instance_free(&half);
        skinned_instance_free(&walking);
        skinned_instance_free(&bind);
    }

	printf("\nTEST 5: every mesh of the shipped model is named and shaded\n");
	{
		int named = 1, shaded = 1;
		for(int m = 0; m < model.mesh_count; ++m) {
			if(model.meshes[m].name[0] == '\0')
				named = 0;
			if(model.meshes[m].tint_shade < 0.0f || model.meshes[m].tint_shade > 1.0f)
				shaded = 0;
		}
		CHECK(named, "every mesh carries the name the file gave it");
		CHECK(shaded, "every mesh has a tint shade between 0 and 1");
		CHECK(model.meshes[0].tint_shade == 1.0f, "the body material takes a tint in full");
		if(model.mesh_count > 1)
			CHECK(model.meshes[1].tint_shade < 1.0f, "the joints material is a darker shade");
	}

	printf("\nTEST 6: a kit of several meshes loads, named, and a body can wear some\n");
	{
		const char* kit_path = "model_test_kit.glb";
		SkinnedModel kit;
		char kit_error[256] = "";

		CHECK(write_kit_glb(kit_path), "the kit file is written");
		int kit_ok = skinned_model_load_glb(&kit, kit_path, kit_error, (int)sizeof(kit_error));
		if(!kit_ok)
			printf("  (%s)\n", kit_error);
		CHECK(kit_ok, "it loads");
		remove(kit_path);
		if(kit_ok) {
			CHECK(kit.mesh_count == 2, "both mesh nodes are read, not only the first");
			CHECK(strcmp(kit.meshes[0].name, "Body_M") == 0, "the first is named by its node");
			CHECK(strcmp(kit.meshes[1].name, "Ears_wolf") == 0,
				"Blender's .001 copy suffix is dropped");
			CHECK(strcmp(kit.meshes[0].material, "M_Main.001") == 0, "the material name is kept");
			CHECK(kit.meshes[0].tint_shade == 1.0f, "M_Main.001 still takes a tint in full");
			CHECK(kit.meshes[1].tint_shade == 0.0f, "an unlisted material keeps its own colour");
			CHECK(kit.meshes[1].base_color[0] > 0.19f && kit.meshes[1].base_color[0] < 0.21f,
				"and keeps its own colour");
			CHECK(kit.max_y == 1.0f, "height is measured from the body, not the tall ears");

			SkinnedInstance worn;
			CHECK(skinned_instance_init_masked(&worn, &kit, 1), "a body wearing only mesh 0 inits");
			CHECK(worn.mesh_count == kit.mesh_count, "its mesh slots still line up with the model");
			CHECK(worn.meshes[0].vertices != NULL && worn.meshes[0].vertex_count == 3,
				"the worn mesh has its vertices");
			CHECK(worn.meshes[1].vertices == NULL && worn.meshes[1].vertex_count == 0,
				"the mesh it does not wear costs nothing");

			static Pose kit_pose;
			skinned_model_bind_pose(&kit, &kit_pose);
			skinned_model_skin(&kit, &kit_pose, &worn);
			CHECK(max_vertex_reach(&worn) < 2.0f, "skinning a masked body is safe");
			skinned_instance_free(&worn);
			skinned_model_free(&kit);
		}
	}

	printf("\nTEST 7: the shipped kit holds what the look rule and the rig expect\n");
	{
		static const char* const PART_BONES[] = {
			"ear_01_l", "ear_02_l", "ear_01_r", "ear_02_r",
			"tail_01", "tail_02", "tail_03", "tail_04",
		};
		int missing = 0;

		for(int b = 0; b < (int)(sizeof(PART_BONES) / sizeof(PART_BONES[0])); b++) {
			int found = 0;

			for(int n = 0; n < model.skeleton.node_count; n++)
				if(strcmp(model.skeleton.node_names[n], PART_BONES[b]) == 0)
					found = 1;
			if(!found) {
				printf("  (no bone %s)\n", PART_BONES[b]);
				missing++;
			}
		}
		CHECK(missing == 0, "every ear and tail bone is in the rig");

		SkinMeshMask male = character_look_mask(&model, "wolf", appearance_pack(APPEARANCE_BODY_MALE), 0);
		SkinMeshMask female = character_look_mask(&model, "wolf", appearance_pack(APPEARANCE_BODY_FEMALE), 0);
		SkinMeshMask cat = character_look_mask(&model, "cat", appearance_pack(APPEARANCE_BODY_MALE), 0);
		SkinMeshMask claws = character_look_mask(&model, "wolf", 0, CHARACTER_LOOK_CLAWS);
		int wolf_parts = 0, claw_meshes = 0;

		for(int m = 0; m < model.mesh_count; m++) {
			if(strcmp(model.meshes[m].name, "Ears_wolf") == 0 || strcmp(model.meshes[m].name, "Tail_wolf") == 0)
				wolf_parts++;
			if(strcmp(model.meshes[m].name, "Claws_wolf") == 0)
				claw_meshes++;
		}
		CHECK(wolf_parts > 0, "the kit has wolf ears and a tail");
		CHECK(male != female, "the two bodies are different meshes");
		CHECK(male != 0 && female != 0, "each look wears something");
		CHECK((male & female) != 0, "and the wolf's parts are shared between them");
		CHECK(cat != male, "a cat wears fewer meshes than a wolf, having no parts in the kit yet");
		CHECK(claw_meshes > 0 && claws != male, "claws are in the kit and worn only on request");

		// Both bodies stand on the same floor at the same height, or one of
		// them would sink or float when the renderer scales from the other
		float low[2] = { 1e9f, 1e9f }, high[2] = { -1e9f, -1e9f };

		for(int m = 0; m < model.mesh_count; m++) {
			int body = strcmp(model.meshes[m].name, "Body_M") == 0 ? 0 :
			           strcmp(model.meshes[m].name, "Body_F") == 0 ? 1 : -1;

			if(body < 0)
				continue;
			for(int v = 0; v < model.meshes[m].vertex_count; v++) {
				float y = model.meshes[m].positions[v * 3 + 1];

				if(y < low[body])
					low[body] = y;
				if(y > high[body])
					high[body] = y;
			}
		}
		// Each ear must face outward, away from its own centre. A mirrored ear
		// with its normals left alone is inside-out, and with back faces culled
		// the game shows the inside of it
		int ears_checked = 0, ears_inside_out = 0;

		for(int side = -1; side <= 1; side += 2) {
			float centre[3] = { 0, 0, 0 };
			int count = 0;

			for(int m = 0; m < model.mesh_count; m++) {
				if(strncmp(model.meshes[m].name, "Ears_", 5) != 0)
					continue;
				for(int v = 0; v < model.meshes[m].vertex_count; v++) {
					const float* p = model.meshes[m].positions + v * 3;

					if(p[0] * (float)side <= 0.0f)
						continue;
					for(int a = 0; a < 3; a++)
						centre[a] += p[a];
					count++;
				}
			}
			if(count == 0)
				continue;
			for(int a = 0; a < 3; a++)
				centre[a] /= (float)count;

			float outward = 0.0f;

			for(int m = 0; m < model.mesh_count; m++) {
				if(strncmp(model.meshes[m].name, "Ears_", 5) != 0)
					continue;
				for(int v = 0; v < model.meshes[m].vertex_count; v++) {
					const float* p = model.meshes[m].positions + v * 3;
					const float* n = model.meshes[m].normals + v * 3;

					if(p[0] * (float)side <= 0.0f)
						continue;
					outward += (p[0] - centre[0]) * n[0] + (p[1] - centre[1]) * n[1] +
					           (p[2] - centre[2]) * n[2];
				}
			}
			ears_checked++;
			if(outward <= 0.0f)
				ears_inside_out++;
		}
		CHECK(ears_checked == 2, "both ears are present, one each side");
		CHECK(ears_inside_out == 0, "and both face outward, neither is inside-out");

		// The tail starts on the lower back, above the pelvis joint (0.92 m on this
		// rig), not down at the hip
		float tail_top = -1e9f;

		for(int m = 0; m < model.mesh_count; m++) {
			if(strncmp(model.meshes[m].name, "Tail_", 5) != 0)
				continue;
			for(int v = 0; v < model.meshes[m].vertex_count; v++)
				if(model.meshes[m].positions[v * 3 + 1] > tail_top)
					tail_top = model.meshes[m].positions[v * 3 + 1];
		}
		CHECK(tail_top >= 1.0f, "the tail starts at least a metre up, on the lower back");

		CHECK(fabsf(low[0] - low[1]) < 0.02f, "both bodies' feet are at the same height");
		CHECK(fabsf(high[0] - high[1]) < 0.05f, "and the two bodies are the same height to 5 cm");
	}

    skinned_model_free(&model);
    CHECK(model.mesh_count == 0 && model.clips == NULL, "free leaves the model empty");

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}

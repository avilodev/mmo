// Check the emote hub's data: clip table, six slots and their file, layout.

#include "player/emote_clips.h"
#include "player/emote_slots.h"
#include "ui/emote_hub_layout.h"
#include "core/keybinds.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH "/tmp/emote_hub_test_slots.cfg"

KeyBinds g_keybinds;

static void test_clip_table(void) {
	char label[64];

	assert(emote_clip_count() == EMOTE_HUB_CLIP_COUNT);
	assert(emote_clip_name(-1) == NULL && emote_clip_name(EMOTE_HUB_CLIP_COUNT) == NULL);

	// every name is there once, and finds itself
	for(int i = 0; i < EMOTE_HUB_CLIP_COUNT; i++) {
		const char* name = emote_clip_name(i);

		assert(name && name[0]);
		assert(emote_clip_find(name) == i);
	}

	assert(emote_clip_find("No_Such_Clip") == -1 && emote_clip_find(NULL) == -1);

	emote_clip_label(emote_clip_find("Idle_Torch_Loop"), label, sizeof(label));
	assert(strcmp(label, "Idle Torch") == 0);
	emote_clip_label(emote_clip_find("Yes"), label, sizeof(label));
	assert(strcmp(label, "Yes") == 0);
	emote_clip_label(emote_clip_find("Sword_Regular_A"), label, sizeof(label));
	assert(strcmp(label, "Sword Regular A") == 0);
	emote_clip_label(emote_clip_find("Sword_Regular_A"), label, 6);
	assert(strcmp(label, "Sword") == 0);
	emote_clip_label(-1, label, sizeof(label));
	assert(label[0] == '\0');
}

static void test_clip_sets(void) {
	int middle = emote_clip_find("Spell_Simple_Idle_Loop");
	int enter = emote_clip_find("Spell_Simple_Enter");
	int exit = emote_clip_find("Spell_Simple_Exit");
	int seen_middle = 0;
	int shown = 0;

	// a set is named by its middle clip, which finds the other two
	assert(emote_clip_enter(middle) == enter && emote_clip_exit(middle) == exit);
	assert(emote_clip_enter(enter) == -1 && emote_clip_exit(emote_clip_find("Dance_Loop")) == -1);
	assert(emote_clip_enter(-1) == -1 && emote_clip_exit(EMOTE_HUB_CLIP_COUNT) == -1);

	// the hub lists each clip once, but not the intro and exit of a set
	for(int n = 0; n < emote_hub_count(); n++) {
		int clip = emote_hub_clip(n);

		assert(clip >= 0 && clip < EMOTE_HUB_CLIP_COUNT);
		assert(clip != enter && clip != exit);
		assert(clip != emote_clip_find("Slide_Start") && clip != emote_clip_find("Jump_Land"));
		seen_middle += clip == middle;
		shown++;
	}
	assert(seen_middle == 1 && shown == emote_hub_count());
	assert(emote_hub_count() == EMOTE_HUB_CLIP_COUNT - 10);
	assert(emote_hub_clip(-1) == -1 && emote_hub_clip(emote_hub_count()) == -1);

	// the order is the table's, with the whole emotes last
	assert(emote_hub_clip(0) == 0);
	assert(emote_hub_clip(emote_hub_count() - 1) == EMOTE_CLIP_LEAN_2);
	assert(emote_hub_clip(emote_hub_count() - 2) == EMOTE_CLIP_LEAN_1);
}

static void test_set_labels(void) {
	char label[64];

	// a set reads as its own name, not as its idle clip's
	emote_clip_label(emote_clip_find("Slide_Loop"), label, sizeof(label));
	assert(strcmp(label, "Slide") == 0);
	emote_clip_label(emote_clip_find("Sitting_Idle_Loop"), label, sizeof(label));
	assert(strcmp(label, "Sit Down") == 0);
	emote_clip_label(emote_clip_find("NinjaJump_Idle_Loop"), label, 6);
	assert(strcmp(label, "Ninja") == 0);
}

static void test_whole_emotes(void) {
	char label[64];

	// the leans are hub entries past the wire clips, each its own emote
	assert(EMOTE_CLIP_LEAN_1 == EMOTE_CLIP_COUNT && EMOTE_CLIP_LEAN_2 == EMOTE_CLIP_COUNT + 1);
	assert(emote_clip_find("Lean_1") == EMOTE_CLIP_LEAN_1);
	assert(emote_clip_find("Lean_2") == EMOTE_CLIP_LEAN_2);
	assert(emote_clip_emote(EMOTE_CLIP_LEAN_1) == EMOTE_LEAN);
	assert(emote_clip_emote(EMOTE_CLIP_LEAN_2) == EMOTE_LEAN_FOOT);
	assert(emote_clip_emote(0) == EMOTE_CUSTOM && emote_clip_emote(EMOTE_CLIP_COUNT - 1) == EMOTE_CUSTOM);
	emote_clip_label(EMOTE_CLIP_LEAN_2, label, sizeof(label));
	assert(strcmp(label, "Lean 2") == 0);

	// only an emote slot holds one; a gait or a sit slot keeps what it had
	emote_slots_defaults();
	emote_slots_set(EMOTE_SLOT_EMOTE_2, EMOTE_CLIP_LEAN_1);
	assert(emote_slots_get(EMOTE_SLOT_EMOTE_2) == EMOTE_CLIP_LEAN_1);
	emote_slots_set(EMOTE_SLOT_WALK, EMOTE_CLIP_LEAN_1);
	emote_slots_set(EMOTE_SLOT_SIT, EMOTE_CLIP_LEAN_2);
	assert(emote_slots_get(EMOTE_SLOT_WALK) == emote_clip_find("Walk_Loop"));
	assert(emote_slots_get(EMOTE_SLOT_SIT) == emote_clip_find("Sitting_Idle_Loop"));
}

static void test_whole_emotes_file(void) {
	FILE* f;

	// a lean in an emote slot survives the file; one in a gait slot is refused
	f = fopen(SCRATCH, "w");
	assert(f);
	fprintf(f, "emote_3=Lean_2\nrun=Lean_1\n");
	fclose(f);
	emote_slots_load(SCRATCH);
	assert(emote_slots_get(EMOTE_SLOT_EMOTE_3) == EMOTE_CLIP_LEAN_2);
	assert(emote_slots_get(EMOTE_SLOT_RUN) == emote_clip_find("Jog_Fwd_Loop"));
	remove(SCRATCH);
}

static void test_defaults(void) {
	emote_slots_defaults();

	assert(strcmp(emote_clip_name(emote_slots_get(EMOTE_SLOT_WALK)), "Walk_Loop") == 0);
	assert(strcmp(emote_clip_name(emote_slots_get(EMOTE_SLOT_RUN)), "Jog_Fwd_Loop") == 0);
	assert(strcmp(emote_clip_name(emote_slots_get(EMOTE_SLOT_SIT)), "Sitting_Idle_Loop") == 0);
	assert(strcmp(emote_clip_name(emote_slots_get(EMOTE_SLOT_EMOTE_1)), "Dance_Loop") == 0);
	assert(emote_slots_get(-1) == 0 && emote_slots_get(EMOTE_SLOT_COUNT) == 0);

	assert(strcmp(emote_slot_label(EMOTE_SLOT_WALK), "Walk") == 0);
	assert(strcmp(emote_slot_label(EMOTE_SLOT_EMOTE_3), "Emote 3") == 0);
}

static void test_any_clip_in_any_slot(void) {
	int sword = emote_clip_find("Sword_Regular_A");

	emote_slots_defaults();
	emote_slots_set(EMOTE_SLOT_WALK, sword);
	emote_slots_set(EMOTE_SLOT_EMOTE_2, emote_clip_find("Walk_Loop"));
	assert(emote_slots_get(EMOTE_SLOT_WALK) == sword);
	assert(emote_slots_get(EMOTE_SLOT_EMOTE_2) == emote_clip_find("Walk_Loop"));

	// a bad slot or clip changes nothing
	emote_slots_set(EMOTE_SLOT_COUNT, sword);
	emote_slots_set(EMOTE_SLOT_RUN, EMOTE_HUB_CLIP_COUNT);
	assert(strcmp(emote_clip_name(emote_slots_get(EMOTE_SLOT_RUN)), "Jog_Fwd_Loop") == 0);
}

static void test_file_round_trip(void) {
	int sword = emote_clip_find("Sword_Regular_A");
	int roll = emote_clip_find("Roll");
	FILE* f;

	remove(SCRATCH);
	emote_slots_load(SCRATCH);
	assert(emote_slots_get(EMOTE_SLOT_SIT) == emote_clip_find("Sitting_Idle_Loop"));

	// setting saves, and a later load reads it back
	emote_slots_set(EMOTE_SLOT_SIT, sword);
	emote_slots_set(EMOTE_SLOT_EMOTE_3, roll);
	emote_slots_defaults();
	assert(emote_slots_get(EMOTE_SLOT_SIT) != sword);
	emote_slots_load(SCRATCH);
	assert(emote_slots_get(EMOTE_SLOT_SIT) == sword);
	assert(emote_slots_get(EMOTE_SLOT_EMOTE_3) == roll);
	assert(emote_slots_get(EMOTE_SLOT_WALK) == emote_clip_find("Walk_Loop"));

	// an unknown clip, an unknown slot and a comment leave the rest as they were
	f = fopen(SCRATCH, "w");
	assert(f);
	fprintf(f, "# comment\nwalk=Not_A_Clip\nflying=Roll\nrun=Roll\nsit\n");
	fclose(f);
	emote_slots_load(SCRATCH);
	assert(emote_slots_get(EMOTE_SLOT_WALK) == emote_clip_find("Walk_Loop"));
	assert(emote_slots_get(EMOTE_SLOT_RUN) == roll);
	assert(emote_slots_get(EMOTE_SLOT_SIT) == emote_clip_find("Sitting_Idle_Loop"));
	remove(SCRATCH);
}

static void test_emote_keys(void) {
	g_keybinds.emote[0] = 90;
	g_keybinds.emote[1] = 88;
	g_keybinds.emote[2] = 86;

	assert(emote_slot_key(EMOTE_SLOT_EMOTE_1) == 90);
	assert(emote_slot_key(EMOTE_SLOT_EMOTE_3) == 86);
	assert(emote_slot_key(EMOTE_SLOT_WALK) == 0 && emote_slot_key(EMOTE_SLOT_SIT) == 0);
}

static int inside(const hub_rect* outer, const hub_rect* r) {
	return r->x >= outer->x && r->y >= outer->y && r->x + r->w <= outer->x + outer->w &&
	       r->y + r->h <= outer->y + outer->h;
}

static int overlaps(const hub_rect* a, const hub_rect* b) {
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h &&
	       b->y < a->y + a->h;
}

static void test_layout_fits(void) {
	hub_rect panel = hub_panel(3200.0f, 1800.0f);
	hub_rect track = hub_scroll_track(&panel);
	hub_rect close = hub_close_button(&panel);

	assert(panel.x == (3200.0f - HUB_PANEL_W) * 0.5f && panel.y == (1800.0f - HUB_PANEL_H) * 0.5f);
	assert(inside(&panel, &track) && inside(&panel, &close));

	for(int slot = 0; slot < EMOTE_SLOT_COUNT; slot++) {
		hub_rect area = hub_slot_area(&panel, slot);
		hub_rect box = hub_slot_box(&panel, slot);

		assert(inside(&panel, &area) && inside(&area, &box));
		for(int other = slot + 1; other < EMOTE_SLOT_COUNT; other++) {
			hub_rect next = hub_slot_area(&panel, other);

			assert(!overlaps(&area, &next));
		}
	}

	for(int place = 0; place < HUB_VISIBLE; place++) {
		hub_rect cell = hub_cell_box(&panel, place);

		assert(inside(&panel, &cell));
		assert(cell.x + cell.w < track.x);
		for(int slot = 0; slot < EMOTE_SLOT_COUNT; slot++) {
			hub_rect area = hub_slot_area(&panel, slot);

			assert(!overlaps(&cell, &area));
		}
		for(int other = place + 1; other < HUB_VISIBLE; other++) {
			hub_rect next = hub_cell_box(&panel, other);

			assert(!overlaps(&cell, &next));
		}
	}
}

static void test_picking(void) {
	hub_rect panel = hub_panel(3200.0f, 1800.0f);
	hub_rect cell = hub_cell_box(&panel, 7);
	int place = -1;

	// the middle of a box names its clip, whatever the scroll
	assert(hub_clip_at(&panel, 0, cell.x + 5.0f, cell.y + 5.0f, &place) == emote_hub_clip(7) && place == 7);
	assert(hub_clip_at(&panel, 2, cell.x + 5.0f, cell.y + 5.0f, &place) == emote_hub_clip(2 * HUB_COLS + 7));
	assert(hub_clip_at(&panel, 0, panel.x + 1.0f, panel.y + 1.0f, NULL) == -1);

	// the gap between two boxes is no box
	assert(hub_clip_at(&panel, 0, cell.x + cell.w + 2.0f, cell.y + 5.0f, NULL) == -1);

	// past the last clip there is nothing to pick up
	for(int place_at = 0; place_at < HUB_VISIBLE; place_at++) {
		hub_rect box = hub_cell_box(&panel, place_at);
		int clip = hub_clip_at(&panel, hub_max_scroll(), box.x + 1.0f, box.y + 1.0f, NULL);
		int want = hub_max_scroll() * HUB_COLS + place_at;

		assert(clip == emote_hub_clip(want));
	}

	for(int slot = 0; slot < EMOTE_SLOT_COUNT; slot++) {
		hub_rect box = hub_slot_box(&panel, slot);
		hub_rect area = hub_slot_area(&panel, slot);

		assert(hub_slot_at(&panel, box.x + box.w * 0.5f, box.y + box.h * 0.5f) == slot);
		assert(hub_slot_at(&panel, area.x + 4.0f, area.y + 4.0f) == slot);
	}
	assert(hub_slot_at(&panel, cell.x + 5.0f, cell.y + 5.0f) == -1);
}

static void test_scrolling(void) {
	hub_rect panel = hub_panel(3200.0f, 1800.0f);
	hub_rect track = hub_scroll_track(&panel);
	int max = hub_max_scroll();

	assert(max == (emote_hub_count() + HUB_COLS - 1) / HUB_COLS - HUB_ROWS && max > 0);

	// the last page ends on the last clip
	assert(max * HUB_COLS + HUB_VISIBLE >= emote_hub_count());
	assert((max - 1) * HUB_COLS + HUB_VISIBLE < emote_hub_count() + HUB_COLS);

	assert(hub_scroll_for(&panel, track.y - 50.0f) == 0);
	assert(hub_scroll_for(&panel, track.y + track.h + 50.0f) == max);
	assert(hub_scroll_for(&panel, track.y + track.h * 0.5f) > 0);
	assert(hub_scroll_for(&panel, track.y + track.h * 0.5f) < max);
}

int main(void) {
	test_clip_table();
	test_clip_sets();
	test_set_labels();
	test_whole_emotes();
	test_whole_emotes_file();
	test_defaults();
	test_any_clip_in_any_slot();
	test_file_round_trip();
	test_emote_keys();
	test_layout_fits();
	test_picking();
	test_scrolling();
	printf("emote_hub_test: OK\n");

	return 0;
}

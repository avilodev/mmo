import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))

import sync_items
from sync_items import ItemSyncError, slice_items


def item(**fields):
    base = {"id": 1, "name": "Cap"}
    base.update(fields)
    return base


class SliceItemsTest(unittest.TestCase):
    def test_copies_every_player_visible_field(self):
        full = item(description="A cap.", type="armor", slot="helmet", rarity="rare",
                    max_stack=1, damage=2, defense=3, level_req=4, value=5,
                    bind_on_pickup=1, bind_on_equip=1, use_effect="restore_health",
                    use_value=40, use_cooldown=10.0, bonus_vitality=2, bonus_armor=1)
        doc, dropped = slice_items({"items": [full]})
        self.assertEqual(doc["items"], [full])
        self.assertEqual(dropped, [])

    def test_every_stat_has_a_bonus_field(self):
        for key in sync_items.STAT_KEYS:
            self.assertIn("bonus_" + key, sync_items.CLIENT_FIELDS)

    def test_drops_server_only_fields_and_reports_them(self):
        doc, dropped = slice_items({"items": [item(drop_weight=9, internal_note="x")]})
        self.assertEqual(doc["items"], [{"id": 1, "name": "Cap"}])
        self.assertEqual(dropped, ["drop_weight", "internal_note"])

    def test_drops_loot_tables(self):
        doc, _ = slice_items({"items": [item()], "loot_tables": [{"id": 1}]})
        self.assertNotIn("loot_tables", doc)

    def test_does_not_invent_absent_fields(self):
        doc, _ = slice_items({"items": [item()]})
        self.assertEqual(set(doc["items"][0]), {"id", "name"})

    def test_keeps_the_generated_comment(self):
        doc, _ = slice_items({"items": []})
        self.assertIn("Do not edit by hand", doc["_comment"])
        self.assertNotIn('"items"', doc["_comment"])

    def test_rejects_a_missing_items_list(self):
        for bad in ({}, {"items": {}}, [], None):
            with self.assertRaises(ItemSyncError):
                slice_items(bad)

    def test_rejects_an_item_without_id_or_name(self):
        for bad in ({"name": "Cap"}, {"id": "1", "name": "Cap"}, {"id": True, "name": "Cap"},
                    {"id": 1}, {"id": 1, "name": ""}, "not an object"):
            with self.assertRaises(ItemSyncError):
                slice_items({"items": [bad]})

    def test_rejects_duplicate_ids(self):
        with self.assertRaises(ItemSyncError):
            slice_items({"items": [item(), item(name="Other")]})

    def test_race_req_is_copied_and_checked(self):
        doc, _ = slice_items({"items": [item(race_req=[3, 5])]})
        self.assertEqual(doc["items"][0]["race_req"], [3, 5])
        for bad in ([1, 2, 3, 4, 5], [0], [256], ["wolf"], [True], 3, "3"):
            with self.assertRaises(ItemSyncError):
                slice_items({"items": [item(race_req=bad)]})

    def test_rejects_text_the_client_parser_cannot_read(self):
        for bad in ('say "hi"', "back\\slash", "tab\there", "café"):
            with self.assertRaises(ItemSyncError):
                slice_items({"items": [item(description=bad)]})
        with self.assertRaises(ItemSyncError):
            slice_items({"items": [item(description=5)]})


class SyncTest(unittest.TestCase):
    def test_writes_a_file_the_client_can_read_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            server = os.path.join(tmp, "server.json")
            client = os.path.join(tmp, "client.json")
            with open(server, "w", encoding="utf-8") as f:
                json.dump({"items": [item(value=7, loot_note="x")], "loot_tables": []}, f)

            count, dropped = sync_items.sync(server, client)

            with open(client, encoding="utf-8") as f:
                written = json.load(f)
            self.assertEqual(count, 1)
            self.assertEqual(dropped, ["loot_note"])
            self.assertEqual(written["items"], [{"id": 1, "name": "Cap", "value": 7}])

    def test_the_client_file_matches_the_server_file(self):
        with open(sync_items.SERVER_PATH, encoding="utf-8") as f:
            expected, _ = slice_items(json.load(f))
        with open(sync_items.CLIENT_PATH, encoding="utf-8") as f:
            actual = json.load(f)
        self.assertEqual(actual, expected, "Game/data/items.json is stale: run make items")

    def test_the_real_server_file_converts(self):
        with open(sync_items.SERVER_PATH, encoding="utf-8") as f:
            doc, _ = slice_items(json.load(f))
        self.assertTrue(doc["items"])
        for entry in doc["items"]:
            self.assertTrue(entry.get("description"), "item %d has no description" % entry["id"])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Generate the opening-sequence content: spawns, dialogues, quests, NPC names.

The whole sequence is derived from one table below. Adding a tenth playable race
means one more row here plus its abilities in abilities.json -- the ring
placement, the NPC type id, the dialogue id, the quest id and every gate are
computed, never typed twice.
"""

import json, math, os, sys

SERVER = "mmo_server"
CLIENT = "mmo_client"

# --- World geometry ------------------------------------------------------
# Ennara's courtyard centre, from the shared city table in world_regions.h:
#   CITY_ENNARA_TILE_X = WG_SCALE(13900), CITY_ENNARA_TILE_Y = WG_SCALE(5580)
#   WG_SCALE(v) = (v + WORLD_SCALE_DIV/2) / WORLD_SCALE_DIV, WORLD_SCALE_DIV = 3
#   world pixels = tiles * WORLD_TILE_PX (16)
WORLD_SCALE_DIV = 3
TILE_PX = 16
def wg_scale(v): return (v + WORLD_SCALE_DIV // 2) // WORLD_SCALE_DIV

CENTRE_X = wg_scale(13900) * TILE_PX
CENTRE_Y = wg_scale(5580) * TILE_PX
# COURTYARD_RADIUS is WG_SCALE(220) tiles; the ring sits well inside it and the
# generator keeps the courtyard clear of buildings.
COURTYARD_HALF = wg_scale(220) * TILE_PX
RING_RADIUS = 800.0

# --- Identifier scheme ---------------------------------------------------
NPC_TYPE_WARDEN      = 20
NPC_TYPE_GATE_SENTRY = 21
NPC_TYPE_LEADER_BASE = 30    # leader type id  = 30 + race_id

DIALOGUE_WARDEN      = 10
DIALOGUE_GATE_SENTRY = 11
DIALOGUE_LEADER_BASE = 20    # leader dialogue = 20 + race_id

QUEST_CALLED_BASE    = 100   # "Called to ..."  = 100 + race_id
QUEST_INTO_ENNARA    = 200   # shared final step

def leader_type(race_id):     return NPC_TYPE_LEADER_BASE + race_id
def leader_dialogue(race_id): return DIALOGUE_LEADER_BASE + race_id
def quest_called(race_id):    return QUEST_CALLED_BASE + race_id

# --- The races that take part -------------------------------------------
# race_id matches races.json. Snake is absent on purpose: the world bible flags
# its passive as undesigned, so it has no leader to be sent to yet.
RACES = [
    dict(id=1,  key="wolf",   name="Wolf",
         leader="God of the Hunt",  domain="the Hunt"),
    dict(id=2,  key="bear",   name="Bear",
         leader="God of Might",     domain="Might"),
    dict(id=3,  key="fox",    name="Fox",
         leader="God of Secrets",   domain="Secrets"),
    dict(id=4,  key="crow",   name="Crow",
         leader="God of the Unseen", domain="the Unseen"),
    dict(id=5,  key="hawk",   name="Hawk",
         leader="God of Patience",  domain="Patience"),
    dict(id=6,  key="deer",   name="Deer",
         leader="God of Aid",       domain="Aid"),
    # The Rabbit god died fleeing with others; her people are led by a survivor.
    dict(id=8,  key="rabbit", name="Rabbit",
         leader="Elder of the Fallen", domain="the Fallen"),
    dict(id=9,  key="hyena",  name="Hyena",
         leader="God of Opportunity", domain="Opportunity"),
    # The Cat god's ninth death was final; his rooms are kept, not his example.
    dict(id=10, key="cat",    name="Cat",
         leader="Keeper of the Ninth", domain="Hubris"),
]

# --- Per-race initiation beats ------------------------------------------
# greeting  : page 0, read by everyone who walks up, before any state is known.
# turn_in   : the option text that hands in "Called to ...".
# initiation: page 1, the tonally unique beat.
# accept    : the option text that takes "Into Ennara".
# sendoff   : page 2, pointing at the gate.
# unsent    : page 3, for someone who arrived without being sent.
# revisit   : page 4, for someone already released into the city.
BEATS = {
"wolf": dict(
  greeting="Walk while you talk. Standing still is how things end up eaten.\n\n"
           "You smell like the rift. Everyone does, the first week. It passes.",
  turn_in="The Warden sent me.",
  initiation=
    "Good. Then keep up.\n\n"
    "Here is the whole of it. You are fast. You are not fast alone. Anything you can run\n"
    "down by yourself was already dying, and the rest you take with the pack at your\n"
    "shoulder -- and the pack does not wait for you to finish deciding.\n\n"
    "Change. Now, not in a moment. Animal Form is not something you put on for the hunt.\n"
    "It is how you learn the ground under your own feet. Run the ring road until you stop\n"
    "thinking about running it.",
  accept="How far?",
  sendoff=
    "Until it is boring. Then further.\n\n"
    "The sentry at the south gate logs every new mouth in this city. See her, and after\n"
    "that Ennara is yours to wear out.",
  unsent=
    "You are not on my list, and I do not keep one.\n\n"
    "Go back to the Warden in the middle of the courtyard. She sorts you. I run you.",
  revisit="Still here? Move.",
),
"bear": dict(
  greeting="You will address me properly or you will not address me at all.\n\n"
           "Try again.",
  turn_in="God of Might. The Warden sent me.",
  initiation=
    "Better.\n\n"
    "Talk is worth nothing here. I have heard a hundred Blessed explain what they intend\n"
    "to become. Animal Form. Now. Set your feet and hit me.\n\n"
    "[You are put on the ground twice before you land anything at all. The third time,\n"
    "you land something. It does not move him. He looks at it anyway.]\n\n"
    "Hm. Recruits these days. That was almost worth standing up for.",
  accept="Was that a compliment?",
  sendoff=
    "It was the only one you will get from me this year.\n\n"
    "Go. The sentry at the south gate writes down the living. Be on her list.",
  unsent=
    "Nobody sent you and you came anyway. That is either nerve or ignorance.\n\n"
    "The Warden. Middle of the courtyard. Do it properly.",
  revisit="You are still standing. Noted.",
),
"fox": dict(
  greeting="Did you find me, or did I let you? Sit with that a moment.",
  turn_in="The Warden pointed the way.",
  initiation=
    "The Warden points at a great many things.\n\n"
    "Here is your first lesson, and it is the only one I will hand over whole: nothing\n"
    "worth having in Ennara is where it is supposed to be.\n\n"
    "Look at the courtyard. Count the paving stones along the north wall. Now count them\n"
    "again.\n\n"
    "Different number, is it not? Good. That is Search. You already knew how -- you had\n"
    "simply never been told it was allowed.",
  accept="What is under the stones?",
  sendoff=
    "What a question. What a wonderfully answered-for-you question.\n\n"
    "Go and see the sentry at the south gate. Tell her nothing interesting.",
  unsent=
    "You came here without being sent. I like that. It will get you hurt, but I like it.\n\n"
    "The Warden first. Then come back and we will pretend this never happened.",
  revisit="Back already. Did you count them again?",
),
"crow": dict(
  greeting="...ah. You.\n\nI was listening for something else.",
  turn_in="The Warden sent me.",
  initiation=
    "Sent. Yes. That is one of the words for it.\n\n"
    "I will not fight you and I will not test you. I have never been much good at either,\n"
    "and the wind does not care which of us is stronger.\n\n"
    "What I can give you is height. The Windborne go over the thing instead of through it.\n"
    "Over walls. Over water. Over most arguments.\n\n"
    "Go up. Look east until the harbour stops being a shape and starts being a place.\n"
    "Something out there has not answered me in a long while. I am not saying it will not.",
  accept="What are you waiting for?",
  sendoff=
    "I have stopped phrasing it as a question. It keeps better that way.\n\n"
    "The sentry at the south gate. She writes names down. Let her have yours.",
  unsent=
    "No one sent you. That happens more than people admit.\n\n"
    "The Warden is in the middle of the courtyard, and she is easier to find than I am.",
  revisit="You came back. Most things do not.",
),
"hawk": dict(
  greeting="[She does not look at you. A full minute passes. Then, without turning:]\n\n"
           "You moved four times while you waited. That is four times too many.",
  turn_in="The Warden sent me.",
  initiation=
    "I know. I watched you cross the courtyard.\n\n"
    "Everything I have to teach is inside the minute you just failed. You will be given\n"
    "exactly one opening in most fights worth having. Not two. The Blessed who die young\n"
    "are the ones who spend theirs early, on something that was never the throat.\n\n"
    "Stand there. Do not move. When the market bell strikes, take one step forward.\n"
    "Not before it. Not after.\n\n"
    "[You wait. You wait past comfortable. The bell strikes. You step.]\n\n"
    "That one was yours.",
  accept="How will I know the next one?",
  sendoff=
    "You will not, and you will take it anyway. That is the whole difficulty.\n\n"
    "The sentry at the south gate, when you are ready. She has been ready for some time.",
  unsent=
    "You came before you were sent. Early is its own kind of wrong.\n\n"
    "The Warden. Courtyard centre. Come back when you have a reason.",
  revisit="[She is watching the market bell. She does not turn.]",
),
"deer": dict(
  greeting="There you are. Sit, if you like -- nothing here is going to test you.",
  turn_in="The Warden sent me to you.",
  initiation=
    "Of course she did. Everyone comes through me eventually, and it is far easier to\n"
    "learn this before you need it than during.\n\n"
    "So. Human Form first, because Human Form is the same for every one of us. No passive,\n"
    "no trick, nothing borrowed from the shape you were born to. It is the honest baseline,\n"
    "and it is where you will do most of your thinking.\n\n"
    "Here -- hold your hands like this. Mending is mostly attention. What you are watching\n"
    "for is the moment somebody stops being able to ask for help.\n\n"
    "The other shape will come when it comes. I am not going to push you into it in a\n"
    "courtyard.",
  accept="And if I need it before then?",
  sendoff=
    "Then you will have it, and you will be clumsy with it, and that is survivable.\n\n"
    "The sentry at the south gate is expecting you. Go gently.",
  unsent=
    "You have not been sent, have you. That is all right -- nobody is in trouble.\n\n"
    "Find the Warden in the middle of the courtyard first. She likes to be told.",
  revisit="Back so soon. Are you eating?",
),
"rabbit": dict(
  greeting="Come in out of the open. There -- that is better.\n\n"
           "You will want to know where the god is. Everyone does.",
  turn_in="The Warden sent me.",
  initiation=
    "She is not here. She has not been for a long time, and I would rather tell you plainly\n"
    "than let you work it out from how people go quiet.\n\n"
    "She died getting others out. Not fighting -- getting others out. The thing that took\n"
    "her was put down later, by someone louder, and he is very welcome to the story.\n"
    "Ours is the running.\n\n"
    "So learn this before anything else. Your speed is not for you. It is for the one\n"
    "behind you who does not have it. Anybody can be brave in one direction.",
  accept="And if I cannot carry them all?",
  sendoff=
    "Then you carry who you can, and you come back. You always come back.\n\n"
    "Go and let the sentry at the south gate write your name down. It matters that\n"
    "somebody knows it.",
  unsent=
    "Nobody sent you? Then nobody is expecting you back, and that is the part I mind.\n\n"
    "The Warden, in the middle of the courtyard. Tell her I want your name written down.",
  revisit="You came back. Good. That is the whole lesson, really.",
),
"hyena": dict(
  greeting="Mm. You are the new one.\n\n"
           "Do not stand there, you are blocking the good view of the market.",
  turn_in="The Warden sent me over.",
  initiation=
    "Course she did.\n\n"
    "Watch the fruit stall. No -- do not look at me, look at the stall. The one on the left\n"
    "has been shorting weight all morning and the one on the right knows it. Neither of\n"
    "them has said a word. Somebody is going to move first, and whoever moves first has\n"
    "the corner for the season.\n\n"
    "That is it. That is the entire teaching. Everything worth having is already in motion\n"
    "and mostly unattended. You do not need to be the strongest thing in Ennara. You need\n"
    "to be the one paying attention when the strongest thing gets tired.",
  accept="So I wait.",
  sendoff=
    "You watch. Waiting is what you call it afterwards, once it has worked.\n\n"
    "Sentry, south gate. She will want your name -- give her the short version.",
  unsent=
    "Came over on your own. Bold. Slightly useless, but bold.\n\n"
    "Warden. Courtyard centre. She is the one holding the list.",
  revisit="Still watching the stall. It has not happened yet.",
),
"cat": dict(
  greeting="You will have heard he died nine times.\n\n"
           "You will have heard the ninth one stuck.",
  turn_in="The Warden sent me.",
  initiation=
    "Good. Then I do not have to be tactful.\n\n"
    "He was magnificent. I am not being cruel -- he was the best of us and he knew it, which\n"
    "is the whole trouble. Eight times something killed him, and eight times he came back\n"
    "and told the story better than it had happened.\n\n"
    "The ninth time he went off the harbour wall to prove a point to a man who was not\n"
    "looking.\n\n"
    "I keep his rooms. I keep his notes. I am not going to teach you one single thing he\n"
    "did. I am going to teach you to notice the moment you stop checking whether you can.",
  accept="And if I do not notice?",
  sendoff=
    "Then somebody keeps your rooms. It is a living.\n\n"
    "Go on. Sentry, south gate. Try not to be interesting on the way.",
  unsent=
    "Nobody sent you and you walked in anyway. He used to do that.\n\n"
    "The Warden, courtyard centre. Humour me.",
  revisit="Still checking? Good.",
),
}

# --- Ring placement ------------------------------------------------------
def ring_position(index, total, radius=None):
    """Space entries evenly around the courtyard centre, starting due north.

    The leaders sit on the inner ring; the enemy camps use the same placement at
    a wider radius, so the two are laid out by one function and cannot drift into
    each other.
    """
    r = RING_RADIUS if radius is None else radius
    angle = math.radians(-90.0 + (360.0 / total) * index)
    return (round(CENTRE_X + r * math.cos(angle), 1),
            round(CENTRE_Y + r * math.sin(angle), 1))

WARDEN_POS = (float(CENTRE_X), float(CENTRE_Y) - 160.0)
# South edge of the courtyard, on the road that leaves through the gate.
SENTRY_POS = (float(CENTRE_X), float(CENTRE_Y) + COURTYARD_HALF - 120.0)

# --- Spawns --------------------------------------------------------------
def build_spawns():
    spawns = [
        dict(name="Courtyard Warden", npc_type_id=NPC_TYPE_WARDEN,
             x=WARDEN_POS[0], y=WARDEN_POS[1], health=1000, hitbox_radius=18.0,
             dialogue_id=DIALOGUE_WARDEN, is_interactable=1,
             category="quest", respawn_time=0.0, armor=0, xp_reward=0),
        dict(name="Gate Sentry", npc_type_id=NPC_TYPE_GATE_SENTRY,
             x=SENTRY_POS[0], y=SENTRY_POS[1], health=1000, hitbox_radius=18.0,
             dialogue_id=DIALOGUE_GATE_SENTRY, is_interactable=1,
             category="quest", respawn_time=0.0, armor=0, xp_reward=0),
    ]
    for i, race in enumerate(RACES):
        x, y = ring_position(i, len(RACES))
        spawns.append(dict(
            name=race["leader"], npc_type_id=leader_type(race["id"]),
            x=x, y=y, health=5000, hitbox_radius=20.0,
            dialogue_id=leader_dialogue(race["id"]), is_interactable=1,
            category="quest", respawn_time=0.0, armor=0, xp_reward=0))
    spawns.extend(build_enemy_spawns())
    return spawns


# --- Enemy camps ---------------------------------------------------------
#
# Where the roster actually stands. One camp per faction, placed on a ring
# outside the courtyard, so a player leaving the opening area walks into content
# rather than into empty ground.
#
# Camps are laid out from the registry rather than listed here: each names the
# type keys it holds, and everything else -- health, armour, XP, hitbox, name --
# comes from the type. That is the same promise types.json makes to the server,
# kept in the one other place a type is turned into an entity.

ENEMY_RING_RADIUS = COURTYARD_HALF + wg_scale(90) * TILE_PX
ENEMY_RESPAWN_SECONDS = 45.0

# Faction key -> the camp it fields, nearest ids first. Packs and adds are listed
# once per body: a pack of five Mini Wolves is five entries, because a spawn row
# is one entity and the summoner path is what creates them in bulk.
ENEMY_CAMPS = [
    ("feral",     ["feral_wolf", "feral_wolf", "feral_wolf",
                   "feral_boar", "feral_hawk", "feral_bear"]),
    ("sundered",  ["sundered_risen_soldier", "sundered_risen_soldier",
                   "sundered_risen_soldier", "sundered_bone_archer",
                   "sundered_wailing_spirit", "sundered_cursed_knight"]),
    ("choir",     ["choir_cultist", "choir_cultist", "choir_failed_experiment",
                   "choir_grafted_horror", "choir_bio_caster", "choir_experimenter"]),
    ("fang",      ["fang_rusher", "fang_rusher", "fang_dasher", "fang_volley",
                   "fang_brawler", "fang_net_thrower"]),
    ("hunt",      ["hunt_bonded_wolf", "hunt_bonded_wolf", "hunt_handler",
                   "hunt_skinner", "hunt_ice_slinger", "hunt_packmaster"]),
    ("vessane",   ["vessane_zealot", "vessane_zealot", "vessane_ritualist",
                   "vessane_fanatic", "vessane_charm_singer",
                   "vessane_ashen_acolyte"]),
    ("unpara",    ["unpara_purge_rusher", "unpara_purge_rusher", "unpara_hunter",
                   "unpara_enforcer", "unpara_chain_breaker", "unpara_firebrand"]),
    ("forgotten", ["forgotten_rogue_wolf", "forgotten_rogue_wolf",
                   "forgotten_rogue_hyena", "forgotten_rogue_rabbit",
                   "forgotten_rogue_hawk", "forgotten_rogue_deer"]),
]

# One affixed enemy, so the affix system is reachable rather than merely present.
AFFIXED = {"fang_brawler": "duelist"}


def build_enemy_spawns():
    """Place one camp per faction on a ring outside the courtyard.

    Returns an empty list when the content registry is absent, for the same
    reason build_enemy_types() does: the opening sequence has to regenerate in a
    tree where enemy content has not been authored yet.
    """
    types_path = os.path.join(NPC_CONTENT_DIR, "types.json")
    if not os.path.exists(types_path):
        return []

    by_key = {t["key"]: t for t in load_json(types_path)["npc_types"]}
    arche_path = os.path.join(NPC_CONTENT_DIR, "archetypes.json")
    hitboxes = {a["key"]: a.get("hitbox_radius", 16.0)
                for a in load_json(arche_path)["archetypes"]}

    rows = []
    for camp_index, (faction, members) in enumerate(ENEMY_CAMPS):
        cx, cy = ring_position(camp_index, len(ENEMY_CAMPS), ENEMY_RING_RADIUS)

        for member_index, key in enumerate(members):
            t = by_key.get(key)
            if t is None:
                raise SystemExit("spawns: camp '%s' names unknown type '%s'"
                                 % (faction, key))
            if t["faction"] != faction:
                raise SystemExit("spawns: '%s' is %s, not %s"
                                 % (key, t["faction"], faction))

            # Scatter within the camp rather than stacking on its centre.
            angle = 2.0 * math.pi * member_index / len(members)
            spread = wg_scale(14) * TILE_PX
            row = dict(
                name=t["name"], npc_type_id=t["id"],
                x=round(cx + math.cos(angle) * spread, 1),
                y=round(cy + math.sin(angle) * spread, 1),
                health=t["health"],
                hitbox_radius=t.get("hitbox_radius",
                                    hitboxes.get(t["archetype"], 16.0)),
                dialogue_id=0, is_interactable=0,
                category="hostile",
                respawn_time=ENEMY_RESPAWN_SECONDS,
                armor=t.get("armor", 0),
                xp_reward=t.get("xp_reward", 0))
            if key in AFFIXED:
                row["affix"] = AFFIXED[key]
            rows.append(row)

    return rows

# --- Dialogue ------------------------------------------------------------
def option(option_id, text, next_page, conditions=None, action=None,
           action_value=None, fail_page=None):
    o = {"option_id": option_id, "text": text, "next_page": next_page}
    if fail_page is not None: o["fail_page"] = fail_page
    if action: o["action"] = action
    if action_value is not None: o["action_value"] = action_value
    if conditions: o["conditions"] = conditions
    return o

def cond(kind, value): return {"type": kind, "value": value}

def warden_dialogue():
    """The one conversation every Blessed has, whatever they turned out to be."""
    pages = [
        {"page_num": 0,
         "text":
            "The rift closed behind you three days ago. That is what it does -- opens, puts\n"
            "somebody down in this courtyard, closes again. You have been asleep since.\n\n"
            "This is Ennara, and Ennara is what is still standing. You are Blessed, which is\n"
            "the polite word for two shapes and no instructions. Neither of those is your\n"
            "fault, and neither of them is going away.",
         "options": [
            option(1, "Blessed? Explain that.", 1),
            option(2, "What is Ennara?", 2),
            option(3, "Then tell me what to do.", 3),
         ]},
        {"page_num": 1,
         "text":
            "Two shapes. Human Form and Animal Form, and you will be switching between them\n"
            "for the rest of your life.\n\n"
            "Human Form is identical for every one of us -- no gift, no trait, nothing inherited.\n"
            "Animal Form is where whatever you are actually shows itself, and it is not a\n"
            "choice you get to make twice.\n\n"
            "Which one you carry is already decided. You will find out what it costs you soon\n"
            "enough.",
         "options": [option(1, "Go on.", 0)]},
        {"page_num": 2,
         "text":
            "The fourth kingdom, and the only capital still taking arrivals. The other three\n"
            "have their own walls and their own coin and their own opinions about the rift.\n\n"
            "You are standing in the courtyard. Market north, guild south of that, harbour east\n"
            "where the water is. The gate out is on the south road, and there is a sentry on it\n"
            "who will want your name before you use it.",
         "options": [option(1, "Understood.", 0)]},
        {"page_num": 3,
         "text":
            "The same thing every Blessed does first. You go and be seen by the one who carries\n"
            "your shape.\n\n"
            "They are all here, all around this courtyard, and they have all been waiting on\n"
            "somebody. Whether they admit to waiting is a matter of temperament.",
         "options": []},
    ]

    # One race-gated option per race, plus the one anybody may take. Only the
    # matching race ever sees theirs, so the page reads as a single instruction.
    branch = pages[3]["options"]
    option_id = 1
    for i, race in enumerate(RACES):
        branch.append(option(
            option_id,
            "Where do I find the %s?" % race["leader"],
            10 + i,
            conditions=[cond("race", race["key"])],
            action="quest_accept",
            action_value=quest_called(race["id"])))
        option_id += 1
    branch.append(option(option_id, "I will find my own way.", -1))

    for i, race in enumerate(RACES):
        x, y = ring_position(i, len(RACES))
        bearing = compass_bearing(i, len(RACES))
        pages.append({
            "page_num": 10 + i,
            "text":
                "The %s. %s side of the courtyard -- you will know them when you see them.\n\n"
                "Go and be seen. Whatever happens after that is between the two of you."
                % (race["leader"], bearing),
            "options": [option(1, "I will go.", -1)],
        })

    return {"id": DIALOGUE_WARDEN, "name": "Courtyard Warden", "pages": pages}

def compass_bearing(index, total):
    """Name the ring position in words, so directions match where the NPC stands.

    The ring angle is measured the way ring_position() uses it: zero is +x, which
    is east, and it increases towards +y, which is south on screen. Naming the
    buckets from north instead sent every player a quarter-turn the wrong way.

    Sixteen points rather than eight because nine leaders spaced evenly around a
    ring cannot land on eight distinct names, and two of them sharing "south"
    reads like a mistake even though the ring is correct.
    """
    names = ["east", "east-southeast", "southeast", "south-southeast",
             "south", "south-southwest", "southwest", "west-southwest",
             "west", "west-northwest", "northwest", "north-northwest",
             "north", "north-northeast", "northeast", "east-northeast"]
    angle = (-90.0 + (360.0 / total) * index) % 360.0
    name = names[int(round(angle / 22.5)) % 16]
    return name[0].upper() + name[1:]

def leader_dialogue_doc(race):
    """Build one race leader's conversation from its beats and its quest ids."""
    beats = BEATS[race["key"]]
    called = quest_called(race["id"])

    page0_options = [
        # Talking is itself the objective, so the hand-in is live on arrival.
        option(1, beats["turn_in"], 1,
               conditions=[cond("quest_complete", called)],
               action="quest_turnin", action_value=called, fail_page=-1),
        # Accepted but somehow unticked -- reachable if the log was restored mid-step.
        option(2, beats["turn_in"], 0,
               conditions=[cond("quest_active", called)]),
        option(3, "I was not sent here.", 3,
               conditions=[cond("quest_not_started", called)]),
        # Sent onward but not yet through the gate.
        option(4, "Where is the gate again?", 2,
               conditions=[cond("quest_turned_in", called),
                           cond("quest_active", QUEST_INTO_ENNARA)]),
        option(5, "Nothing. Just passing.", 4,
               conditions=[cond("quest_turned_in", called),
                           cond("quest_turned_in", QUEST_INTO_ENNARA)]),
    ]

    return {
        "id": leader_dialogue(race["id"]),
        "name": race["leader"],
        "pages": [
            {"page_num": 0, "text": beats["greeting"], "options": page0_options},
            {"page_num": 1, "text": beats["initiation"], "options": [
                # Gated on the quest's own rules, not a copy of them here.
                option(1, beats["accept"], 2,
                       conditions=[cond("quest_available", QUEST_INTO_ENNARA)],
                       action="quest_accept", action_value=QUEST_INTO_ENNARA),
                # Reached again after the sendoff was already taken.
                option(2, beats["accept"], 2)]},
            {"page_num": 2, "text": beats["sendoff"], "options": [
                option(1, "I will go.", -1)]},
            {"page_num": 3, "text": beats["unsent"], "options": [
                option(1, "The Warden, then.", -1)]},
            {"page_num": 4, "text": beats["revisit"], "options": [
                option(1, "Understood.", -1)]},
        ],
    }

def gate_sentry_dialogue():
    return {
        "id": DIALOGUE_GATE_SENTRY,
        "name": "Gate Sentry",
        "pages": [
            {"page_num": 0,
             "text":
                "South gate. Nobody through without a name, and nobody back in after dark\n"
                "without a better one.\n\n"
                "State your business.",
             "options": [
                option(1, "I was sent. I am to be written down.", 1,
                       conditions=[cond("quest_complete", QUEST_INTO_ENNARA)],
                       action="quest_turnin", action_value=QUEST_INTO_ENNARA,
                       fail_page=2),
                option(2, "Nothing yet.", 2,
                       conditions=[cond("quest_not_started", QUEST_INTO_ENNARA)]),
                option(3, "Passing through.", 3,
                       conditions=[cond("quest_turned_in", QUEST_INTO_ENNARA)]),
             ]},
            {"page_num": 1,
             "text":
                "[She writes, without hurrying, and turns the book around so you can see it.]\n\n"
                "There. You exist now, officially, which is more than most things that come out\n"
                "of that rift manage.\n\n"
                "Ennara is open to you. Market north, guild east of the ring road, harbour past\n"
                "that. Whatever your one told you to go and do -- go and do it. I only keep the\n"
                "list.",
             "options": [option(1, "Thank you.", -1)]},
            {"page_num": 2,
             "text":
                "Then you are not going through my gate yet.\n\n"
                "Warden is in the middle of the courtyard. Start there, like everybody does.",
             "options": [option(1, "Right.", -1)]},
            {"page_num": 3,
             "text": "You are on the list. Go where you like.",
             "options": [option(1, "I will.", -1)]},
        ],
    }

# --- Quests --------------------------------------------------------------
def build_quests():
    quests = []
    for race in RACES:
        quests.append({
            "quest_id": quest_called(race["id"]),
            "title": ("Called to %s" % race["domain"])[:47],
            "require_race": race["key"],
            "objectives": [{
                "type": "talk",
                "target_id": leader_type(race["id"]),
                "required": 1,
                "description": ("Speak with the %s" % race["leader"])[:63],
            }],
            "xp": 120,
            "currency_reward": 25,
        })

    # The sendoff is one quest reached from nine different starts, so its
    # prerequisite is "any of them" rather than a single step. Declaring it here
    # means the server enforces the chain at the grant, not only when the leader
    # happens to offer it.
    quests.append({
        "quest_id": QUEST_INTO_ENNARA,
        "title": "Into Ennara",
        "require_quests_any": [quest_called(r["id"]) for r in RACES],
        "objectives": [{
            "type": "talk",
            "target_id": NPC_TYPE_GATE_SENTRY,
            "required": 1,
            "description": "Report to the Gate Sentry at the south gate",
        }],
        "xp": 180,
        "currency_reward": 50,
    })
    return quests

# --- NPC display table (client) -----------------------------------------
#
# The client loads exactly one data file, and this writes all of it. Quest NPCs
# come from the race table above; enemies come from the server's own content
# registry, so the two sides cannot disagree about what id 151 is called or what
# colour it is drawn in.
#
# Faction picks the hue and role picks the value and the box size. That is the
# whole scheme: a player learns eight hues and eight roles and can then read any
# enemy in the game on sight, with no art budget spent.

NPC_CONTENT_DIR = os.path.join(SERVER, "world_server/data/npc")

# Colour value multiplier, box scale, and optional outline, by role.
# Mini-bosses and elites get a white outline because it is the one cue that
# survives at a distance without art.
ROLE_PRESENTATION = {
    "melee":    (1.00, 1.00, None),
    "ranged":   (0.80, 0.80, None),
    "brute":    (0.65, 1.35, None),
    "support":  (1.25, 1.00, None),
    "summoner": (1.10, 1.10, None),
    "add":      (0.90, 0.75, None),
    "elite":    (1.00, 1.00, [1.0, 1.0, 1.0]),
    "miniboss": (1.00, 1.50, [1.0, 1.0, 1.0]),
}

QUEST_NPC_COLOR = [0.90, 0.75, 0.10]   # the gold the client already draws


def load_json(path):
    with open(path) as f:
        return json.load(f)


def shade(hue, value):
    """Apply a role's value multiplier to a faction hue, clamped to the 0-1 range."""
    return [round(min(1.0, max(0.0, c * value)), 4) for c in hue]


def build_enemy_types():
    """Read the server's content registry and derive the client's display rows.

    Returns an empty list when the registry is absent, so the opening sequence can
    still be regenerated in a tree where enemy content has not been authored yet.
    """
    types_path = os.path.join(NPC_CONTENT_DIR, "types.json")
    factions_path = os.path.join(NPC_CONTENT_DIR, "factions.json")
    if not (os.path.exists(types_path) and os.path.exists(factions_path)):
        print("note: no NPC content registry at %s; emitting quest NPCs only"
              % NPC_CONTENT_DIR)
        return []

    hues = {f["key"]: f["color"] for f in load_json(factions_path)["factions"]}

    rows = []
    for t in load_json(types_path)["npc_types"]:
        role = t.get("role", "melee")
        if role not in ROLE_PRESENTATION:
            raise SystemExit("types.json: '%s' has unknown role '%s'" % (t["key"], role))
        if t["faction"] not in hues:
            raise SystemExit("types.json: '%s' names unknown faction '%s'"
                             % (t["key"], t["faction"]))

        value, box, outline = ROLE_PRESENTATION[role]
        row = {
            "id": t["id"],
            "name": t["name"][:31],
            "color": shade(hues[t["faction"]], value),
            "size_scale": box,
        }
        if outline:
            row["outline"] = outline
        rows.append(row)
    return rows


def build_npc_types():
    types = [
        {"id": NPC_TYPE_WARDEN,      "name": "Courtyard Warden",
         "color": QUEST_NPC_COLOR, "size_scale": 1.0},
        {"id": NPC_TYPE_GATE_SENTRY, "name": "Gate Sentry",
         "color": QUEST_NPC_COLOR, "size_scale": 1.0},
    ]
    for race in RACES:
        types.append({"id": leader_type(race["id"]), "name": race["leader"][:31],
                      "color": QUEST_NPC_COLOR, "size_scale": 1.0})

    types.extend(build_enemy_types())

    # An id collision here would have the client draw one name for two enemies.
    seen = {}
    for row in types:
        if row["id"] in seen:
            raise SystemExit("npc_type id %d is used by both '%s' and '%s'"
                             % (row["id"], seen[row["id"]], row["name"]))
        seen[row["id"]] = row["name"]

    types.sort(key=lambda r: r["id"])
    return types

# --- Emit ----------------------------------------------------------------
def write_json(path, payload):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(payload, f, indent=2)
        f.write("\n")
    print("wrote", path)

def main():
    dialogue_dir = os.path.join(SERVER, "world_server/data/dialogues")
    os.makedirs(dialogue_dir, exist_ok=True)

    write_json(os.path.join(dialogue_dir, "courtyard_warden.json"), warden_dialogue())
    write_json(os.path.join(dialogue_dir, "gate_sentry.json"), gate_sentry_dialogue())
    for race in RACES:
        write_json(os.path.join(dialogue_dir, "leader_%s.json" % race["key"]),
                   leader_dialogue_doc(race))

    write_json(os.path.join(SERVER, "world_server/data/spawns.json"),
               {"_comment": "Generated by Next_steps/gen_opening.py from the race table. "
                            "Edit that script, not this file, so ring placement and the "
                            "npc_type/dialogue/quest id scheme stay in step.",
                "spawns": build_spawns()})

    write_json(os.path.join(SERVER, "world_server/data/quests.json"),
               {"_comment": "Quest rewards: 'xp' grants experience; 'currency_reward' grants "
                            "coin in the kingdom named by 'currency' (0 Ennara, 1 Kingdom 1, "
                            "2 Kingdom 2, 3 Kingdom 3 - CurrencyId in world_regions.h). A quest "
                            "that omits 'currency' pays in Ennara's coin. 'require_race', "
                            "'require_level' and 'require_quest' gate who may take it. "
                            "Generated by Next_steps/gen_opening.py.",
                "quests": build_quests()})

    write_json(os.path.join(CLIENT, "Game/data/npc_types.json"),
               {"_comment": "The only data file the client loads. Everything else - races, "
                            "abilities, items, zones, dialogue - is served by the server, so "
                            "there is one source of truth for each and no copy here to drift. "
                            "Display name, colour and box scale per npc_type_id. "
                            "Colours are derived: faction picks the hue, role picks "
                            "the value and the size, so the client never decides what "
                            "an enemy looks like. Generated by Story/gen_opening.py "
                            "from the race table and mmo_server/world_server/data/npc/.",
                "npc_types": build_npc_types()})

    print("\ncourtyard centre: (%d, %d), ring radius %.0f, courtyard half-extent %d"
          % (CENTRE_X, CENTRE_Y, RING_RADIUS, COURTYARD_HALF))

if __name__ == "__main__":
    os.chdir(sys.argv[1] if len(sys.argv) > 1 else ".")
    main()

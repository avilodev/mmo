# Hana to Taiga MMO — Opening Sequence & Race Leader Personalities
Status: Design draft. Covers first-login flow through race leader initiation for all 9 playable races.

## 1. Sequence Overview

1. **Character creation** — player selects race. Locked permanently for that character.
2. **Spawn** — player arrives in Ennara's Courtyard.
3. **Shared NPC** — identical for every player regardless of race. Provides context for arrival (rift, world state, being Blessed), then directs player to their race leader.
4. **Race Leader initiation** — tonally unique per race. Varies in: combat vs. non-combat framing, Human Form vs. Animal Form emphasis, warmth vs. distance.
5. **Sendoff into Ennara** — player released into the open city/game.

Only Wolf, Bear, and Deer are currently built out mechanically (abilities exist). All 9 have locked pantheon ties and personalities below so writing can proceed uniformly.

---

1b - Quests should appear in quests and when selected - should track the next step in the quest - if its talk to an npc its a we will say for now a yellow box above theurt head and something that shows up on map

- Do your best at assuming here with races - i will change as needed

## 2. Race Leaders

### Wolf — God of the Hunt (alive)
**Personality:** Driven, restless, always mid-motion. Doesn't lecture — leads by moving and expects you to keep pace. Pack-minded: judges the player less as an individual and more as someone who will eventually need to move *with* others.
**Initiation lean:** Likely motion/pursuit-based — a chase, a hunt, something that can't be done standing still. Animal Form emphasized early (movement/pack identity).

### Bear — God of Might (alive, prideful, expects deference)
**Personality:** Blunt, unimpressed by talk, only respects demonstrated strength. Formal in a rigid way — expects proper address, gives none of it back until earned. **Locked beat:** forces player into a fight (Animal Form), grudgingly approves even on a loss ("recruits these days...").
**Initiation lean:** Short, physical, low-dialogue. Animal Form forced immediately.

### Deer — God of Aid (alive, master healer all races turn to)
**Personality:** Warm, patient, teacherly. Doesn't test the player — walks them through things before they're needed. Closest thing to a "safe" leader tonally.
**Initiation lean:** Guided walkthrough, not a trial. Human Form likely emphasized; Animal Form teased/shown rather than forced.

### Fox — God of Secrets (alive, seeds hints toward hidden treasure)
**Personality:** Playful but evasive — answers questions with questions, never gives anything straight. Enjoys watching the player figure things out rather than being told.
**Initiation lean:** Likely puzzle/discovery-based rather than combat — fits "seeds hints toward treasure." Good candidate for teaching Search (human ability).

### Crow — God of the Unseen (fate unconfirmed — vanished mid-task, may or may not be dead)
**Personality:** Distant, cryptic, speaks like someone still listening for a reply that hasn't come. Doesn't fully commit to statements — comfortable with unresolved things.
**Initiation lean:** Likely observational/travel-flavored (fits Windborne's fly-over-terrain, no-combat identity) rather than a fight.

### Hawk — God of Patience (alive, perches over a whole city)
**Personality:** Watchful, judges silently before speaking, doesn't waste words or motion. Makes the player wait, then acts precisely once — mirrors the "waits, then strikes" domain trait.
**Initiation lean:** Likely tests patience/timing rather than raw combat — e.g. wait for the exact right moment to act.

### Rabbit — God of the Fallen (dead — died fleeing/saving others from the threat Bear later defeated)
**Personality:** Leader here is *not* the god (god is dead) — likely an elder/survivor-type figure. Gentle but grief-touched, values the group's survival over any individual's glory. Protective, urgent under the surface.
**Initiation lean:** Likely frames survival/protecting others rather than personal proving — inverse of Bear's individual trial.

### Hyena — God of Opportunity (alive)
**Personality:** Quick-witted, low-key, doesn't posture. Notices openings others miss and expects the player to start noticing too. Not aggressive — more "watch and wait for it."
**Initiation lean:** Likely reactive/opportunistic scenario — waiting for or creating an opening rather than a straight fight.

### Cat — God of Hubris (dead — ninth self-inflicted death was final)
**Personality:** Leader here also isn't the god — likely a caretaker of the god's cautionary legacy. Wry, a little weary, teaches through "don't do what he did" rather than direct instruction. Slight dark humor about the god's own failures.
**Initiation lean:** Likely a scenario built around a *mistake* to avoid rather than a trial to pass.

---

## 3. Open Items
- Only Wolf/Bear/Deer initiations have locked beats; the rest above are directional leans, not final beats.
- Rabbit and Cat leaders are NOT their gods (both dead) — needs a named figure/role eventually (elder, disciple, caretaker, etc.).
- Crow's alive/dead ambiguity should probably stay unresolved in dialogue, not clarified by the leader.
---

## 4. Implementation Notes

Status: built and playable. The sequence below runs end to end for every
playable race; the leader beats above are in the dialogue as written, and the
directional leans for the six races without locked beats are prose only — no
mechanics were invented for them.

### What a new character walks through

1. **Spawn** — Ennara's courtyard centre, from the shared city table
   (`world_regions.h`). Already the default for an unplaced character.
2. **Courtyard Warden** (centre, 160px north of the spawn) — one conversation,
   identical for everyone: the rift, Ennara, being Blessed. Its last page has
   one option per race and shows only yours, which grants *Called to …*.
3. **Race leader** (ring of nine around the courtyard, 800px out) — talking to
   them completes *Called to …*; handing it in opens the initiation beat, which
   grants *Into Ennara*.
4. **Gate Sentry** (south road, courtyard edge) — hands in *Into Ennara* and
   releases the player into the city.

### Identifier scheme

Everything is derived from the race id in `races.json`:

| Thing | Identifier |
|---|---|
| Courtyard Warden | npc type 20, dialogue 10 |
| Gate Sentry | npc type 21, dialogue 11 |
| Race leader | npc type `30 + race_id`, dialogue `20 + race_id` |
| "Called to …" quest | `100 + race_id`, gated `require_race` |
| "Into Ennara" quest | 200, shared by every race |

### Changing any of it

`Next_steps/gen_opening.py` holds the race table, the leader names, and every
line of dialogue, and writes `spawns.json`, `quests.json`, the eleven files in
`world_server/data/dialogues/`, and the client's `npc_types.json`. Edit the
script and re-run it from `dev/`:

```
python3 Next_steps/gen_opening.py .
```

Renaming a leader, rewriting a beat, or adding a tenth playable race is a change
in that one table. The ring placement, the id scheme and the gates follow.

### Tests

Two, doing different jobs.

`make test` in `mmo_server` runs **`opening_sequence_test`**, which walks the
generated data and follows every reference: a spawn that names a missing
dialogue, a dialogue that offers a missing quest, an objective pointing at an
NPC type nothing spawns, or a playable race with no path all fail the build.
It needs nothing running.

**`quest_persistence_test`** covers the quest log, the completion set, and the
save format including migration. Also offline.

**`opening_flow_test`** plays the sequence against live servers, as a client
would — TLS login, realm, character creation, world entry, then walking the
courtyard and talking to all three NPCs, asserting on the pages, the offered
options and the quest packets at every step. It is not in `make test` because it
needs the three servers, PostgreSQL and Redis up:

```
# once per day, or the realm cannot authenticate to the worlds
bash common/server_keys/generate_daily_server_keys.sh

PGPASSWORD=... ./login_server/bin/login_server &
PGPASSWORD=... ./world_server/bin/world_server world_server/world_config/Armeia.conf &
PGPASSWORD=... ./realm_server/bin/realm_server realm_server/realm_config/realm_1.conf &

make tests/bin/opening_flow_test
./tests/bin/opening_flow_test wolf     # or bear, deer, ... any race key
```

It deletes and recreates its own character each run, because the sequence under
test is a *first* login. A race that is not `playable` in races.json is refused
at character creation, which is correct: the other six leaders have their
content and are waiting on a kit.

### Supporting systems this needed

- **Dialogue conditions.** An option carries a list of requirements — race,
  level, or a quest's state — and the server sends only the options that pass.
  That is what lets one Warden serve every race from a single page. Conditions
  are re-checked when the choice comes back, so a stale client cannot take one.
- **Option identity.** Choices travel as `option_id`, not as a row number,
  because the rows differ between two players reading the same page.
- **Dialogue text on the wire.** The client no longer ships a copy of every
  conversation; the page's text and its offered choices arrive with the page.
  One source of truth, and no way to render an option the server hid.
- **No fixed registries, and no fixed player state.** Dialogues, their pages and
  their options, the quest registry, a character's active quest log and the set
  of quests they have finished are all heap-allocated and sized from what is
  actually there. Conversations are keyed by character id rather than indexed by
  it, which is what makes them work past the thousandth character.
- **Quests split by what changes them.** `quest_registry.c` holds what a quest
  *is*, `quest_storage.c` holds what one character has done with quests and the
  file that survives a logout, `quest_repeat.c` holds when a finished quest comes
  back, and `quest_system.c` holds the rules joining them and the packets.
  `quest_system.h` includes the other three, so a caller still writes one
  include, and the Makefile names them once as `QUEST_SRC`.
- **Completion split out of the quest log.** A turned-in quest used to keep its
  slot forever, because that leftover record was the only evidence it had been
  done — so the 32-entry log was a limit on quests taken in a whole lifetime,
  not on quests held at once. Finishing a quest now frees its slot and adds its
  identifier to a completion set that never forgets and has no ceiling. A
  prerequisite check is one lookup against that set.
- **Objective targets and markers.** A quest objective now carries what it
  points at and where that is, resolved from the live world at send time. The
  tracker panel, the badge over the target NPC, and the map marker all read the
  same tracked step.
- **One accept path for all three services.** `common/net_reactor.c` owns the
  epoll loops, the descriptor table, the reassembly buffers, the bounded
  blocking-worker pool and the idle sweep, and knows nothing about players,
  packets, sessions or TLS — what to do with the bytes arrives as callbacks.
  The world, realm and login servers are each a few hundred lines of "what a
  connection means here" on top of it. Login and realm used to run a thread per
  connected client, which on a patch day is a thread per returning player with
  nothing bounding the total; the thread count is now fixed at startup and a
  surge becomes a queue. TLS is a transport on the same object: the handshake is
  driven across events from the loop rather than blocking a thread.
- **Header dependencies in the server Makefile.** Not part of this feature, but
  found by it: nothing rebuilt when protocol.h changed, so the three services
  kept stale objects, each announced a PROTOCOL_VERSION it no longer shared, and
  turned each other away at the handshake. `-MMD -MP` plus an `-include` of the
  generated `.d` files fixes it.

### Giving a quest up

`J` expands a quest; the expanded row carries an Abandon button. It frees the
slot and the progress in it, and records nothing — an abandoned quest was never
finished, and the completion history never forgets, so an entry there would take
the quest away for the rest of the character's life. Taking it again starts it
over, because the progress lived in the slot that was dropped.

`PACKET_QUEST_ABANDON` is the only quest packet a client starts: quests are
taken and handed in through dialogue, never by packet. The handler re-checks
that the named quest is in the caller's own log and believes nothing else the
packet says, and the client removes its own entry only when the server confirms,
so a refused abandon leaves both sides agreeing the quest is still there.
`opening_flow_test` walks it live: abandon, a refused second attempt, a refused
fabricated one, then taking the same quest again from the Warden.

### Quest chains

A chain is declared once, on the quest, as the identifiers that must already be
finished:

```json
"require_quests_all": [101],              // every one of them
"require_quests_any": [101, 102, 103]     // any one of them
"require_quest": 101                       // sugar for a one-entry "all"
```

`require_race` and `require_level` sit alongside them. Dialogue does not restate
any of it — an option that offers a quest gates itself on
`{"type": "quest_available", "value": 200}`, which asks the quest system. That
is what stops an offer and a grant from disagreeing, and it means a fabricated
accept packet is refused by the same rule that hid the option.

`opening_sequence_test` prints the whole graph and fails on a prerequisite that
names a missing quest or on a cycle:

```
  quest chain:
    101   Called to the Hunt           race=wolf
    ...
    200   Into Ennara                  after any of {101,102,103,104,105,106,108,109,110}
```

### Quest storage on disk

`data/quests/<character_id>.bin`, magic `MASQ`, version 3: counts of active
quests, finished ones and repeat counters, then the active records, then the
finished identifiers, then the counters. Version 2 files have no counter section
and load with an empty counter table, which is correct because nobody who wrote
one had a repeatable quest. Version 1 files have no magic at all and hold one
fixed array; they still load, and their inactive records are read as completion
history, which is what they always meant. `quest_persistence_test` covers the
round trip, both migrations, and corrupt length fields.

### Repeatable quests

A quest may declare `"repeat": "once" | "free" | "daily" | "weekly"` and an
optional `"max_count"`. Absent means once, which is what every quest currently
authored says, so no data file needed an edit. `free` comes back the moment the
slot is free; `daily` and `weekly` come back on a wall-clock reset, so everyone's
flip at the same instant rather than drifting later each day.

Nothing in the opening sequence uses it — the capability exists because its
storage was close to free to add before there were characters to migrate and
would have cost a format version afterwards. A finished repeatable is written to
*both* the completion history and the counter table: prerequisites read the
history, so a counter alone would leave anything chained behind a repeatable
permanently unopenable. The dialogue layer needed no change at all, because an
offer's `quest_available` condition already asks the quest system.

### Quest tracking (item 1b)

Selecting a quest in the log (`J`) tracks it; with nothing selected the first
active quest is tracked, so a player who never opens the log is still pointed
somewhere. The tracked step appears in three places at once: a panel under the
minimap, a yellow badge over the target NPC's head, and a marker on the map
(`M`) that clamps to the edge with a tail when the target is off-view.

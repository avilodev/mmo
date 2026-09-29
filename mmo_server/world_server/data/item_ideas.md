
//Blessed Races, Stats & Builds

How it works

- Your race is your class. It sets your stats, passive, role and abilities. Only Wolf, Bear and Deer are playable.
- Human Form is the same for every race: no passive, no resource pool, and abilities that run on cooldowns only.
- Animal Form turns on the race passive, the resource pool and the Ferality power bonus. Switching forms has a 1.5s cooldown.
- Specs: Spec A is the default. Spec B unlocks at level 20, but no race has Spec B abilities yet.
- Resource by role: Tanks use Rage (Endurance), DPS use Stamina (Stamina Capacity), Healers use Mana (Focus).
- Levels: max level is 30. XP needed for the next level = 100 × 1.5^(level − 1).

The 11 stats

┌──────────────────┬────────────────────────────────┬────────────────────────────────────────────────────────────────────────────┐
│       Stat       │             Effect             │                               Formula / cap                                │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Strength         │ Physical damage                │ ×(1 + STR/100)                                                             │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Dexterity        │ Move speed and haste           │ +1.5 speed per point; cooldowns ×(1 − DEX/200), haste caps at 40% (80 DEX) │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Vitality         │ Max HP                         │ 60 + 12 per point                                                          │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Intelligence     │ Magic damage                   │ ×(1 + INT/100); only Deer's Ranged Shot uses it                            │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Focus            │ Mana pool, mana regen, healing │ Pool 50 + 10/pt, regen 2 + 0.2/s per pt, heals ×(1 + FOC/100)              │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Endurance        │ Rage pool                      │ 50 + 4 per point                                                           │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Ferocity         │ Crit damage                    │ ×(1 + 0.02 × FER); also boosts crit heals                                  │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Stamina Capacity │ Stamina pool and regen         │ Pool 50 + 6/pt, regen 2 + 0.15/s per pt                                    │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Precision        │ Crit chance                    │ 0.5% per point, caps at 60% (120 PRE)                                      │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Ferality         │ Animal Form power              │ +1% per point, Animal Form only; grows at the same rate for every race     │
├──────────────────┼────────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
│ Armor            │ Flat damage reduction          │ Subtracted from every hit; a hit always does at least 1                    │
└──────────────────┴────────────────────────────────┴────────────────────────────────────────────────────────────────────────────┘

- Move speed = race base (195–250) + 1.5 × DEX + buffs.
- Weapon damage is added to an ability's base damage before the Strength multiplier, so it also scales with Strength.
- Damage-taken reductions add together and cap at 80%.

Playable races

Wolf (DPS, Stamina)
- Pack Sense: +3% damage and +4 speed per nearby ally, up to 5 allies.
- Abilities: Bite (heals you for 50% of the damage), Claws, 6, −6 enemy armor), Rend (level 10, ×2.5 damage below 25% HP,plus a bleed).

Bear (Tank, Rage)
- Thick Hide: takes 10% less damage.
- Abilities: Tough Hide, Bite, Hibernate (level 4, heals 20% over 5s), Roar (level 6, taunt), Pounce (level 10, leap that roots nearby enemies).

Deer (Healer, Mana)
- Herd Instinct: a regen aura for nearby allies. It isn't imy has no passive.
- Abilities: Ranged Shot, Mend, Clear Mind (level 4, removes 3 debuffs), Renewal (level 6, area heal plus heal-over-time), Bounty (level 10, restores
  300 resource to nearby allies).

All ten at level 30 (before gear; every race has 54 Ferality

┌────────┬──────────┬─────────────────┬──────────────────────────────┬──────┬───────┬───────┬───────┐
│  Race  │ Playable │   Spec A / B    │                          Passive                          │  HP  │ Speed │ Crit% │ Armor │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Wolf   │ ✓        │ DPS / Tank      │ Pack Sense                                                │ 770  │ 349   │ 33    │ 39    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Bear   │ ✓        │ Tank / Tank     │ Thick Hide                                                │ 1446 │ 244   │ 16    │ 97    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Deer   │ ✓        │ Healer / Healer │ Herd Instinct                                             │ 852  │ 314   │ 25    │ 48    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Hyena  │          │ Tank / DPS      │ Bloodlust (heals when finishing low-HP enemies)           │ 1166 │ 300   │ 26    │ 78    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Fox    │          │ DPS / DPS       │ Keen Senses (sees stealth and traps)                      │ 654  │ 394   │ 46    │ 33    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Hawk   │          │ DPS / DPS       │ Marked Prey (marks a target for the party)                │ 654  │ 380   │ 53    │ 33    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Cat    │          │ DPS / DPS       │ Predator's Pounce (no fall damage, crit bonus on landing) │ 654  │ 390   │ 50    │ 33    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Snake  │          │ DPS / DPS       │ not designed                                              │ 654  │ 324   │ 36    │ 40    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Crow   │          │ DPS / Healer    │ Windborne (flies over terrain, travel only)               │ 572  │ 394   │ 41    │ 26    │
├────────┼──────────┼─────────────────┼──────────────────────────────┼──────┼───────┼───────┼───────┤
│ Rabbit │          │ Healer / Healer │ Startle Reflex (speed burst when hit)                     │ 654  │ 409   │ 30    │ 26    │
└────────┴──────────┴─────────────────┴──────────────────────────────┴──────┴───────┴───────┴───────┘

Best gear stats per race

┌───────────────────────┬─────────────────────────────────┬─
│      Race / spec      │            Priority             │       Skip        │
├───────────────────────┼─────────────────────────────────┼─
│ Wolf DPS              │ STR > PRE > FER > DEX > Stamina │ INT, Focus, END   │
├───────────────────────┼─────────────────────────────────┼─
│ Bear Tank             │ Armor > VIT > END > STR         │ INT, Focus, PRE   │
├───────────────────────┼─────────────────────────────────┼─
│ Deer Healer           │ Focus > DEX > VIT > INT         │ STR, END, Stamina │
├───────────────────────┼─────────────────────────────────┼─
│ Hyena Tank            │ Armor > VIT > STR > END         │ INT, Focus        │
├───────────────────────┼─────────────────────────────────┼─
│ Fox / Hawk / Cat      │ PRE > FER > STR > DEX           │ INT, Focus, END   │
├───────────────────────┼─────────────────────────────────┼─
│ Snake                 │ FER > PRE > STR                 │ Focus, END        │
├───────────────────────┼─────────────────────────────────┼─
│ Crow / Rabbit healers │ Focus > DEX > VIT               │ STR, END          │
└───────────────────────┴─────────────────────────────────┴─

Caps to design around:
- Haste stops at 80 DEX, and most DPS races reach that by level 30 without gear.
- Crit chance stops at 120 PRE. Hawk needs only about 14 Pre
- Ferality gear is useful to every race.

Items (6 examples in items.json)

┌──────┬────────────────┬───────────────────────┬───────────

├──────┼────────────────┼───────────────────────┼────────────────────────────────┤
│ 1001 │ Wolfhide Cap   │ 3 def, +2 VIT         │ Anyone                         │
├──────┼────────────────┼───────────────────────┼───────────
│ 1002 │ Swiftpaw Boots │ 2 def, +4 DEX         │ Wolf, leve
├──────┼────────────────┼───────────────────────┼────────────────────────────────┤
│ 1003 │ Fang Knuckles  │ 6 dmg, +3 STR, +2 PRE │ Wolf, leve
├──────┼────────────────┼───────────────────────┼───────────
│ 1004 │ Bearhide Vest  │ 8 def, +5 VIT, +2 END │ Bear, level 5                  │
├──────┼────────────────┼───────────────────────┼────────────────────────────────┤
│ 1005 │ Feral Totem    │ +6 Ferality, +3 FER   │ Anyone in
├──────┼────────────────┼───────────────────────┼───────────
│ 1101 │ Health Tonic   │ +40 HP, stacks to 20  │ Anyone
└──────┴────────────────┴───────────────────────┴───────────

None of these can be obtained yet: there are no loot tables  no items.json, so items would show as "Unknown Item".

Balance issues

- Armor: a level-30 Bear has 97 flat Armor, while ability hiy hits could drop to 1 damage. Consider percentage-based
  armor, such as armor / (armor + K).
- Intelligence is nearly useless. Let it scale healing or spFocus.
- Endurance only sets the Rage pool size, and Rage drains after 5 seconds. Give it a second job, such as faster rage gain.
- DEX hits the haste cap early, so DEX gear only adds speed

Missing pieces

- Passives for Deer, Hyena, Fox, Hawk, Rabbit and Cat aren't't designed.
- No race has Spec B abilities yet.
- Items need a client items.json, tooltips that show stat bohop.

Ideas

- Race-locked gear: items already have a race_req field, but
- Form-specific bonuses: stats that only apply in Animal Form or only in Human Form.
- Move speed and weapon damage on gear: buffs can already raise these; items can't.
- Set bonuses: e.g. 3 Wolfhide pieces make Pack Sense strong
- On-hit effects: reuse the existing bleed, slow and heal-over-time effects as item triggers.
- Elixirs: consumables that give a timed stat buff.
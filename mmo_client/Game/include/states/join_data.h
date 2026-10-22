#ifndef JOIN_DATA_H
#define JOIN_DATA_H

// Which pieces of the join data the server has delivered so far.
// The packet handlers note what arrives; the loader waits on it. No GL here.

#define JOIN_STATS (1u << 0)
#define JOIN_ABILITIES_HUMAN (1u << 1)
#define JOIN_ABILITIES_ANIMAL (1u << 2)
#define JOIN_ABILITIES (JOIN_ABILITIES_HUMAN | JOIN_ABILITIES_ANIMAL)
#define JOIN_QUESTS (1u << 3)
#define JOIN_FRIENDS (1u << 4)
#define JOIN_FRIEND_REQUESTS (1u << 5)
#define JOIN_ONLINE_LIST (1u << 6)
#define JOIN_PLAYERS (1u << 7)
#define JOIN_NPCS (1u << 8)

// Forget everything seen, at the start of a join
void join_data_reset(void);

// Record that these pieces have arrived
void join_data_note(unsigned flags);

// Nonzero once every one of these pieces has arrived
int join_data_seen(unsigned flags);

// The available-quests list names how many entries follow it
void join_data_available_reset(unsigned expected);

// One entry of the available-quests list arrived
void join_data_available_entry(void);

#endif /* JOIN_DATA_H */

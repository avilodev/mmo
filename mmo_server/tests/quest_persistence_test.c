#define _GNU_SOURCE

#include "quest_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char directory[] = "/tmp/mmo-quest-test-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    quest_system_set_dir(directory);

    PlayerQuestEntry expected[2] = {0};
    expected[0].quest_id = 10;
    expected[0].is_active = 1;
    expected[0].progress[0] = 4;
    expected[1].quest_id = 20;
    expected[1].is_complete = 1;
    expected[1].progress[2] = 7;

    if (!quest_player_save(42, expected, 2)) {
        fprintf(stderr, "quest save failed\n");
        return 1;
    }

    PlayerQuestEntry actual[MAX_PLAYER_QUESTS] = {0};
    int count = quest_player_load(42, actual, MAX_PLAYER_QUESTS);
    if (count != 2 || memcmp(expected, actual, sizeof(expected)) != 0) {
        fprintf(stderr, "quest round trip mismatch\n");
        return 1;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/42.bin", directory);
    FILE* corrupt = fopen(path, "wb");
    int invalid_count = -1;
    if (!corrupt || fwrite(&invalid_count, sizeof(invalid_count), 1, corrupt) != 1 ||
        fclose(corrupt) != 0) {
        return 1;
    }
    memset(actual, 0, sizeof(actual));
    if (quest_player_load(42, actual, MAX_PLAYER_QUESTS) != 0) {
        fprintf(stderr, "corrupt quest file was accepted\n");
        return 1;
    }

    unlink(path);
    rmdir(directory);
    puts("quest_persistence_test: PASS");
    return 0;
}

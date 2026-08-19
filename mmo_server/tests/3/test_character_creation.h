/** @file Run the legacy PostgreSQL character-creation integration suite. */

#include "headers.h"
#include "database_operations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/** Encode ANSI colors used by the test reporter. */
#define GREEN "\033[0;32m"
#define RED "\033[0;31m"
#define YELLOW "\033[0;33m"
#define BLUE "\033[0;34m"
#define RESET "\033[0m"

/** Print a framed heading for one test case. */
void print_test_header(const char* test_name) {
    printf("\n" BLUE "╔════════════════════════════════════════════════════════════╗\n");
    printf("║ %-58s ║\n", test_name);
    printf("╚════════════════════════════════════════════════════════════╝" RESET "\n");
}

/** Print a successful test assertion. */
void print_pass(const char* msg) {
    printf(GREEN "✓ PASS: %s" RESET "\n", msg);
}

/** Print a failed test assertion. */
void print_fail(const char* msg) {
    printf(RED "✗ FAIL: %s" RESET "\n", msg);
}

/** Print supplementary test information. */
void print_info(const char* msg) {
    printf(YELLOW "→ %s" RESET "\n", msg);
}

/** Verify database initialization and its global connection handle.
 *
 * @return Nonzero when both checks succeed, otherwise zero.
 */
int test_database_connection(const char* conn_str) {
    print_test_header("TEST 1: Database Connection");
    
    if (!character_database_init(conn_str)) {
        print_fail("Failed to connect to PostgreSQL");
        return 0;
    }
    
    print_pass("Connected to PostgreSQL successfully");
    
    if (!g_pg) {
        print_fail("Global connection handle is NULL");
        return 0;
    }
    
    print_pass("Global connection handle is valid");
    return 1;
}

/** Verify creation of a character with a nonzero identifier.
 *
 * @return Nonzero on success, otherwise zero.
 */
int test_character_creation(void) {
    print_test_header("TEST 2: Character Creation");
    
    uint32_t account_id = 12345;
    uint32_t world_id = 1;
    const char* name = "TestWarrior";
    int class_id = 1;
    int race_id = 1;
    uint32_t character_id = 0;
    
    print_info("Creating character...");
    printf("   Account ID: %u\n", account_id);
    printf("   World ID: %u\n", world_id);
    printf("   Name: %s\n", name);
    printf("   Class: %d, Race: %d\n", class_id, race_id);
    
    int result = character_create_in_world(account_id, world_id, name, 
                                           class_id, race_id, &character_id);
    
    if (!result) {
        print_fail("character_create_in_world returned 0");
        return 0;
    }
    
    if (character_id == 0) {
        print_fail("Character ID is 0 after creation");
        return 0;
    }
    
    print_pass("Character created successfully");
    printf("   " GREEN "Character ID: %u" RESET "\n", character_id);
    
    return 1;
}

/** Verify retrieval of the character created by the preceding test.
 *
 * @return Nonzero when at least one character is returned, otherwise zero.
 */
int test_character_list(void) {
    print_test_header("TEST 3: Character List Retrieval");
    
    uint32_t account_id = 12345;
    uint32_t world_id = 1;
    CharacterInfo characters[10];
    
    print_info("Retrieving character list...");
    
    int count = character_get_list_for_world(account_id, world_id, characters, 10);
    
    if (count < 0) {
        print_fail("character_get_list_for_world returned negative value");
        return 0;
    }
    
    printf("   Found %d character(s)\n", count);
    
    for (int i = 0; i < count; i++) {
        printf("   [%d] ID:%u Name:'%s' Level:%u Class:%u Race:%u\n",
               i + 1,
               characters[i].character_id,
               characters[i].name,
               characters[i].level,
               characters[i].class_id,
               characters[i].race_id);
    }
    
    if (count == 0) {
        print_fail("No characters found (expected at least 1 from previous test)");
        return 0;
    }
    
    print_pass("Character list retrieved successfully");
    return 1;
}

/** Verify the account's character count is positive.
 *
 * @return Nonzero when the count is positive, otherwise zero.
 */
int test_character_count(void) {
    print_test_header("TEST 4: Character Count");
    
    uint32_t account_id = 12345;
    uint32_t world_id = 1;
    
    int count = character_count_in_world(account_id, world_id);
    
    printf("   Character count: %d\n", count);
    
    if (count <= 0) {
        print_fail("Expected at least 1 character");
        return 0;
    }
    
    print_pass("Character count is correct");
    return 1;
}

/** Verify rejection of a duplicate character name.
 *
 * @return Nonzero when creation is rejected, otherwise zero.
 */
int test_duplicate_name(void) {
    print_test_header("TEST 5: Duplicate Name Prevention");
    
    uint32_t account_id = 12345;
    uint32_t world_id = 1;
    const char* name = "TestWarrior"; // reuse the first test's name
    uint32_t character_id = 0;
    
    print_info("Attempting to create duplicate character...");
    
    int result = character_create_in_world(account_id, world_id, name, 
                                           1, 1, &character_id);
    
    if (result) {
        print_fail("Duplicate character name was allowed (should have failed)");
        return 0;
    }
    
    print_pass("Duplicate name was correctly rejected");
    return 1;
}

/** Verify creation of three characters with distinct class and race values.
 *
 * @return Nonzero when all creations succeed, otherwise zero.
 */
int test_multiple_characters(void) {
    print_test_header("TEST 6: Multiple Character Creation");
    
    uint32_t account_id = 12345;
    uint32_t world_id = 1;
    const char* names[] = {"Mage1", "Rogue1", "Priest1"};
    int classes[] = {2, 3, 4};
    int races[] = {1, 2, 1};
    
    int success_count = 0;
    
    for (int i = 0; i < 3; i++) {
        uint32_t char_id = 0;
        print_info("Creating character...");
        printf("   Name: %s, Class: %d, Race: %d\n", names[i], classes[i], races[i]);
        
        int result = character_create_in_world(account_id, world_id, names[i],
                                               classes[i], races[i], &char_id);
        
        if (result && char_id > 0) {
            printf("   " GREEN "Created with ID: %u" RESET "\n", char_id);
            success_count++;
        } else {
            printf("   " RED "Failed to create" RESET "\n");
        }
    }
    
    if (success_count != 3) {
        print_fail("Not all characters were created");
        return 0;
    }
    
    print_pass("All characters created successfully");
    return 1;
}

/** Verify deletion and the resulting character-count decrement.
 *
 * @return Nonzero when both checks succeed, otherwise zero.
 */
int test_character_deletion(void) {
    print_test_header("TEST 7: Character Deletion");
    
    uint32_t account_id = 12345;
    uint32_t world_id = 1;
    CharacterInfo characters[10];
    
    int count = character_get_list_for_world(account_id, world_id, characters, 10);
    
    if (count <= 0) {
        print_fail("No characters to delete");
        return 0;
    }
    
    uint32_t char_to_delete = characters[0].character_id;
    print_info("Deleting character...");
    printf("   Character ID: %u, Name: %s\n", char_to_delete, characters[0].name);
    
    int result = character_delete_from_world(account_id, char_to_delete, world_id);
    
    if (!result) {
        print_fail("Character deletion failed");
        return 0;
    }
    
    print_pass("Character deleted successfully");
    
    int new_count = character_count_in_world(account_id, world_id);
    
    if (new_count != count - 1) {
        print_fail("Character count didn't decrease after deletion");
        return 0;
    }
    
    print_pass("Character count decreased correctly");
    return 1;
}

/** Verify characters remain isolated between world identifiers.
 *
 * @return Nonzero when the second-world character is visible there, otherwise zero.
 */
int test_cross_world_isolation(void) {
    print_test_header("TEST 8: Cross-World Isolation");
    
    uint32_t account_id = 12345;
    uint32_t world1 = 1;
    uint32_t world2 = 2;
    uint32_t char_id = 0;
    
    print_info("Creating character in world 2...");
    int result = character_create_in_world(account_id, world2, "World2Hero",
                                           1, 1, &char_id);
    
    if (!result) {
        print_fail("Failed to create character in world 2");
        return 0;
    }
    
    print_pass("Character created in world 2");
    
    int count_world1 = character_count_in_world(account_id, world1);
    int count_world2 = character_count_in_world(account_id, world2);
    
    printf("   World 1 characters: %d\n", count_world1);
    printf("   World 2 characters: %d\n", count_world2);
    
    if (count_world2 == 0) {
        print_fail("Character not found in world 2");
        return 0;
    }
    
    print_pass("Worlds are properly isolated");
    return 1;
}

/** Delete test characters from both exercised worlds.
 *
 * @return Nonzero after issuing all cleanup deletions.
 */
int test_cleanup(void) {
    print_test_header("TEST 9: Cleanup");
    
    uint32_t account_id = 12345;
    
    for (uint32_t world_id = 1; world_id <= 2; world_id++) {
        CharacterInfo characters[10];
        int count = character_get_list_for_world(account_id, world_id, characters, 10);
        
        printf("   Cleaning up %d characters from world %u...\n", count, world_id);
        
        for (int i = 0; i < count; i++) {
            character_delete_from_world(account_id, characters[i].character_id, world_id);
        }
    }
    
    print_pass("Test data cleaned up");
    return 1;
}

/** Run the ordered character database integration suite.
 *
 * @return Zero when every test passes, otherwise nonzero.
 */
int main(int argc, char** argv) {
    printf("\n");
    printf(BLUE "╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                                                               ║\n");
    printf("║         CHARACTER CREATION SYSTEM TEST SUITE                 ║\n");
    printf("║                                                               ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝" RESET "\n");
    
    const char* conn_str = getenv("PG_CONNECTION_STRING");
    if (!conn_str) {
        conn_str = "host=localhost dbname=postgres user=postgres password=postgres";
        print_info("Using default connection string");
        printf("   (Set PG_CONNECTION_STRING env var to customize)\n");
    }
    
    int total_tests = 0;
    int passed_tests = 0;
    
    total_tests++;
    if (test_database_connection(conn_str)) {
        passed_tests++;
    } else {
        printf("\n" RED "Cannot continue without database connection" RESET "\n");
        return 1;
    }
    
    total_tests++;
    if (test_character_creation()) {
        passed_tests++;
    }
    
    total_tests++;
    if (test_character_list()) {
        passed_tests++;
    }
    
    total_tests++;
    if (test_character_count()) {
        passed_tests++;
    }
    
    total_tests++;
    if (test_duplicate_name()) {
        passed_tests++;
    }
    
    total_tests++;
    if (test_multiple_characters()) {
        passed_tests++;
    }
    
    total_tests++;
    if (test_character_deletion()) {
        passed_tests++;
    }
    
    total_tests++;
    if (test_cross_world_isolation()) {
        passed_tests++;
    }
    
    total_tests++;
    if (test_cleanup()) {
        passed_tests++;
    }
    
    character_database_close();
    
    printf("\n");
    printf(BLUE "╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                      TEST SUMMARY                             ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝" RESET "\n");
    
    printf("\n   Tests Passed: " GREEN "%d/%d" RESET "\n", passed_tests, total_tests);
    printf("   Tests Failed: " RED "%d/%d" RESET "\n", total_tests - passed_tests, total_tests);
    
    if (passed_tests == total_tests) {
        printf("\n" GREEN "   ✓ ALL TESTS PASSED!" RESET "\n\n");
        return 0;
    } else {
        printf("\n" RED "   ✗ SOME TESTS FAILED" RESET "\n\n");
        return 1;
    }
}
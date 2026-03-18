// ============================================================================
// dialogue_system.c — Core dialogue system implementation
//
// Manages dialogue definitions and active sessions. JSON parsing is handled
// in dialogue_loader.c to keep this file focused on runtime logic.
// ============================================================================

#include "dialogue_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>

// ---------------------------------------------------------------------------
// External functions from dialogue_loader.c
// ---------------------------------------------------------------------------

// Parse one per-NPC JSON file {"id":N,"pages":[...]} into dialogue_table
// Returns 1 on success, 0 on failure
int dialogue_parse_single(const char* json_content, DialogueDef** dialogue_table, int max_dialogues);

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------

static DialogueDef* dialogue_table[MAX_DIALOGUES];   // Indexed by dialogue_id
static int dialogues_loaded = 0;

static DialogueSession sessions[MAX_PLAYERS];        // One session per player slot
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    return buffer;
}

static double get_current_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// ---------------------------------------------------------------------------
// Public API - Initialization & Cleanup
// ---------------------------------------------------------------------------

int dialogue_system_init(const char* dir_path) {
    printf("[DIALOGUE] Loading dialogues from directory: %s\n", dir_path);

    // Clear dialogue table
    memset(dialogue_table, 0, sizeof(dialogue_table));
    dialogues_loaded = 0;

    // Clear all sessions
    pthread_mutex_lock(&sessions_lock);
    memset(sessions, 0, sizeof(sessions));
    pthread_mutex_unlock(&sessions_lock);

    DIR* dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "[DIALOGUE] Failed to open dialogue directory: %s\n", dir_path);
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* fname = entry->d_name;
        size_t len = strlen(fname);
        if (len < 6 || strcmp(fname + len - 5, ".json") != 0)
            continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, fname);

        char* json_content = read_file(filepath);
        if (!json_content) {
            fprintf(stderr, "[DIALOGUE] Could not read file: %s\n", filepath);
            continue;
        }

        if (dialogue_parse_single(json_content, dialogue_table, MAX_DIALOGUES) > 0) {
            dialogues_loaded++;
        }
        free(json_content);
    }

    closedir(dir);
    printf("[DIALOGUE] Successfully loaded %d dialogues\n", dialogues_loaded);
    return 1;
}

void dialogue_system_cleanup(void) {
    for (int i = 0; i < MAX_DIALOGUES; i++) {
        if (dialogue_table[i]) {
            free(dialogue_table[i]);
            dialogue_table[i] = NULL;
        }
    }

    pthread_mutex_lock(&sessions_lock);
    memset(sessions, 0, sizeof(sessions));
    pthread_mutex_unlock(&sessions_lock);

    dialogues_loaded = 0;
    printf("Dialogue system cleaned up\n");
}

// ---------------------------------------------------------------------------
// Public API - Dialogue Lookups
// ---------------------------------------------------------------------------

const DialogueDef* dialogue_get(uint32_t dialogue_id) {
    if (dialogue_id == 0 || dialogue_id >= MAX_DIALOGUES) {
        return NULL;
    }
    return dialogue_table[dialogue_id];
}

int dialogues_get_count(void) {
    return dialogues_loaded;
}

// ---------------------------------------------------------------------------
// Public API - Session Management
// ---------------------------------------------------------------------------

DialogueSession* dialogue_session_create(uint32_t player_id, uint32_t npc_id, uint32_t dialogue_id) {
    if (player_id >= MAX_PLAYERS) {
        fprintf(stderr, "[DIALOGUE] Invalid player_id %u\n", player_id);
        return NULL;
    }

    pthread_mutex_lock(&sessions_lock);

    DialogueSession* session = &sessions[player_id];

    // Close existing session if active
    if (session->is_active) {
        printf("[DIALOGUE] Closing existing session for player %u\n", player_id);
    }

    // Initialize new session
    session->player_id = player_id;
    session->npc_id = npc_id;
    session->dialogue_id = dialogue_id;
    session->current_page = 0;
    session->is_active = 1;
    session->last_interaction_time = get_current_time();

    pthread_mutex_unlock(&sessions_lock);

    printf("[DIALOGUE] Created session: player=%u npc=%u dialogue=%u\n",
           player_id, npc_id, dialogue_id);

    return session;
}

DialogueSession* dialogue_session_get(uint32_t player_id) {
    if (player_id >= MAX_PLAYERS) {
        return NULL;
    }

    pthread_mutex_lock(&sessions_lock);
    DialogueSession* session = &sessions[player_id];

    if (!session->is_active) {
        pthread_mutex_unlock(&sessions_lock);
        return NULL;
    }

    pthread_mutex_unlock(&sessions_lock);
    return session;
}

void dialogue_session_close(uint32_t player_id) {
    if (player_id >= MAX_PLAYERS) {
        return;
    }

    pthread_mutex_lock(&sessions_lock);

    DialogueSession* session = &sessions[player_id];
    if (session->is_active) {
        printf("[DIALOGUE] Closing session for player %u\n", player_id);
        memset(session, 0, sizeof(DialogueSession));
    }

    pthread_mutex_unlock(&sessions_lock);
}

void dialogue_session_update_page(uint32_t player_id, uint8_t new_page) {
    if (player_id >= MAX_PLAYERS) {
        return;
    }

    pthread_mutex_lock(&sessions_lock);

    DialogueSession* session = &sessions[player_id];
    if (session->is_active) {
        session->current_page = new_page;
        session->last_interaction_time = get_current_time();
    }

    pthread_mutex_unlock(&sessions_lock);
}

void dialogue_check_timeouts(void) {
    double now = get_current_time();
    double timeout = 60.0;  // 60 second timeout

    pthread_mutex_lock(&sessions_lock);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        DialogueSession* session = &sessions[i];
        if (session->is_active) {
            double elapsed = now - session->last_interaction_time;
            if (elapsed > timeout) {
                printf("[DIALOGUE] Session timeout for player %u (%.1fs idle)\n",
                       session->player_id, elapsed);
                memset(session, 0, sizeof(DialogueSession));
            }
        }
    }

    pthread_mutex_unlock(&sessions_lock);
}

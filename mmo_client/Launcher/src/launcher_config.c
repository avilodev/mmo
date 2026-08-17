/**
 * @file
 * Load launcher server endpoints from the executable's configuration directory.
 */
#include "launcher_config.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

char g_login_server_ip[64]  = DEFAULT_LOGIN_IP;
int  g_login_server_port     = DEFAULT_LOGIN_PORT;
char g_game_server_ip[64]   = DEFAULT_GAME_IP;
int  g_game_server_port      = DEFAULT_GAME_PORT;

/**
 * Reset server endpoints to defaults and apply values from server.conf when present.
 */
void launcher_config_load(void) {
    // Reset to compiled-in defaults first
    strncpy(g_login_server_ip, DEFAULT_LOGIN_IP, sizeof(g_login_server_ip) - 1);
    g_login_server_port = DEFAULT_LOGIN_PORT;
    strncpy(g_game_server_ip, DEFAULT_GAME_IP, sizeof(g_game_server_ip) - 1);
    g_game_server_port = DEFAULT_GAME_PORT;

    // Build path: <dir of Launcher.exe>\server.conf
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    if (!slash) return;
    *slash = '\0';

    char confPath[MAX_PATH + 16];   // +16 to safely append "\\server.conf"
    snprintf(confPath, sizeof(confPath), "%s\\server.conf", exePath);

    FILE* f = fopen(confPath, "r");
    if (!f) {
        printf("[CONFIG] server.conf not found at %s — using defaults\n", confPath);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline/CR
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        // Skip comments and blank lines
        if (line[0] == '#' || line[0] == '\0') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        if (strcmp(key, "login_ip") == 0) {
            strncpy(g_login_server_ip, val, sizeof(g_login_server_ip) - 1);
            g_login_server_ip[sizeof(g_login_server_ip) - 1] = '\0';
        } else if (strcmp(key, "login_port") == 0) {
            int p = atoi(val);
            if (p >= 1 && p <= 65535) g_login_server_port = p;
        } else if (strcmp(key, "game_ip") == 0) {
            strncpy(g_game_server_ip, val, sizeof(g_game_server_ip) - 1);
            g_game_server_ip[sizeof(g_game_server_ip) - 1] = '\0';
        } else if (strcmp(key, "game_port") == 0) {
            int p = atoi(val);
            if (p >= 1 && p <= 65535) g_game_server_port = p;
        }
    }

    fclose(f);
    printf("[CONFIG] Loaded: login=%s:%d  game=%s:%d\n",
           g_login_server_ip, g_login_server_port,
           g_game_server_ip,  g_game_server_port);
}

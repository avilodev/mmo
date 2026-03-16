#ifndef LAUNCHER_CONFIG_H
#define LAUNCHER_CONFIG_H

// Runtime server configuration loaded from server.conf next to Launcher.exe.
// If the file is missing, the compile-time defaults below are used.
#define DEFAULT_LOGIN_IP   "192.168.1.2"
#define DEFAULT_LOGIN_PORT 7776
#define DEFAULT_GAME_IP    "192.168.1.2"
#define DEFAULT_GAME_PORT  7777

extern char g_login_server_ip[64];
extern int  g_login_server_port;
extern char g_game_server_ip[64];
extern int  g_game_server_port;

// Load (or re-load) server.conf. Safe to call multiple times.
void launcher_config_load(void);

#endif // LAUNCHER_CONFIG_H

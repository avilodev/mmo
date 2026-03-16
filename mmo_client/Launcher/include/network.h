#ifndef NETWORK_H
#define NETWORK_H

#include "protocol.h"
#include "launcher_config.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

// Initialize Winsock
BOOL NetworkInit(void);

// Cleanup Winsock
void NetworkCleanup(void);

BOOL SendLoginRequest(const char* username, const char* password, uint32_t* out_player_id, char* errorMsg, int errorMsgSize);

BOOL SendRegisterRequest(const char* username, const char* password, const char* email, const char* birthday,
                         uint32_t* out_player_id, char* errorMsg, int errorMsgSize);
BOOL SendStartGameRequest(uint32_t player_id, const char* username, char* out_session_key, char* errorMsg, int errorMsgSize);

#endif // NETWORK_H

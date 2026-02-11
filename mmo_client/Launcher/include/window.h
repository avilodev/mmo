#ifndef WINDOW_H
#define WINDOW_H

#include <winsock2.h>
#include <windows.h>

#include "window_types.h"
#include "ui_login.h"
#include "ui_patch_notes.h"

#define TIMER_RECONNECT 2
#define RECONNECT_INTERVAL 10000  // 10 seconds

/*
// Control IDs
#define ID_USERNAME_BOX 100
#define ID_PASSWORD_BOX 101
#define ID_LOGIN_BUTTON 102
#define ID_START_GAME_BUTTON 103
*/

#define ID_STATUS_BAR 1001

// Initialize and create main window
BOOL InitializeWindow(HINSTANCE hInstance);

// Get main window handle
HWND GetMainWindow(void);

// Run message loop
void RunMessageLoop(void);

#endif // WINDOW_H
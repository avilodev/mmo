#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include <winsock2.h>
#include <windows.h>

#include "window_types.h"

#define STATUS_BAR_HEIGHT 30
#define ID_STATUS_BAR 1001

// Create status bar at bottom of window
void CreateStatusBar(HWND hwndParent);

// Update status bar text
void UpdateStatusBar(const char* status);

// Set online/offline status
void SetStatusOnline(void);
void SetStatusOffline(void);

void ShowStatusBar(void);
void HideStatusBar(void);

#endif // UI_STATUS_BAR_H
#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include <winsock2.h>
#include <windows.h>

#include "window_types.h"

/** Define the status-bar height in pixels and its window control ID. */
#define STATUS_BAR_HEIGHT 30
#define ID_STATUS_BAR 1001

void CreateStatusBar(HWND hwndParent);

void SetStatusOffline(void);

void ShowStatusBar(void);

#endif // UI_STATUS_BAR_H

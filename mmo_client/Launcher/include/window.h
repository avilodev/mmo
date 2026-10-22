#ifndef WINDOW_H
#define WINDOW_H

#include <winsock2.h>
#include <windows.h>

#include "window_types.h"
#include "ui_login.h"
#include "ui_patch_notes.h"

/** Identify the main-window status bar. */
#define ID_STATUS_BAR 1001

BOOL InitializeWindow(HINSTANCE hInstance);

HWND GetMainWindow(void);

void RunMessageLoop(void);

#endif // WINDOW_H

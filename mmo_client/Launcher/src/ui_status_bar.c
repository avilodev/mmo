#include "ui_status_bar.h"
#include "window.h"
#include <stdio.h>

static HWND g_hwndStatusBar = NULL;
static HBRUSH g_hStatusBrush = NULL;
 
void CreateStatusBar(HWND hwndParent) {
    // Create dark brush for status bar
    if (g_hStatusBrush == NULL) {
        g_hStatusBrush = CreateSolidBrush(RGB(25, 25, 30));
    }
    
    // Create font for status bar
    HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    
    // Create status bar static text control at bottom (HIDDEN by default)
    RECT rect;
    GetClientRect(hwndParent, &rect);
    
    g_hwndStatusBar = CreateWindowEx(
        0, "STATIC", "Status: Offline",
        WS_CHILD | SS_LEFT | SS_CENTERIMAGE,  // No WS_VISIBLE - start hidden
        0, rect.bottom - STATUS_BAR_HEIGHT,
        rect.right, STATUS_BAR_HEIGHT,
        hwndParent, (HMENU)ID_STATUS_BAR,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    
    SendMessage(g_hwndStatusBar, WM_SETFONT, (WPARAM)hFont, TRUE);
}

void UpdateStatusBar(const char* status) {
    if (g_hwndStatusBar) {
        SetWindowText(g_hwndStatusBar, status);
        InvalidateRect(g_hwndStatusBar, NULL, TRUE);
    }
}

void SetStatusOnline(void) {
    if (g_hwndStatusBar) {
        SetWindowText(g_hwndStatusBar, "Status: Online");
        InvalidateRect(g_hwndStatusBar, NULL, TRUE);
    }
}

void SetStatusOffline(void) {
    if (g_hwndStatusBar) {
        SetWindowText(g_hwndStatusBar, "Status: Offline");
        InvalidateRect(g_hwndStatusBar, NULL, TRUE);
    }
}

void ShowStatusBar(void) {
    if (g_hwndStatusBar) {
        ShowWindow(g_hwndStatusBar, SW_SHOW);
        InvalidateRect(g_hwndStatusBar, NULL, TRUE);
    }
}

void HideStatusBar(void) {
    if (g_hwndStatusBar) {
        ShowWindow(g_hwndStatusBar, SW_HIDE);
    }
}

HBRUSH GetStatusBarBrush(void) {
    return g_hStatusBrush;
}
/**
 * @file
 * Create the launcher window and dispatch its Windows UI messages.
 */
#include <windowsx.h>
#include "window.h"
#include "ui_header.h"
#include "ui_login.h"
#include "ui_status_bar.h"
#include "ui_patch_notes.h"
#include "network.h"
#include "ui_register.h"
#include "launcher_config.h"

static HWND g_hwndMain = NULL;
static HINSTANCE g_hInstance = NULL;

/**
 * Dispatch Windows messages to launcher panels and window behavior.
 *
 * @return      The message-specific result or the default window-procedure result.
 */
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            launcher_config_load();
            CreateHeader(hwnd);
            CreateLoginPanel(hwnd);
            CreateStatusBar(hwnd);
            NetworkInit();  // Initialize Winsock for auth server only
            return 0;
        
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            HWND hControl = (HWND)lParam;
            
            // Check if this is the patch notes text control
            if (GetDlgCtrlID(hControl) == ID_PATCH_NOTES_TEXT) {
                SetTextColor(hdc, RGB(200, 200, 200));
                SetBkColor(hdc, RGB(30, 30, 35));
                
                static HBRUSH hbrPatchNotesBkgnd = NULL;
                if (hbrPatchNotesBkgnd == NULL) {
                    hbrPatchNotesBkgnd = CreateSolidBrush(RGB(30, 30, 35));
                }
                return (LRESULT)hbrPatchNotesBkgnd;
            }

            int ctrlId = GetDlgCtrlID(hControl);
            if (ctrlId == ID_REGISTER_USERNAME_BOX || ctrlId == ID_REGISTER_PASSWORD_BOX) {
                return (LRESULT)HandleRegisterEditControlColor(hwnd, hdc);
            }
            
            return (LRESULT)HandleEditControlColor(hwnd, hdc);
        }
        
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND hControl = (HWND)lParam;
            
            // Check if this is the status bar
            if (GetDlgCtrlID(hControl) == ID_STATUS_BAR) {
                SetTextColor(hdc, RGB(150, 150, 150));
                SetBkColor(hdc, RGB(25, 25, 30));
                SetBkMode(hdc, TRANSPARENT);
                
                static HBRUSH hbrStatusBkgnd = NULL;
                if (hbrStatusBkgnd == NULL) {
                    hbrStatusBkgnd = CreateSolidBrush(RGB(25, 25, 30));
                }
                return (LRESULT)hbrStatusBkgnd;
            }

            // Check if this is the patch notes text control
            if (GetDlgCtrlID(hControl) == ID_PATCH_NOTES_TEXT) {
                SetTextColor(hdc, RGB(200, 200, 200));  // Light gray text
                SetBkColor(hdc, RGB(30, 30, 35));       // Dark gray background
                
                static HBRUSH hbrPatchNotesBkgnd = NULL;
                if (hbrPatchNotesBkgnd == NULL) {
                    hbrPatchNotesBkgnd = CreateSolidBrush(RGB(30, 30, 35));
                }
                return (LRESULT)hbrPatchNotesBkgnd;
            }
            
            // Set text color to light grey
            SetTextColor(hdc, RGB(200, 200, 200));
            // Set background to match window background
            SetBkColor(hdc, RGB(30, 30, 35));
            SetBkMode(hdc, TRANSPARENT);
            
            // Return brush matching the background
            static HBRUSH hbrBkgnd = NULL;
            if (hbrBkgnd == NULL) {
                hbrBkgnd = CreateSolidBrush(RGB(30, 30, 35));
            }
            return (LRESULT)hbrBkgnd;
        }
        
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProc(hwnd, uMsg, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                
                // Check if over close button - allow mouse events for hover effect
                if (IsPointInCloseButton(pt)) {
                    return HTCLIENT;  // Allow mouse events for hover
                }
                
                // Check if over menu items - allow normal mouse events
                int hoveredMenu = GetHoveredMenuItem(pt);
                if (hoveredMenu >= 0) {
                    return HTCLIENT;  // Allow normal mouse events for menu items
                }
                
                // Make rest of header area draggable (but not the menu items or close button)
                if (pt.y < HEADER_HEIGHT && pt.x < WINDOW_WIDTH - 100) {
                    return HTCAPTION;  // Draggable area
                }
            }
            return hit;
        }
        
        case WM_NCLBUTTONDOWN: {
            if (wParam == HTCLOSE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        
        case WM_COMMAND:
            HandleLoginCommand(hwnd, LOWORD(wParam));
            HandlePatchNotesCommand(hwnd, LOWORD(wParam));
            HandleRegisterCommand(hwnd, LOWORD(wParam));

            return 0;

        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            
            // Check if clicking close button
            if (IsPointInCloseButton(pt)) {
                DestroyWindow(hwnd);
                return 0;
            }
            
            HandleHeaderClick(hwnd, pt);
            return 0;
        }

        case WM_DESTROY:
            NetworkCleanup();  // Cleanup Winsock
            PostQuitMessage(0);
            return 0;
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // Dark background
            HBRUSH brush = CreateSolidBrush(RGB(30, 30, 35));
            FillRect(hdc, &ps.rcPaint, brush);
            DeleteObject(brush);
            
            PaintHeader(hdc); 
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_ERASEBKGND:
            return 1;  // Prevent flicker

        case WM_MOUSEMOVE: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            HandleHeaderHover(hwnd, pt);
            
            // Track mouse leave events
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE: {
            // Clear hover when mouse leaves window
            POINT pt = {-1, -1};
            HandleHeaderHover(hwnd, pt);
            return 0;
        }
    }
    
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

/**
 * Register and create the launcher's borderless top-level window.
 *
 * @return      TRUE after successful creation, otherwise FALSE.
 */
BOOL InitializeWindow(HINSTANCE hInstance) {
    g_hInstance = hInstance;

    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    const char CLASS_NAME[] = "LauncherClass";
    
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);  // Prevent white flash
    
    if (!RegisterClass(&wc)) {
        MessageBox(NULL, "Window Registration Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }
    
    // Create borderless window with resize capability
    g_hwndMain = CreateWindowEx(
        WS_EX_APPWINDOW,  // Show in taskbar
        CLASS_NAME,
        "Game Launcher",
        WS_POPUP | WS_VISIBLE,  // Completely borderless
        CW_USEDEFAULT, CW_USEDEFAULT,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL, NULL, hInstance, NULL
    );
    
    if (g_hwndMain == NULL) {
        MessageBox(NULL, "Window Creation Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }
    
    // Remove any default window styles that might show borders
    SetWindowLong(g_hwndMain, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(g_hwndMain, NULL, 0, 0, 0, 0, 
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    
    ShowWindow(g_hwndMain, SW_SHOW);
    UpdateWindow(g_hwndMain);
    
    return TRUE;
}

/**
 * Return the launcher's top-level window handle.
 */
HWND GetMainWindow(void) {
    return g_hwndMain;
}

/**
 * Process Windows messages until a quit message is received.
 *
 * This call blocks for the lifetime of the launcher window.
 */
void RunMessageLoop(void) {
    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

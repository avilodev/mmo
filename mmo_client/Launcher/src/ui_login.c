#include "ui_login.h"
#include "ui_status_bar.h"
#include "network.h"
#include "window.h"

#include <stdio.h>
#include <ctype.h>

static HWND g_hwndUsername = NULL;
static HWND g_hwndPassword = NULL;
static HWND g_hwndLoginButton = NULL;
static HWND g_hwndUsernameLabel = NULL;
static HWND g_hwndPasswordLabel = NULL;
static HWND g_hwndStartGameButton = NULL;
static HWND g_hwndWelcomeLabel = NULL;
static HBRUSH g_hEditBrush = NULL;

// Store session info for game connection
static char g_sessionKey[32] = {0};
static uint32_t g_playerId = 0;

// Store actual password text
static char g_actualPassword[256] = {0};

// Store username for later use
static char g_storedUsername[256] = {0};

// Subclass procedure for username (with multiline)
static WNDPROC g_oldUsernameProc = NULL;
// Subclass procedure for password (with custom masking)
static WNDPROC g_oldPasswordProc = NULL;

LRESULT CALLBACK CustomUsernameProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Prevent Enter key from adding newlines
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        return 0;
    }
    
    if (uMsg == WM_NCPAINT) {
        // Let default paint happen first
        CallWindowProc(g_oldUsernameProc, hwnd, uMsg, wParam, lParam);
        
        // Get window DC for non-client area
        HDC hdc = GetWindowDC(hwnd);
        
        RECT rect;
        GetWindowRect(hwnd, &rect);
        
        // Convert to local coordinates
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        
        // Draw custom darker border
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(35, 35, 40));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        
        // Draw rectangle border
        Rectangle(hdc, 0, 0, width, height);
        
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hPen);
        
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    
    return CallWindowProc(g_oldUsernameProc, hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK CustomPasswordProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Prevent Enter key from adding newlines
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        return 0;
    }
    
    if (uMsg == WM_CHAR) {
        int len = strlen(g_actualPassword);
        
        if (wParam == VK_BACK) {
            // Backspace - remove last character
            if (len > 0) {
                g_actualPassword[len - 1] = '\0';
                
                // Update display with bullets
                char bullets[256];
                memset(bullets, 0, sizeof(bullets));
                for (int i = 0; i < len - 1; i++) {
                    bullets[i] = '*';
                }
                SetWindowText(hwnd, bullets);
                
                // Set cursor to end
                SendMessage(hwnd, EM_SETSEL, len - 1, len - 1);
            }
            return 0;
        } else if (wParam >= 32 && wParam <= 126) {
            // Printable character
            if (len < 255) {
                g_actualPassword[len] = (char)wParam;
                g_actualPassword[len + 1] = '\0';
                
                // Update display with bullets
                char bullets[256];
                memset(bullets, 0, sizeof(bullets));
                for (int i = 0; i <= len; i++) {
                    bullets[i] = '*';
                }
                SetWindowText(hwnd, bullets);
                
                // Set cursor to end
                SendMessage(hwnd, EM_SETSEL, len + 1, len + 1);
            }
            return 0;
        }
        return 0;
    }
    
    if (uMsg == WM_NCPAINT) {
        // Let default paint happen first
        CallWindowProc(g_oldPasswordProc, hwnd, uMsg, wParam, lParam);
        
        // Get window DC for non-client area
        HDC hdc = GetWindowDC(hwnd);
        
        RECT rect;
        GetWindowRect(hwnd, &rect);
        
        // Convert to local coordinates
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        
        // Draw custom darker border
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(35, 35, 40));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        
        // Draw rectangle border
        Rectangle(hdc, 0, 0, width, height);
        
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hPen);
        
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    
    return CallWindowProc(g_oldPasswordProc, hwnd, uMsg, wParam, lParam);
}

void CreateLoginPanel(HWND hwndParent) {
    int margin = 40;
    int entryBoxWidth = 300;
    int entryBoxHeight = 40;
    int spacing = 20;
    
    // Position on LEFT side (below header)
    int xPos = margin;
    int yPosStart = HEADER_HEIGHT + 150;
    
    // Create brush for edit boxes
    if (g_hEditBrush == NULL) {
        g_hEditBrush = CreateSolidBrush(RGB(60, 60, 65));
    }
    
    // Clear password buffer
    memset(g_actualPassword, 0, sizeof(g_actualPassword));
    
    // Create font - slightly larger to help with vertical centering
    HFONT hFont = CreateFont(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    
    // Username label
    g_hwndUsernameLabel = CreateWindowEx(
        0, "STATIC", "Username:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        xPos, yPosStart - 25,
        entryBoxWidth, 20,
        hwndParent, NULL,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    
    HFONT hLabelFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SendMessage(g_hwndUsernameLabel, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
    
    // Username box - Use ES_MULTILINE for vertical centering
    g_hwndUsername = CreateWindowEx(
        0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER | ES_MULTILINE,
        xPos, yPosStart,
        entryBoxWidth, entryBoxHeight,
        hwndParent, (HMENU)ID_USERNAME_BOX,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndUsername, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    // Set formatting rect to vertically center text
    RECT formatRect = {8, 10, entryBoxWidth - 8, entryBoxHeight - 10};
    SendMessage(g_hwndUsername, EM_SETRECT, 0, (LPARAM)&formatRect);
    
    // Subclass for custom border and prevent newlines
    g_oldUsernameProc = (WNDPROC)SetWindowLongPtr(g_hwndUsername, GWLP_WNDPROC, (LONG_PTR)CustomUsernameProc);
    
    // Password label (moved down 10 pixels)
    g_hwndPasswordLabel = CreateWindowEx(
        0, "STATIC", "Password:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        xPos, yPosStart + entryBoxHeight + spacing - 25 + 10,
        entryBoxWidth, 20,
        hwndParent, NULL,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndPasswordLabel, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
    
    // Password box - Use ES_MULTILINE for vertical centering with custom masking
    g_hwndPassword = CreateWindowEx(
        0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER | ES_MULTILINE,
        xPos, yPosStart + entryBoxHeight + spacing + 10,
        entryBoxWidth, entryBoxHeight,
        hwndParent, (HMENU)ID_PASSWORD_BOX,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndPassword, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    // Set formatting rect to vertically center text
    SendMessage(g_hwndPassword, EM_SETRECT, 0, (LPARAM)&formatRect);
    
    // Subclass password field for custom masking and border
    g_oldPasswordProc = (WNDPROC)SetWindowLongPtr(g_hwndPassword, GWLP_WNDPROC, (LONG_PTR)CustomPasswordProc);
    
    // Login button (moved down 10 pixels)
    int buttonHeight = 45;
    int buttonYPos = yPosStart + (2 * entryBoxHeight) + (2 * spacing) + 20 + 10;
    
    g_hwndLoginButton = CreateWindowEx(
        0, "BUTTON", "LOGIN",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        xPos, buttonYPos,
        entryBoxWidth, buttonHeight,
        hwndParent, (HMENU)ID_LOGIN_BUTTON,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    
    HFONT hButtonFont = CreateFont(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SendMessage(g_hwndLoginButton, WM_SETFONT, (WPARAM)hButtonFont, TRUE);

    CreateRegisterLink(hwndParent);
}

void HideLoginPanel(void) {
    if (g_hwndUsername) ShowWindow(g_hwndUsername, SW_HIDE);
    if (g_hwndPassword) ShowWindow(g_hwndPassword, SW_HIDE);
    if (g_hwndLoginButton) ShowWindow(g_hwndLoginButton, SW_HIDE);
    if (g_hwndUsernameLabel) ShowWindow(g_hwndUsernameLabel, SW_HIDE);
    if (g_hwndPasswordLabel) ShowWindow(g_hwndPasswordLabel, SW_HIDE);

    HideRegisterLink();
}

void ShowStartGameButton(HWND hwndParent) {
    int margin = 40;
    int entryBoxWidth = 300;
    int buttonHeight = 45;
    int xPos = margin;
    int yPosStart = HEADER_HEIGHT + 150;
    
    // Get the username
    char username[256];
    GetUsername(username, 256);
    
    // Store username for later use when starting game
    strncpy(g_storedUsername, username, sizeof(g_storedUsername) - 1);
    g_storedUsername[sizeof(g_storedUsername) - 1] = '\0';
    
    // Create welcome label with username - using STATIC control
    g_hwndWelcomeLabel = CreateWindowEx(
        0, "STATIC", username,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        xPos, yPosStart,
        entryBoxWidth, 40,
        hwndParent, NULL,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    
    // Create larger font for username
    HFONT hWelcomeFont = CreateFont(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SendMessage(g_hwndWelcomeLabel, WM_SETFONT, (WPARAM)hWelcomeFont, TRUE);
    
    // Force redraw to apply WM_CTLCOLORSTATIC
    InvalidateRect(hwndParent, NULL, TRUE);
    
    // Create Start Game button below username
    g_hwndStartGameButton = CreateWindowEx(
        0, "BUTTON", "START GAME",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        xPos, yPosStart + 60,
        entryBoxWidth, buttonHeight,
        hwndParent, (HMENU)ID_START_GAME_BUTTON,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    
    HFONT hButtonFont = CreateFont(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SendMessage(g_hwndStartGameButton, WM_SETFONT, (WPARAM)hButtonFont, TRUE);
}

void HandleLoginCommand(HWND hwnd, WORD controlId) {
    if (controlId == ID_LOGIN_BUTTON) {
        char username[256];
        char password[256];
        char errorMsg[512];
        uint32_t playerId = 0;
        
        GetUsername(username, 256);
        GetPassword(password, 256);
        
        // Validate input
        if (strlen(username) == 0) {
            MessageBox(hwnd, "Please enter a username", "Login Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        if (strlen(password) == 0) {
            MessageBox(hwnd, "Please enter a password", "Login Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        // Initialize network if not already done
        if (!NetworkInit()) {
            MessageBox(hwnd, "Failed to initialize network", "Network Error", MB_OK | MB_ICONERROR);
            return;
        }
        
        // Disable button during login
        EnableWindow(g_hwndLoginButton, FALSE);
        SetWindowText(g_hwndLoginButton, "VALIDATING...");
        
        // STAGE 1: Just validate credentials (no session created yet)
        BOOL success = SendLoginRequest(username, password, &playerId, errorMsg, sizeof(errorMsg));
        
        // Re-enable button
        EnableWindow(g_hwndLoginButton, TRUE);
        SetWindowText(g_hwndLoginButton, "LOGIN");
        
        // Show result
        if (success) {
            printf("Login validation successful! Player ID: %u\n", playerId);
            printf("DEBUG: g_playerId set to: %u\n", g_playerId);

            // Wipe password from memory — no longer needed (#20)
            SecureZeroMemory(g_actualPassword, sizeof(g_actualPassword));

            // Store player ID and username for START GAME
            g_playerId = playerId;
            strncpy(g_storedUsername, username, sizeof(g_storedUsername) - 1);
            g_storedUsername[sizeof(g_storedUsername) - 1] = '\0';
            
            // Hide login panel and show "Start Game" button
            HideLoginPanel();
            ShowStartGameButton(hwnd);
            
            // Show status bar (can show offline since not connected to game yet)
            ShowStatusBar();
            SetStatusOffline();  // No connection yet
            
        } else {
            // Show error from server
            MessageBox(hwnd, errorMsg, "Login Failed", MB_OK | MB_ICONERROR);
        }
        
    } else if (controlId == ID_START_GAME_BUTTON) {
        char errorMsg[512];
        char sessionKey[32];
        
        printf("Start Game button clicked!\n");
        
        // Disable button
        EnableWindow(g_hwndStartGameButton, FALSE);
        SetWindowText(g_hwndStartGameButton, "STARTING...");
        
        // STAGE 2: Request session creation
        BOOL success = SendStartGameRequest(g_playerId, g_storedUsername, sessionKey, errorMsg, sizeof(errorMsg));
        
        if (success) {
            printf("Session created successfully!\n");
            
            // Store session key for game launch
            memcpy(g_sessionKey, sessionKey, 32);
            
            // Launch the game immediately
            StartGame(hwnd);
            
            // StartGame() will close the launcher if successful
            // If we get here, launch failed
            EnableWindow(g_hwndStartGameButton, TRUE);
            SetWindowText(g_hwndStartGameButton, "START GAME");
        } else {
            MessageBox(hwnd, errorMsg, "Start Game Failed", MB_OK | MB_ICONERROR);
            EnableWindow(g_hwndStartGameButton, TRUE);
            SetWindowText(g_hwndStartGameButton, "START GAME");
        }
    }
}

void StartGame(HWND hwnd) {
    // Validate username contains only safe characters before inserting into
    // the CreateProcess command line (#8 — prevent argument injection)
    for (int i = 0; g_storedUsername[i] != '\0'; i++) {
        char c = g_storedUsername[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != ' ' && c != '-') {
            MessageBox(hwnd, "Invalid username characters detected.", "Launch Error", MB_OK | MB_ICONERROR);
            return;
        }
    }

    // Build path to Game.exe relative to this launcher's location.
    // Launcher is at <root>\Launcher\bin\Launcher.exe, so strip 3 components.
    char gameExePath[MAX_PATH];
    GetModuleFileNameA(NULL, gameExePath, MAX_PATH);
    for (int strip = 0; strip < 3; strip++) {
        char* last = strrchr(gameExePath, '\\');
        if (last) *last = '\0';
    }
    strncat(gameExePath, "\\Game\\bin\\Game.exe", MAX_PATH - strlen(gameExePath) - 1);
    
    // Convert session key to hex string
    char sessionHex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&sessionHex[i * 2], "%02x", (unsigned char)g_sessionKey[i]);
    }
    sessionHex[64] = '\0';
    
    // Pass session key via environment variable — not visible in process command line
    // (tasklist /v, Process Explorer, etc. expose argv to all local users).
    SetEnvironmentVariableA("MMO_SESSION", sessionHex);

    char commandLine[2048];
    snprintf(commandLine, sizeof(commandLine),
             "\"%s\" --playerid=%u --username=\"%s\" --server=%s:%d",
             gameExePath,
             g_playerId,
             g_storedUsername,
             g_game_server_ip,
             g_game_server_port);

    printf("Launching game: %s\n", commandLine);
    fflush(stdout);

    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    char cmdLineCopy[2048];
    strncpy(cmdLineCopy, commandLine, sizeof(cmdLineCopy) - 1);
    cmdLineCopy[sizeof(cmdLineCopy) - 1] = '\0';

    // Game gets its own console window
    DWORD creationFlags = CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP;

    if (CreateProcess(NULL, cmdLineCopy,
                      NULL,
                      NULL,
                      FALSE,
                      creationFlags,
                      NULL, NULL, &si, &pi)) {
        printf("Game launched with new console!\n");
        printf("Launcher exiting...\n");
        fflush(stdout);
        
        // Close handles immediately - we don't manage the child
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // Clear the session key from our own environment now that the child has inherited it
        SetEnvironmentVariableA("MMO_SESSION", NULL);

        // Small delay so you can see the message
        Sleep(500);
        
        // Exit the launcher cleanly
        PostQuitMessage(0);
    } else {
        DWORD error = GetLastError();
        char errorMsg[512];
        snprintf(errorMsg, sizeof(errorMsg), 
                 "Failed to launch game.\nError code: %ld\n\nMake sure Game.exe exists at:\n%s", 
                 error, gameExePath);
        MessageBox(hwnd, errorMsg, "Launch Error", MB_OK | MB_ICONERROR);
    }
}

void GetUsername(char* buffer, int bufferSize) {
    GetWindowText(g_hwndUsername, buffer, bufferSize);
}

void GetPassword(char* buffer, int bufferSize) {
    // Return the actual password, not the bullets
    snprintf(buffer, bufferSize, "%s", g_actualPassword);
}

HBRUSH HandleEditControlColor(HWND hwnd, HDC hdc) {
    (void)hwnd;
    // Set text color to white
    SetTextColor(hdc, RGB(255, 255, 255));
    // Set background color to match the brush
    SetBkColor(hdc, RGB(60, 60, 65));
    // Return the brush for the background
    return g_hEditBrush;
}

void ShowLoginPanel(void) {
    // Check if user is logged in based on player ID, not button visibility
    // (When switching tabs, buttons get hidden but user is still logged in)
    if (g_playerId != 0) {
        // User is already logged in
        // IMPORTANT: Hide the login form first
        if (g_hwndUsername) ShowWindow(g_hwndUsername, SW_HIDE);
        if (g_hwndPassword) ShowWindow(g_hwndPassword, SW_HIDE);
        if (g_hwndLoginButton) ShowWindow(g_hwndLoginButton, SW_HIDE);
        if (g_hwndUsernameLabel) ShowWindow(g_hwndUsernameLabel, SW_HIDE);
        if (g_hwndPasswordLabel) ShowWindow(g_hwndPasswordLabel, SW_HIDE);
        HideRegisterLink();
        
        // Show the logged-in state
        // Check if welcome label exists - if not, create it
        if (!g_hwndWelcomeLabel || !g_hwndStartGameButton) {
            // Get the main window handle
            HWND hwndParent = GetMainWindow();
            ShowStartGameButton(hwndParent);
        } else {
            // Just show existing controls
            if (g_hwndWelcomeLabel) ShowWindow(g_hwndWelcomeLabel, SW_SHOW);
            if (g_hwndStartGameButton) ShowWindow(g_hwndStartGameButton, SW_SHOW);
        }
    } else {
        // User not logged in
        // IMPORTANT: Hide the logged-in state first
        if (g_hwndWelcomeLabel) ShowWindow(g_hwndWelcomeLabel, SW_HIDE);
        if (g_hwndStartGameButton) ShowWindow(g_hwndStartGameButton, SW_HIDE);
        
        // Show login form
        if (g_hwndUsername) ShowWindow(g_hwndUsername, SW_SHOW);
        if (g_hwndPassword) ShowWindow(g_hwndPassword, SW_SHOW);
        if (g_hwndLoginButton) {
            ShowWindow(g_hwndLoginButton, SW_SHOW);
            // Reset button state in case it was left in "CONNECTING..." state
            EnableWindow(g_hwndLoginButton, TRUE);
            SetWindowText(g_hwndLoginButton, "LOGIN");
        }
        if (g_hwndUsernameLabel) ShowWindow(g_hwndUsernameLabel, SW_SHOW);
        if (g_hwndPasswordLabel) ShowWindow(g_hwndPasswordLabel, SW_SHOW);

        ShowRegisterLink();
    }
}

void HideStartGameButton(void) {
    if (g_hwndWelcomeLabel) ShowWindow(g_hwndWelcomeLabel, SW_HIDE);
    if (g_hwndStartGameButton) ShowWindow(g_hwndStartGameButton, SW_HIDE);
}

void ShowStartGameButtonOnly(void) {
    if (g_hwndWelcomeLabel) ShowWindow(g_hwndWelcomeLabel, SW_SHOW);
    if (g_hwndStartGameButton) ShowWindow(g_hwndStartGameButton, SW_SHOW);
}
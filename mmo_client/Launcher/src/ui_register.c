/**
 * @file
 * Manage launcher account-registration controls and submission.
 */
#include "ui_register.h"
#include "ui_login.h"
#include "ui_status_bar.h"
#include "network.h"
#include "window.h"

#include <stdio.h>

static HWND g_hwndRegUsername = NULL;
static HWND g_hwndRegPassword = NULL;
static HWND g_hwndRegEmail = NULL;
static HWND g_hwndRegBirthday = NULL;
static HWND g_hwndRegisterButton = NULL;
static HWND g_hwndBackToLoginButton = NULL;
static HWND g_hwndRegUsernameLabel = NULL;
static HWND g_hwndRegPasswordLabel = NULL;
static HWND g_hwndRegEmailLabel = NULL;
static HWND g_hwndRegBirthdayLabel = NULL;
static HWND g_hwndShowRegisterButton = NULL;
static HBRUSH g_hRegEditBrush = NULL;

// Store actual password text for registration
static char g_regActualPassword[256] = {0};

// Subclass procedures
static WNDPROC g_oldRegUsernameProc = NULL;
static WNDPROC g_oldRegPasswordProc = NULL;
static WNDPROC g_oldRegEmailProc = NULL;
static WNDPROC g_oldRegBirthdayProc = NULL;

/** Dispatch registration username messages and paint the custom edit border. */
LRESULT CALLBACK CustomRegUsernameProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        return 0;
    }
    
    if (uMsg == WM_NCPAINT) {
        CallWindowProc(g_oldRegUsernameProc, hwnd, uMsg, wParam, lParam);
        
        HDC hdc = GetWindowDC(hwnd);
        RECT rect;
        GetWindowRect(hwnd, &rect);
        
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(35, 35, 40));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        
        Rectangle(hdc, 0, 0, width, height);
        
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hPen);
        
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    
    return CallWindowProc(g_oldRegUsernameProc, hwnd, uMsg, wParam, lParam);
}

/** Dispatch registration password messages while retaining the unmasked value separately. */
LRESULT CALLBACK CustomRegPasswordProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        return 0;
    }
    
    if (uMsg == WM_CHAR) {
        int len = strlen(g_regActualPassword);
        
        if (wParam == VK_BACK) {
            if (len > 0) {
                g_regActualPassword[len - 1] = '\0';
                
                char bullets[256];
                memset(bullets, 0, sizeof(bullets));
                for (int i = 0; i < len - 1; i++) {
                    bullets[i] = '*';
                }
                SetWindowText(hwnd, bullets);
                SendMessage(hwnd, EM_SETSEL, len - 1, len - 1);
            }
            return 0;
        } else if (wParam >= 32 && wParam <= 126) {
            if (len < 255) {
                g_regActualPassword[len] = (char)wParam;
                g_regActualPassword[len + 1] = '\0';
                
                char bullets[256];
                memset(bullets, 0, sizeof(bullets));
                for (int i = 0; i <= len; i++) {
                    bullets[i] = '*';
                }
                SetWindowText(hwnd, bullets);
                SendMessage(hwnd, EM_SETSEL, len + 1, len + 1);
            }
            return 0;
        }
        return 0;
    }
    
    if (uMsg == WM_NCPAINT) {
        CallWindowProc(g_oldRegPasswordProc, hwnd, uMsg, wParam, lParam);
        
        HDC hdc = GetWindowDC(hwnd);
        RECT rect;
        GetWindowRect(hwnd, &rect);
        
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(35, 35, 40));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        
        Rectangle(hdc, 0, 0, width, height);
        
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hPen);
        
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    
    return CallWindowProc(g_oldRegPasswordProc, hwnd, uMsg, wParam, lParam);
}

/** Dispatch registration email messages and paint the custom edit border. */
LRESULT CALLBACK CustomRegEmailProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        return 0;
    }
    
    if (uMsg == WM_NCPAINT) {
        CallWindowProc(g_oldRegEmailProc, hwnd, uMsg, wParam, lParam);
        
        HDC hdc = GetWindowDC(hwnd);
        RECT rect;
        GetWindowRect(hwnd, &rect);
        
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(35, 35, 40));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        
        Rectangle(hdc, 0, 0, width, height);
        
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hPen);
        
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    
    return CallWindowProc(g_oldRegEmailProc, hwnd, uMsg, wParam, lParam);
}

/** Dispatch registration birthday messages and paint the custom edit border. */
LRESULT CALLBACK CustomRegBirthdayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        return 0;
    }
    
    if (uMsg == WM_NCPAINT) {
        CallWindowProc(g_oldRegBirthdayProc, hwnd, uMsg, wParam, lParam);
        
        HDC hdc = GetWindowDC(hwnd);
        RECT rect;
        GetWindowRect(hwnd, &rect);
        
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(35, 35, 40));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        
        Rectangle(hdc, 0, 0, width, height);
        
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hPen);
        
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    
    return CallWindowProc(g_oldRegBirthdayProc, hwnd, uMsg, wParam, lParam);
}

/** Create the login panel's registration navigation control. */
void CreateRegisterLink(HWND hwndParent) {
    int margin = 40;
    int entryBoxWidth = 300;
    int entryBoxHeight = 40;
    int spacing = 20;
    int xPos = margin;
    int yPosStart = HEADER_HEIGHT + 150;
    int buttonHeight = 45;
    int buttonYPos = yPosStart + (2 * entryBoxHeight) + (2 * spacing) + 20 + 10;
    
    // Small "Register" button below login button
    g_hwndShowRegisterButton = CreateWindowEx(
        0, "BUTTON", "Register",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        xPos, buttonYPos + buttonHeight + 10,
        entryBoxWidth, 30,
        hwndParent, (HMENU)ID_SHOW_REGISTER_BUTTON,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    
    HFONT hLinkFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SendMessage(g_hwndShowRegisterButton, WM_SETFONT, (WPARAM)hLinkFont, TRUE);
}

/** Create and subclass the registration form controls. */
void CreateRegisterPanel(HWND hwndParent) {
    int margin = 40;
    int entryBoxWidth = 300;
    int entryBoxHeight = 40;
    int spacing = 15;  // Reduced spacing to fit all fields
    
    int xPos = margin;
    int yPosStart = HEADER_HEIGHT + 100;  // Moved up to fit everything
    
    // Create brush for edit boxes
    if (g_hRegEditBrush == NULL) {
        g_hRegEditBrush = CreateSolidBrush(RGB(60, 60, 65));
    }
    
    // Clear password buffer
    memset(g_regActualPassword, 0, sizeof(g_regActualPassword));
    
    HFONT hFont = CreateFont(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    
    HFONT hLabelFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    
    RECT formatRect = {8, 10, entryBoxWidth - 8, entryBoxHeight - 10};
    
    int currentY = yPosStart;
    
    // Username field
    g_hwndRegUsernameLabel = CreateWindowEx(
        0, "STATIC", "Username:",
        WS_CHILD | SS_LEFT,
        xPos, currentY,
        entryBoxWidth, 20,
        hwndParent, NULL,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndRegUsernameLabel, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
    currentY += 25;
    
    g_hwndRegUsername = CreateWindowEx(
        0, "EDIT", "",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER | ES_MULTILINE,
        xPos, currentY,
        entryBoxWidth, entryBoxHeight,
        hwndParent, (HMENU)ID_REGISTER_USERNAME_BOX,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndRegUsername, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hwndRegUsername, EM_SETRECT, 0, (LPARAM)&formatRect);
    g_oldRegUsernameProc = (WNDPROC)SetWindowLongPtr(g_hwndRegUsername, GWLP_WNDPROC, (LONG_PTR)CustomRegUsernameProc);
    currentY += entryBoxHeight + spacing;
    
    // Password field
    g_hwndRegPasswordLabel = CreateWindowEx(
        0, "STATIC", "Password:",
        WS_CHILD | SS_LEFT,
        xPos, currentY,
        entryBoxWidth, 20,
        hwndParent, NULL,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndRegPasswordLabel, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
    currentY += 25;
    
    g_hwndRegPassword = CreateWindowEx(
        0, "EDIT", "",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER | ES_MULTILINE,
        xPos, currentY,
        entryBoxWidth, entryBoxHeight,
        hwndParent, (HMENU)ID_REGISTER_PASSWORD_BOX,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndRegPassword, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hwndRegPassword, EM_SETRECT, 0, (LPARAM)&formatRect);
    g_oldRegPasswordProc = (WNDPROC)SetWindowLongPtr(g_hwndRegPassword, GWLP_WNDPROC, (LONG_PTR)CustomRegPasswordProc);
    currentY += entryBoxHeight + spacing;
    
    // Email field
    g_hwndRegEmailLabel = CreateWindowEx(
        0, "STATIC", "Email:",
        WS_CHILD | SS_LEFT,
        xPos, currentY,
        entryBoxWidth, 20,
        hwndParent, NULL,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndRegEmailLabel, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
    currentY += 25;
    
    g_hwndRegEmail = CreateWindowEx(
        0, "EDIT", "",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER | ES_MULTILINE,
        xPos, currentY,
        entryBoxWidth, entryBoxHeight,
        hwndParent, (HMENU)ID_REGISTER_EMAIL_BOX,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndRegEmail, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hwndRegEmail, EM_SETRECT, 0, (LPARAM)&formatRect);
    g_oldRegEmailProc = (WNDPROC)SetWindowLongPtr(g_hwndRegEmail, GWLP_WNDPROC, (LONG_PTR)CustomRegEmailProc);
    currentY += entryBoxHeight + spacing;
    
    // Birthday field
    g_hwndRegBirthdayLabel = CreateWindowEx(
        0, "STATIC", "Birthday (YYYY-MM-DD):",
        WS_CHILD | SS_LEFT,
        xPos, currentY,
        entryBoxWidth, 20,
        hwndParent, NULL,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndRegBirthdayLabel, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
    currentY += 25;
    
    g_hwndRegBirthday = CreateWindowEx(
        0, "EDIT", "",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER | ES_MULTILINE,
        xPos, currentY,
        entryBoxWidth, entryBoxHeight,
        hwndParent, (HMENU)ID_REGISTER_BIRTHDAY_BOX,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    SendMessage(g_hwndRegBirthday, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_hwndRegBirthday, EM_SETRECT, 0, (LPARAM)&formatRect);
    g_oldRegBirthdayProc = (WNDPROC)SetWindowLongPtr(g_hwndRegBirthday, GWLP_WNDPROC, (LONG_PTR)CustomRegBirthdayProc);
    currentY += entryBoxHeight + spacing + 10;
    
    // Register button
    int buttonHeight = 45;
    
    g_hwndRegisterButton = CreateWindowEx(
        0, "BUTTON", "REGISTER",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        xPos, currentY,
        entryBoxWidth, buttonHeight,
        hwndParent, (HMENU)ID_REGISTER_BUTTON,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    
    HFONT hButtonFont = CreateFont(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SendMessage(g_hwndRegisterButton, WM_SETFONT, (WPARAM)hButtonFont, TRUE);
    currentY += buttonHeight + 10;
    
    // Back to login button
    g_hwndBackToLoginButton = CreateWindowEx(
        0, "BUTTON", "Back to Login",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        xPos, currentY,
        entryBoxWidth, 30,
        hwndParent, (HMENU)ID_BACK_TO_LOGIN_BUTTON,
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE), NULL
    );
    
    HFONT hBackFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SendMessage(g_hwndBackToLoginButton, WM_SETFONT, (WPARAM)hBackFont, TRUE);
}

/** Clear and show the registration form while hiding login controls. */
void ShowRegisterPanel(void) {
    if (!g_hwndRegUsername) {
        CreateRegisterPanel(GetMainWindow());
    }
    
    // Clear fields
    SetWindowText(g_hwndRegUsername, "");
    SetWindowText(g_hwndRegPassword, "");
    SetWindowText(g_hwndRegEmail, "");
    SetWindowText(g_hwndRegBirthday, "");
    memset(g_regActualPassword, 0, sizeof(g_regActualPassword));
    
    HideLoginPanel();
    HideRegisterLink();
    
    // Show register controls
    if (g_hwndRegUsername) ShowWindow(g_hwndRegUsername, SW_SHOW);
    if (g_hwndRegPassword) ShowWindow(g_hwndRegPassword, SW_SHOW);
    if (g_hwndRegEmail) ShowWindow(g_hwndRegEmail, SW_SHOW);
    if (g_hwndRegBirthday) ShowWindow(g_hwndRegBirthday, SW_SHOW);
    if (g_hwndRegisterButton) ShowWindow(g_hwndRegisterButton, SW_SHOW);
    if (g_hwndBackToLoginButton) ShowWindow(g_hwndBackToLoginButton, SW_SHOW);
    if (g_hwndRegUsernameLabel) ShowWindow(g_hwndRegUsernameLabel, SW_SHOW);
    if (g_hwndRegPasswordLabel) ShowWindow(g_hwndRegPasswordLabel, SW_SHOW);
    if (g_hwndRegEmailLabel) ShowWindow(g_hwndRegEmailLabel, SW_SHOW);
    if (g_hwndRegBirthdayLabel) ShowWindow(g_hwndRegBirthdayLabel, SW_SHOW);
}

/** Hide every registration form control. */
void HideRegisterPanel(void) {
    if (g_hwndRegUsername) ShowWindow(g_hwndRegUsername, SW_HIDE);
    if (g_hwndRegPassword) ShowWindow(g_hwndRegPassword, SW_HIDE);
    if (g_hwndRegEmail) ShowWindow(g_hwndRegEmail, SW_HIDE);
    if (g_hwndRegBirthday) ShowWindow(g_hwndRegBirthday, SW_HIDE);
    if (g_hwndRegisterButton) ShowWindow(g_hwndRegisterButton, SW_HIDE);
    if (g_hwndBackToLoginButton) ShowWindow(g_hwndBackToLoginButton, SW_HIDE);
    if (g_hwndRegUsernameLabel) ShowWindow(g_hwndRegUsernameLabel, SW_HIDE);
    if (g_hwndRegPasswordLabel) ShowWindow(g_hwndRegPasswordLabel, SW_HIDE);
    if (g_hwndRegEmailLabel) ShowWindow(g_hwndRegEmailLabel, SW_HIDE);
    if (g_hwndRegBirthdayLabel) ShowWindow(g_hwndRegBirthdayLabel, SW_HIDE);
}

/** Show the registration navigation control. */
void ShowRegisterLink(void) {
    if (g_hwndShowRegisterButton) ShowWindow(g_hwndShowRegisterButton, SW_SHOW);
}

/** Hide the registration navigation control. */
void HideRegisterLink(void) {
    if (g_hwndShowRegisterButton) ShowWindow(g_hwndShowRegisterButton, SW_HIDE);
}

/** Validate registration fields and dispatch navigation or account creation. */
void HandleRegisterCommand(HWND hwnd, WORD controlId) {
    if (controlId == ID_SHOW_REGISTER_BUTTON) {
        ShowRegisterPanel();
        
    } else if (controlId == ID_BACK_TO_LOGIN_BUTTON) {
        HideRegisterPanel();
        ShowLoginPanel();
        ShowRegisterLink();
        
    } else if (controlId == ID_REGISTER_BUTTON) {
        char username[256];
        char password[256];
        char email[256];
        char birthday[256];
        char errorMsg[512];
        
        // Get all fields
        GetWindowText(g_hwndRegUsername, username, 256);
        strncpy(password, g_regActualPassword, 255);
        password[255] = '\0';
        GetWindowText(g_hwndRegEmail, email, 256);
        GetWindowText(g_hwndRegBirthday, birthday, 256);
        
        // Validate input
        if (strlen(username) == 0) {
            MessageBox(hwnd, "Please enter a username", "Register Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        if (strlen(password) == 0) {
            MessageBox(hwnd, "Please enter a password", "Register Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        if (strlen(password) < 6) {
            MessageBox(hwnd, "Password must be at least 6 characters", "Register Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        if (strlen(email) == 0) {
            MessageBox(hwnd, "Please enter an email address", "Register Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        // Basic email validation
        if (!strchr(email, '@') || !strchr(email, '.')) {
            MessageBox(hwnd, "Please enter a valid email address", "Register Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        if (strlen(birthday) == 0) {
            MessageBox(hwnd, "Please enter your birthday (YYYY-MM-DD)", "Register Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        // Basic birthday format validation
        if (strlen(birthday) != 10 || birthday[4] != '-' || birthday[7] != '-') {
            MessageBox(hwnd, "Birthday must be in YYYY-MM-DD format", "Register Error", MB_OK | MB_ICONWARNING);
            return;
        }
        
        // Initialize network
        if (!NetworkInit()) {
            MessageBox(hwnd, "Failed to initialize network", "Network Error", MB_OK | MB_ICONERROR);
            return;
        }
        
        // Disable button
        EnableWindow(g_hwndRegisterButton, FALSE);
        SetWindowText(g_hwndRegisterButton, "REGISTERING...");
        
        // Send registration request
        uint32_t playerId = 0;
        BOOL success = SendRegisterRequest(username, password, email, birthday, &playerId, errorMsg, sizeof(errorMsg));
        
        // Re-enable button
        EnableWindow(g_hwndRegisterButton, TRUE);
        SetWindowText(g_hwndRegisterButton, "REGISTER");
         
        if (success) {
            char successMsg[512];
            snprintf(successMsg, sizeof(successMsg), 
                     "Registration successful!\nPlayer ID: %u\n\nYou can now log in.", 
                     playerId);
            MessageBox(hwnd, successMsg, "Success", MB_OK | MB_ICONINFORMATION);
            
            // Switch back to login panel
            HideRegisterPanel();
            ShowLoginPanel();
            ShowRegisterLink();
        } else {
            MessageBox(hwnd, errorMsg, "Registration Failed", MB_OK | MB_ICONERROR);
        }
    }
}

/**
 * Configure registration-edit colors and return their background brush.
 */
HBRUSH HandleRegisterEditControlColor(HWND hwnd, HDC hdc) {
    (void)hwnd;
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkColor(hdc, RGB(60, 60, 65));
    return g_hRegEditBrush;
}

#include "ui_header.h"
#include "window.h"
#include <string.h>

typedef struct {
    char* text;
    int x;
    int width;
} MenuItem;

static MenuItem g_menuItems[3];
static int g_menuCount = 0;
static int g_hoveredIndex = -1;
static BOOL g_closeButtonHovered = FALSE;

void CreateHeader(HWND hwndParent) {
    // Initialize menu items (removed Play Guide and Online Store)
    const char* menus[] = {"Home", "Community", "Patch Notes"};
    int startX = 350;
    int spacing = 150;
    
    g_menuCount = 3;
    for (int i = 0; i < g_menuCount; i++) {
        g_menuItems[i].text = (char*)menus[i];
        g_menuItems[i].x = startX + (i * spacing);
        g_menuItems[i].width = 120;
    }
}

int GetHoveredMenuItem(POINT pt) {
    if (pt.y < 0 || pt.y >= HEADER_HEIGHT) return -1;
    
    for (int i = 0; i < g_menuCount; i++) {
        int startX = g_menuItems[i].x - 15;
        int endX = g_menuItems[i].x + g_menuItems[i].width;
        
        if (pt.x >= startX && pt.x <= endX && pt.y >= 10 && pt.y <= HEADER_HEIGHT - 10) {
            return i;
        }
    }
    return -1;
}

void HandleHeaderHover(HWND hwnd, POINT pt) {
    int newHoveredIndex = GetHoveredMenuItem(pt);
    BOOL newCloseHovered = IsPointInCloseButton(pt);
    
    if (newHoveredIndex != g_hoveredIndex || newCloseHovered != g_closeButtonHovered) {
        g_hoveredIndex = newHoveredIndex;
        g_closeButtonHovered = newCloseHovered;
        // Force redraw of header area
        RECT headerRect = {0, 0, WINDOW_WIDTH, HEADER_HEIGHT};
        InvalidateRect(hwnd, &headerRect, FALSE);
        UpdateWindow(hwnd);
    }
}

void PaintHeader(HDC hdc) {
    // Draw header background
    HBRUSH headerBrush = CreateSolidBrush(RGB(25, 25, 30));
    RECT headerRect = {0, 0, WINDOW_WIDTH, HEADER_HEIGHT};
    FillRect(hdc, &headerRect, headerBrush);
    DeleteObject(headerBrush);
    
    // Draw logo text
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);
    
    HFONT hLogoFont = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, "Arial");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hLogoFont);
    
    TextOut(hdc, 20, 20, "Multiverse MMO", 14);
    
    // Draw menu items with hover gradient fade
    HFONT hMenuFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SelectObject(hdc, hMenuFont);
    
    for (int i = 0; i < g_menuCount; i++) {
        // Draw gradient fade background if this item is hovered
        if (i == g_hoveredIndex) {
            RECT hoverRect = {
                g_menuItems[i].x - 20,
                10,
                g_menuItems[i].x + g_menuItems[i].width,
                HEADER_HEIGHT - 10
            };
            
            // Create vertical gradient from transparent to subtle color
            TRIVERTEX vertex[2];
            vertex[0].x = hoverRect.left;
            vertex[0].y = hoverRect.top;
            vertex[0].Red = 60 << 8;
            vertex[0].Green = 60 << 8;
            vertex[0].Blue = 75 << 8;
            vertex[0].Alpha = 0x0000;
            
            vertex[1].x = hoverRect.right;
            vertex[1].y = hoverRect.bottom;
            vertex[1].Red = 45 << 8;
            vertex[1].Green = 45 << 8;
            vertex[1].Blue = 60 << 8;
            vertex[1].Alpha = 0x0000;
            
            GRADIENT_RECT gRect;
            gRect.UpperLeft = 0;
            gRect.LowerRight = 1;
            
            GradientFill(hdc, vertex, 2, &gRect, 1, GRADIENT_FILL_RECT_V);
            
            // Add subtle bottom line accent
            HPEN accentPen = CreatePen(PS_SOLID, 2, RGB(100, 120, 255));
            HPEN oldPen = (HPEN)SelectObject(hdc, accentPen);
            
            MoveToEx(hdc, hoverRect.left + 5, hoverRect.bottom - 2, NULL);
            LineTo(hdc, hoverRect.right - 5, hoverRect.bottom - 2);
            
            SelectObject(hdc, oldPen);
            DeleteObject(accentPen);
            
            SetTextColor(hdc, RGB(255, 255, 255));  // White text on hover
        } else {
            SetTextColor(hdc, RGB(180, 180, 180));  // Normal grey text
        }
        
        SetBkMode(hdc, TRANSPARENT);
        TextOut(hdc, g_menuItems[i].x, 22, g_menuItems[i].text, strlen(g_menuItems[i].text));
    }
    
    // Draw close button area
    int closeX = WINDOW_WIDTH - 40;
    int closeY = 15;
    int closeSize = 30;
    
    // Draw red gradient background when hovered
    if (g_closeButtonHovered) {
        RECT closeRect = {closeX - 5, closeY - 5, closeX + closeSize, closeY + closeSize};
        
        // Create red gradient
        TRIVERTEX vertex[2];
        vertex[0].x = closeRect.left;
        vertex[0].y = closeRect.top;
        vertex[0].Red = 220 << 8;
        vertex[0].Green = 50 << 8;
        vertex[0].Blue = 50 << 8;
        vertex[0].Alpha = 0x0000;
        
        vertex[1].x = closeRect.right;
        vertex[1].y = closeRect.bottom;
        vertex[1].Red = 180 << 8;
        vertex[1].Green = 30 << 8;
        vertex[1].Blue = 30 << 8;
        vertex[1].Alpha = 0x0000;
        
        GRADIENT_RECT gRect;
        gRect.UpperLeft = 0;
        gRect.LowerRight = 1;
        
        GradientFill(hdc, vertex, 2, &gRect, 1, GRADIENT_FILL_RECT_V);
    }
    
    // Draw X symbol
    HPEN closePen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HPEN oldPen = (HPEN)SelectObject(hdc, closePen);
    
    int padding = 8;
    MoveToEx(hdc, closeX + padding, closeY + padding, NULL);
    LineTo(hdc, closeX + closeSize - padding - 5, closeY + closeSize - padding - 5);
    MoveToEx(hdc, closeX + closeSize - padding - 5, closeY + padding, NULL);
    LineTo(hdc, closeX + padding, closeY + closeSize - padding - 5);
    
    SelectObject(hdc, oldPen);
    DeleteObject(closePen);
    
    SelectObject(hdc, hOldFont);
    DeleteObject(hLogoFont);
    DeleteObject(hMenuFont);
}

void HandleHeaderClick(HWND hwnd, POINT pt) {
    if (pt.y > HEADER_HEIGHT) return;
    
    int clickedIndex = GetHoveredMenuItem(pt);
    if (clickedIndex >= 0) {
        // Check which menu item was clicked
        if (strcmp(g_menuItems[clickedIndex].text, "Patch Notes") == 0) {
            // Show patch notes panel
            ShowPatchNotes(hwnd);
        } else if (strcmp(g_menuItems[clickedIndex].text, "Home") == 0) {
            // If patch notes is visible, hide it and show login
            if (IsPatchNotesVisible()) {
                HidePatchNotes();
            }
        } else {
            // For other menu items, show a message
            char msg[100];
            wsprintf(msg, "Clicked: %s", g_menuItems[clickedIndex].text);
            MessageBox(hwnd, msg, "Menu Click", MB_OK);
        }
    }
}

BOOL IsPointInCloseButton(POINT pt) {
    int closeX = WINDOW_WIDTH - 40;
    int closeY = 15;
    int closeSize = 30;
    return (pt.x >= closeX - 5 && pt.x <= closeX + closeSize - 5 &&
            pt.y >= closeY - 5 && pt.y <= closeY + closeSize - 5);
}
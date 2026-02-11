#ifndef UI_HEADER_H
#define UI_HEADER_H

#include <WinSock2.h>
#include <windows.h>
#include "ui_patch_notes.h"

void CreateHeader(HWND hwndParent);
void PaintHeader(HDC hdc);
void HandleHeaderClick(HWND hwnd, POINT pt);
void HandleHeaderHover(HWND hwnd, POINT pt);
BOOL IsPointInCloseButton(POINT pt);
int GetHoveredMenuItem(POINT pt); 

#endif // UI_HEADER_H
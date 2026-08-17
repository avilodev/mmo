#ifndef UI_LOGIN_H
#define UI_LOGIN_H

#include <WinSock2.h>
#include <windows.h>
#include <stdio.h>

#include "ui_register.h"

/** Identify login-panel controls in window command messages. */
#define ID_USERNAME_BOX 1001
#define ID_PASSWORD_BOX 1002
#define ID_LOGIN_BUTTON 1003
#define ID_START_GAME_BUTTON 1004

void CreateLoginPanel(HWND hwndParent);
void HandleLoginCommand(HWND hwnd, WORD controlId);
void GetUsername(char* buffer, int bufferSize);
void GetPassword(char* buffer, int bufferSize);
void ShowStartGameButton(HWND hwndParent);
void HideLoginPanel(void);
void ShowLoginPanel(void);
void HideStartGameButton(void);
void ShowStartGameButtonOnly(void);
void StartGame(HWND hwnd);
HBRUSH HandleEditControlColor(HWND hwnd, HDC hdc);

#endif // UI_LOGIN_H

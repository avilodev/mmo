#ifndef UI_REGISTER_H
#define UI_REGISTER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "protocol.h"

/** Identify registration controls in window command messages. */
#define ID_REGISTER_USERNAME_BOX    2001
#define ID_REGISTER_PASSWORD_BOX    2002
#define ID_REGISTER_EMAIL_BOX       2003
#define ID_REGISTER_BIRTHDAY_BOX    2004
#define ID_REGISTER_BUTTON          2005
#define ID_BACK_TO_LOGIN_BUTTON     2006
#define ID_REGISTER_USERNAME_LABEL  2007
#define ID_REGISTER_PASSWORD_LABEL  2008
#define ID_REGISTER_EMAIL_LABEL     2009
#define ID_REGISTER_BIRTHDAY_LABEL  2010
#define ID_SHOW_REGISTER_BUTTON     2011

void CreateRegisterPanel(HWND hwndParent);
void ShowRegisterPanel(void);
void HideRegisterPanel(void);
void HandleRegisterCommand(HWND hwnd, WORD controlId);
HBRUSH HandleRegisterEditControlColor(HWND hwnd, HDC hdc);

void CreateRegisterLink(HWND hwndParent);
void ShowRegisterLink(void);
void HideRegisterLink(void);

#endif // UI_REGISTER_H

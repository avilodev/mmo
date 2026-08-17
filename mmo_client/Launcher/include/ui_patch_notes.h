#ifndef UI_PATCH_NOTES_H
#define UI_PATCH_NOTES_H

#include <winsock2.h>
#include <windows.h>

#include "window_types.h"
#include "ui_status_bar.h"
#include "ui_header.h"
#include "ui_login.h"

/** Reserve patch-note control IDs outside the registration range. */
#define ID_PATCH_NOTES_PANEL 3001
#define ID_PATCH_NOTES_TEXT  3002
#define ID_BACK_BUTTON       3003

void CreatePatchNotesPanel(HWND hwndParent);
void ShowPatchNotes(HWND hwndParent);
void HidePatchNotes(void);
BOOL IsPatchNotesVisible(void);
void HandlePatchNotesCommand(HWND hwnd, int controlId);

void HideLoginPanel(void);
void ShowLoginPanel(void);

#endif // UI_PATCH_NOTES_H

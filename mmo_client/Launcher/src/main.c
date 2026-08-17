/**
 * @file
 * Start the Windows launcher and run its message loop.
 */
#include <winsock2.h>
#include "window.h"

/**
 * Initialize the launcher window and process Windows messages until shutdown.
 *
 * @return      Zero after normal shutdown, or one when window initialization fails.
 */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    if (!InitializeWindow(hInstance)) {
        return 1;
    }
    
    RunMessageLoop();
    
    return 0;
} 

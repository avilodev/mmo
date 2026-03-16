#include <winsock2.h>
#include "window.h"

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
/**
 * @file
 * Switch the screen into a real exclusive display mode for Fullscreen.
 */
#include "render/display_mode.h"
#include "core/client_log.h"

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>

// The device name of the primary screen, like \\.\DISPLAY1; nonzero when found
static int primary_adapter(wchar_t* name, size_t count) {
	DISPLAY_DEVICEW device;

	for(DWORD i = 0;; i++) {
		memset(&device, 0, sizeof(device));
		device.cb = sizeof(device);

		if(!EnumDisplayDevicesW(NULL, i, &device, 0))
			return 0;

		if(device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
			wcsncpy(name, device.DeviceName, count - 1);
			name[count - 1] = 0;

			return 1;
		}
	}
}

int display_mode_enter(int w, int h, int hz) {
	wchar_t name[32];
	DEVMODEW mode;
	LONG result;

	if(!primary_adapter(name, 32))
		return 0;

	memset(&mode, 0, sizeof(mode));
	mode.dmSize = sizeof(mode);

	if(!EnumDisplaySettingsW(name, ENUM_CURRENT_SETTINGS, &mode))
		return 0;

	mode.dmPelsWidth = (DWORD)w;
	mode.dmPelsHeight = (DWORD)h;
	mode.dmDisplayFrequency = (DWORD)hz;
	mode.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
	result = ChangeDisplaySettingsExW(name, &mode, NULL, CDS_FULLSCREEN, NULL);
	CLOG_INFO("[GAME] Display mode %dx%d @ %dHz: %s (code %ld)", w, h, hz,
	          result == DISP_CHANGE_SUCCESSFUL ? "set" : "refused", (long)result);

	return result == DISP_CHANGE_SUCCESSFUL;
}

void display_mode_leave(void) {
	wchar_t name[32];

	if(primary_adapter(name, 32))
		ChangeDisplaySettingsExW(name, NULL, NULL, 0, NULL);
}

#else

int display_mode_enter(int w, int h, int hz) {
	(void)w;
	(void)h;
	(void)hz;

	return 0;
}

void display_mode_leave(void) {
}

#endif

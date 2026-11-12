/*
 * seatenum - list the monitors, keyboards and mice Windows can see, and show
 * which keyboard or mouse is being used.
 *
 * This is the user-mode discovery half of a Windows multiseat tool.  It does
 * NOT create seats: that needs the kernel components described in DESIGN.md.
 *
 * Build (MinGW):  x86_64-w64-mingw32-gcc -O2 -Wall -municode seatenum.c -o seatenum.exe -luser32
 * Build (MSVC):   cl /W4 /DUNICODE /D_UNICODE seatenum.c user32.lib
 *
 * Usage:  seatenum            list devices
 *         seatenum --watch    print the device behind each key press / mouse move
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define NAME_MAX_CHARS 512

static void list_monitors(void)
{
    DISPLAY_DEVICEW adapter = { .cb = sizeof(adapter) };

    wprintf(L"MONITORS\n");
    for (DWORD a = 0; EnumDisplayDevicesW(NULL, a, &adapter, 0); a++) {
        if (adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)
            continue;

        DISPLAY_DEVICEW monitor = { .cb = sizeof(monitor) };
        for (DWORD m = 0; EnumDisplayDevicesW(adapter.DeviceName, m, &monitor, 0); m++) {
            DEVMODEW mode = { .dmSize = sizeof(mode) };
            BOOL active = (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;

            wprintf(L"  %ls  %ls  on %ls%ls\n", adapter.DeviceName, monitor.DeviceString,
                    adapter.DeviceString,
                    (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? L"  [primary]" : L"");
            if (active && EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
                wprintf(L"      %lux%lu at (%ld,%ld)\n", mode.dmPelsWidth, mode.dmPelsHeight,
                        mode.dmPosition.x, mode.dmPosition.y);
            else
                wprintf(L"      not active\n");
        }
    }
}

/* Device interface path, e.g. \\?\HID#VID_046D&PID_C31C&MI_00#7&...#{guid} */
static BOOL device_name(HANDLE device, WCHAR *name, UINT chars)
{
    UINT size = chars;
    UINT got = GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name, &size);
    if (got == (UINT)-1 || got == 0) {
        name[0] = L'\0';
        return FALSE;
    }
    return TRUE;
}

static const WCHAR *type_name(DWORD type)
{
    switch (type) {
    case RIM_TYPEKEYBOARD: return L"keyboard";
    case RIM_TYPEMOUSE:    return L"mouse";
    default:               return L"hid";
    }
}

static int list_inputs(void)
{
    UINT count = 0;
    if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0) {
        fwprintf(stderr, L"GetRawInputDeviceList failed (error %lu)\n", GetLastError());
        return 1;
    }
    if (count == 0) {
        wprintf(L"INPUT DEVICES\n  none\n");
        return 0;
    }

    RAWINPUTDEVICELIST *list = calloc(count, sizeof(*list));
    if (!list)
        return 1;
    count = GetRawInputDeviceList(list, &count, sizeof(*list));
    if (count == (UINT)-1) {
        fwprintf(stderr, L"GetRawInputDeviceList failed (error %lu)\n", GetLastError());
        free(list);
        return 1;
    }

    wprintf(L"INPUT DEVICES\n");
    for (UINT i = 0; i < count; i++) {
        WCHAR name[NAME_MAX_CHARS];
        if (list[i].dwType == RIM_TYPEHID)
            continue; /* only keyboards and mice can drive a seat */
        device_name(list[i].hDevice, name, NAME_MAX_CHARS);
        wprintf(L"  %-8ls  %ls\n", type_name(list[i].dwType), name[0] ? name : L"(unnamed)");
    }
    free(list);
    return 0;
}

static LRESULT CALLBACK watch_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    static HANDLE last;

    if (message == WM_INPUT) {
        RAWINPUT input;
        UINT size = sizeof(input);
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &input, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            input.header.hDevice != last && input.header.hDevice != NULL) {
            WCHAR name[NAME_MAX_CHARS];
            last = input.header.hDevice;
            device_name(last, name, NAME_MAX_CHARS);
            wprintf(L"%-8ls  %ls\n", type_name(input.header.dwType), name);
            fflush(stdout);
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int watch(void)
{
    WNDCLASSW cls = { .lpfnWndProc = watch_proc, .hInstance = GetModuleHandleW(NULL),
                      .lpszClassName = L"seatenum-watch" };
    if (!RegisterClassW(&cls))
        return 1;
    HWND window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                cls.hInstance, NULL);
    if (!window)
        return 1;

    /* Generic desktop page: usage 6 = keyboard, usage 2 = mouse. */
    RAWINPUTDEVICE wanted[2] = {
        { .usUsagePage = 1, .usUsage = 6, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = window },
        { .usUsagePage = 1, .usUsage = 2, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = window },
    };
    if (!RegisterRawInputDevices(wanted, 2, sizeof(wanted[0]))) {
        fwprintf(stderr, L"RegisterRawInputDevices failed (error %lu)\n", GetLastError());
        return 1;
    }

    wprintf(L"Press a key or move a mouse. Ctrl+C to stop.\n");
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--watch") == 0)
        return watch();
    if (argc != 1) {
        fwprintf(stderr, L"usage: seatenum [--watch]\n");
        return 2;
    }
    list_monitors();
    wprintf(L"\n");
    return list_inputs();
}

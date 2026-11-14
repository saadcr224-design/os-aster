/*
 * Windows 10/11 mode ("keep apart").
 *
 * Windows 10 and 11 run one desktop for the whole PC, so VISION cannot give
 * each workplace a desktop of its own there.  This mode does the next best
 * thing inside that one desktop:
 *   - every program window belongs to the workplace whose keyboard or mouse
 *     was used last when it opened, and is kept on that workplace's screen;
 *   - a window dragged onto another workplace's screen is moved back;
 *   - the pointer stays on the screen of the workplace whose mouse moved last;
 *   - typing on a workplace's keyboard brings that workplace's window back to
 *     the front;
 *   - each screen's taskbar shows only the windows on that screen.
 * People still share one keyboard focus and one pointer, so they take turns.
 */
#include "common.h"

#include <dwmapi.h>
#include <stdio.h>
#include <wchar.h>

#define MAX_TRACKED 1024
#define HOTKEY_STOP 1
#define WM_PLACE (WM_APP + 10)

typedef struct { HWND wnd; int seat; } Tracked;

static Tracked g_win[MAX_TRACKED];
static int g_winCount;
static int g_active;                 /* workplace that used its keyboard or mouse last */
static int g_pointerSeat = -1;       /* workplace the pointer is kept on */
static POINT g_lastPos[MAX_SEATS];
static HWND g_lastFront[MAX_SEATS];
static HWND g_status;
static HWINEVENTHOOK g_hooks[3];
static HFONT g_fHead, g_fText;
static int g_dpi = 96;
static DWORD g_prevTaskbar, g_prevTaskbarMode;
static BOOL g_hadTaskbar, g_hadTaskbarMode, g_test;
static WCHAR g_note[200];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void test_log(const char *fmt, ...)
{
    char path[MAX_PATH];
    if (!g_test || !GetEnvironmentVariableA("OPENSEAT_TEST_W10_LOG", path, MAX_PATH)) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- screens and workplaces ---------------------------------------------- */

static int seat_at(POINT pt)
{
    for (int i = 0; i < g_screenCount; i++)
        if (PtInRect(&g_screens[i].area, pt)) return g_screens[i].seat;
    return NOT_USED;
}

static int seat_of_window(HWND w)
{
    RECT r;
    if (!GetWindowRect(w, &r)) return NOT_USED;
    POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    return seat_at(c);
}

/* The part of the workplace's screen not covered by the taskbar. */
static BOOL seat_work_area(int seat, RECT *out)
{
    int i = seat_first_screen(seat);
    if (i < 0) return FALSE;
    *out = g_screens[i].area;
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromRect(out, MONITOR_DEFAULTTONULL);
    if (mon && GetMonitorInfoW(mon, &mi) && EqualRect(&mi.rcMonitor, out)) *out = mi.rcWork;
    return TRUE;
}

static int seat_of_device(HANDLE device)
{
    for (int i = 0; i < g_deviceCount; i++)
        for (int h = 0; h < g_devices[i].handleCount; h++)
            if (g_devices[i].handles[h] == device) return g_devices[i].seat;
    return NOT_USED - 1; /* unknown device, e.g. plugged in after VISION started */
}

/* ---- which windows to look after ----------------------------------------- */

static BOOL is_app_window(HWND w)
{
    WCHAR cls[64];
    DWORD pid = 0;
    if (!w || !IsWindow(w) || !IsWindowVisible(w) || GetAncestor(w, GA_ROOT) != w) return FALSE;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return FALSE;
    LONG style = GetWindowLongW(w, GWL_STYLE), ex = GetWindowLongW(w, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return FALSE;
    if (!(style & WS_CAPTION) && !(style & WS_SYSMENU)) return FALSE;
    GetClassNameW(w, cls, 64);
    static const WCHAR *shell[] = { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
                                    L"Windows.UI.Core.CoreWindow", L"NotifyIconOverflowWindow", L"#32768" };
    for (size_t i = 0; i < sizeof(shell) / sizeof(shell[0]); i++)
        if (lstrcmpW(cls, shell[i]) == 0) return FALSE;
    BOOL cloaked = FALSE; /* hidden store-app windows */
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return FALSE;
    return TRUE;
}

static Tracked *find_tracked(HWND w)
{
    for (int i = 0; i < g_winCount; i++)
        if (g_win[i].wnd == w) return &g_win[i];
    return NULL;
}

static Tracked *track(HWND w, int seat)
{
    Tracked *t = find_tracked(w);
    if (t) return t;
    if (g_winCount == MAX_TRACKED) { /* forget closed windows */
        int n = 0;
        for (int i = 0; i < g_winCount; i++)
            if (IsWindow(g_win[i].wnd)) g_win[n++] = g_win[i];
        g_winCount = n;
        if (n == MAX_TRACKED) return NULL;
    }
    t = &g_win[g_winCount++];
    t->wnd = w;
    t->seat = seat;
    return t;
}

/* Move a window onto its workplace's screen, keeping it maximized if it was. */
static void place(HWND w, int seat)
{
    RECT dst, r;
    if (seat < 0 || !seat_work_area(seat, &dst) || !GetWindowRect(w, &r)) return;
    if (seat_of_window(w) == seat) return;
    BOOL maxed = IsZoomed(w);
    if (maxed) ShowWindow(w, SW_RESTORE);
    if (maxed || !GetWindowRect(w, &r)) r = dst;
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    int ww = r.right - r.left, wh = r.bottom - r.top;
    if (ww > dw) ww = dw;
    if (wh > dh) wh = dh;
    SetWindowPos(w, NULL, dst.left + (dw - ww) / 2, dst.top + (dh - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    if (maxed) ShowWindow(w, SW_MAXIMIZE);
    WCHAR cls[64];
    GetClassNameW(w, cls, 64);
    test_log("placed %ls on workplace %d", cls, seat + 1);
}

/* ---- events from Windows -------------------------------------------------- */

static void CALLBACK win_event(HWINEVENTHOOK hook, DWORD event, HWND w, LONG obj, LONG child, DWORD thread, DWORD time)
{
    (void)hook; (void)thread; (void)time;
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !is_app_window(w)) return;
    Tracked *t = find_tracked(w);
    if (event == EVENT_OBJECT_SHOW || (!t && event == EVENT_SYSTEM_FOREGROUND)) {
        if (t) return;
        /* A new window: it belongs to the workplace that is working now.
         * A dialog belongs to the workplace of the window that opened it. */
        HWND owner = GetWindow(w, GW_OWNER);
        Tracked *ot = owner ? find_tracked(GetAncestor(owner, GA_ROOT)) : NULL;
        int seat = ot ? ot->seat : g_active;
        t = track(w, seat);
        /* Let the program finish positioning it, then move it. */
        PostMessageW(g_status, WM_PLACE, (WPARAM)w, 0);
    } else if (event == EVENT_SYSTEM_MOVESIZEEND && t) {
        if (seat_of_window(w) != t->seat) {
            lstrcpynW(g_note, L"Windows stay on their own workplace's screen, so that one was moved back.", 200);
            InvalidateRect(g_status, NULL, FALSE);
            place(w, t->seat);
        }
    }
    if (event == EVENT_SYSTEM_FOREGROUND && t && t->seat >= 0) g_lastFront[t->seat] = w;
}

/* Someone used a keyboard or mouse: that workplace is the one working now. */
static void on_input(HRAWINPUT raw)
{
    RAWINPUT in;
    UINT size = sizeof(in);
    if (GetRawInputData(raw, RID_INPUT, &in, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    int seat = seat_of_device(in.header.hDevice);
    if (seat < 0) return; /* switched off or unknown: leave things as they are */
    if (seat != g_active) {
        g_active = seat;
        InvalidateRect(g_status, NULL, FALSE);
    }
    if (in.header.dwType == RIM_TYPEMOUSE && seat != g_pointerSeat) {
        RECT area;
        if (!seat_work_area(seat, &area)) return;
        POINT now;
        if (g_pointerSeat >= 0 && GetCursorPos(&now)) g_lastPos[g_pointerSeat] = now;
        g_pointerSeat = seat;
        RECT full = g_screens[seat_first_screen(seat)].area;
        ClipCursor(&full);
        POINT to = g_lastPos[seat];
        if (!PtInRect(&full, to)) { to.x = (area.left + area.right) / 2; to.y = (area.top + area.bottom) / 2; }
        SetCursorPos(to.x, to.y);
    }
    if (in.header.dwType == RIM_TYPEKEYBOARD) {
        HWND fg = GetForegroundWindow();
        Tracked *t = fg ? find_tracked(fg) : NULL;
        HWND want = g_lastFront[seat];
        if ((!t || t->seat != seat) && want && IsWindow(want) && IsWindowVisible(want))
            SwitchToThisWindow(want, TRUE); /* bring this workplace's own window back to the front */
    }
}

/* ---- taskbar per screen ---------------------------------------------------- */

#define EXPLORER_ADVANCED L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"

static BOOL get_dword(const WCHAR *name, DWORD *out)
{
    DWORD size = sizeof(*out);
    return RegGetValueW(HKEY_CURRENT_USER, EXPLORER_ADVANCED, name, RRF_RT_REG_DWORD, NULL, out, &size) == ERROR_SUCCESS;
}

static void set_taskbars(BOOL apart)
{
    DWORD on = 1, mode = 2; /* taskbar on every screen, showing only that screen's windows */
    if (apart) {
        g_hadTaskbar = get_dword(L"MMTaskbarEnabled", &g_prevTaskbar);
        g_hadTaskbarMode = get_dword(L"MMTaskbarMode", &g_prevTaskbarMode);
        RegSetKeyValueW(HKEY_CURRENT_USER, EXPLORER_ADVANCED, L"MMTaskbarEnabled", REG_DWORD, &on, sizeof(on));
        RegSetKeyValueW(HKEY_CURRENT_USER, EXPLORER_ADVANCED, L"MMTaskbarMode", REG_DWORD, &mode, sizeof(mode));
    } else {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, EXPLORER_ADVANCED, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
        if (g_hadTaskbar) RegSetValueExW(key, L"MMTaskbarEnabled", 0, REG_DWORD, (BYTE *)&g_prevTaskbar, sizeof(DWORD));
        else RegDeleteValueW(key, L"MMTaskbarEnabled");
        if (g_hadTaskbarMode) RegSetValueExW(key, L"MMTaskbarMode", 0, REG_DWORD, (BYTE *)&g_prevTaskbarMode, sizeof(DWORD));
        else RegDeleteValueW(key, L"MMTaskbarMode");
        RegCloseKey(key);
    }
    DWORD_PTR unused;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 1000, &unused);
}

/* ---- status window ----------------------------------------------------------- */

static void stop_keeping_apart(void)
{
    for (int i = 0; i < 3; i++) if (g_hooks[i]) { UnhookWinEvent(g_hooks[i]); g_hooks[i] = NULL; }
    ClipCursor(NULL);
    if (!g_test) set_taskbars(FALSE);
}

static void paint(HWND w, HDC dc)
{
    RECT rc;
    GetClientRect(w, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    RECT t = { S(20), S(14), rc.right - S(20), S(46) };
    SelectObject(dc, g_fHead);
    SetTextColor(dc, RGB(28, 32, 38));
    DrawTextW(dc, L"VISION is keeping workplaces apart", -1, &t, DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, g_fText);
    int y = S(54);
    for (int s = 0; s < g_seatCount; s++) {
        WCHAR line[200];
        int scr = seat_first_screen(s);
        int windows = 0;
        for (int i = 0; i < g_winCount; i++) windows += IsWindow(g_win[i].wnd) && g_win[i].seat == s;
        if (scr < 0) _snwprintf(line, 200, L"Workplace %d  \x00B7  no screen", s + 1);
        else _snwprintf(line, 200, L"Workplace %d  \x00B7  screen %d  \x00B7  %d window%ls%ls", s + 1, scr + 1, windows,
                        windows == 1 ? L"" : L"s", s == g_active ? L"   (working now)" : L"");
        RECT r = { S(20), y, rc.right - S(20), y + S(22) };
        SetTextColor(dc, s == g_active ? RGB(30, 110, 60) : RGB(28, 32, 38));
        DrawTextW(dc, line, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += S(24);
    }
    RECT note = { S(20), y + S(8), rc.right - S(150), rc.bottom - S(8) };
    SetTextColor(dc, RGB(98, 105, 115));
    DrawTextW(dc, g_note[0] ? g_note
                            : L"Each workplace's windows stay on its own screen. Windows 10 has one desktop for the "
                              L"whole PC, so people take turns: the keyboard or mouse used last is the one that works.",
              -1, &note, DT_WORDBREAK | DT_NOPREFIX);
}

static LRESULT CALLBACK status_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        paint(w, dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_INPUT:
        on_input((HRAWINPUT)lp);
        break;
    case WM_PLACE: {
        HWND target = (HWND)wp;
        Tracked *t = find_tracked(target);
        if (t && IsWindow(target)) place(target, t->seat);
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(w, &rc);
        MoveWindow(GetDlgItem(w, IDCANCEL), rc.right - S(130), rc.bottom - S(46), S(110), S(30), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) DestroyWindow(w);
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_STOP) DestroyWindow(w);
        return 0;
    case WM_DISPLAYCHANGE:
        find_hardware(); /* screens moved: keep the seats, update their positions */
        load_setup();
        return 0;
    case WM_DESTROY:
        stop_keeping_apart();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static HFONT font(int points, int weight)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(points, g_dpi, 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

int run_keep_apart(HINSTANCE inst, BOOL test)
{
    g_test = test;
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\VISION.KeepApart");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"VISION is already keeping the workplaces apart.", L"VISION", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    g_fHead = font(14, FW_SEMIBOLD);
    g_fText = font(10, FW_NORMAL);

    find_hardware();
    load_setup();
    int withScreen = 0;
    for (int s = 0; s < g_seatCount; s++) withScreen += seat_first_screen(s) >= 0;
    if (withScreen < 2) {
        MessageBoxW(NULL, L"Give at least two workplaces a screen first.", L"VISION", MB_OK | MB_ICONWARNING);
        return 1;
    }
    for (int s = 1; s < g_seatCount; s++) {
        if (seat_first_screen(s) < 0) continue;
        if (!seat_has(s, DEV_KEYBOARD) || !seat_has(s, DEV_MOUSE)) {
            WCHAR msg[200];
            _snwprintf(msg, 200, L"Workplace %d needs its own keyboard and mouse. Choose them in the list of keyboards and mice, "
                                 L"or use \"Find a device\" to see which is which.", s + 1);
            MessageBoxW(NULL, msg, L"VISION", MB_OK | MB_ICONWARNING);
            return 1;
        }
    }
    WCHAR v[8];
    if (test && GetEnvironmentVariableW(L"OPENSEAT_TEST_ACTIVE", v, 8)) g_active = _wtoi(v) - 1;

    WNDCLASSEXW cls;
    ZeroMemory(&cls, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = status_proc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    cls.lpszClassName = L"VISIONKeepApart";
    RegisterClassExW(&cls);
    RECT a = g_screens[seat_first_screen(0)].area;
    int w = S(560), h = S(170) + g_seatCount * S(24);
    g_status = CreateWindowExW(0, cls.lpszClassName, L"VISION", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               a.left + (a.right - a.left - w) / 2, a.top + (a.bottom - a.top - h) / 2, w, h,
                               NULL, NULL, inst, NULL);
    HWND stop = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_status,
                              (HMENU)(INT_PTR)IDCANCEL, inst, NULL);
    SendMessageW(stop, WM_SETFONT, (WPARAM)g_fText, TRUE);
    RegisterHotKey(g_status, HOTKEY_STOP, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S');

    RAWINPUTDEVICE want[2] = {
        { 1, 6, RIDEV_INPUTSINK, g_status }, /* keyboards */
        { 1, 2, RIDEV_INPUTSINK, g_status }, /* mice */
    };
    RegisterRawInputDevices(want, 2, sizeof(want[0]));
    DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    g_hooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, win_event, 0, 0, flags);
    g_hooks[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, NULL, win_event, 0, 0, flags);
    g_hooks[2] = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, win_event, 0, 0, flags);
    if (!test) set_taskbars(TRUE);

    /* Windows already open belong to the workplace whose screen they are on. */
    for (HWND w2 = GetTopWindow(NULL); w2; w2 = GetWindow(w2, GW_HWNDNEXT))
        if (is_app_window(w2)) {
            int seat = seat_of_window(w2);
            track(w2, seat >= 0 ? seat : 0);
        }

    ShowWindow(g_status, SW_SHOWNORMAL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(single);
    return 0;
}

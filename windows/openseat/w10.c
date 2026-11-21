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
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
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
static FILETIME g_setupTime;
static BOOL g_live[MAX_SEATS]; /* started by the person and able to run */
static BOOL g_auto;
static int g_launchSeat = -1;        /* workplace that just opened something from its desktop */
static DWORD g_launchPid, g_launchUntil;

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

/* Which workplaces run: the ones started in the setup window that have what
 * they need.  Each one is reported back as Running or Error. */
static int update_live(void)
{
    int count = 0;
    for (int s = 0; s < MAX_SEATS; s++) {
        WCHAR why[200];
        BOOL wanted = s < g_seatCount && want_running(s);
        g_live[s] = FALSE;
        if (!wanted) { report_status(s, WS_NOT_STARTED, NULL); continue; }
        if (seat_problem(s, why, 200)) { report_status(s, WS_ERROR, why); continue; }
        g_live[s] = TRUE;
        report_status(s, WS_RUNNING, NULL);
        count++;
    }
    if (g_pointerSeat >= 0 && !g_live[g_pointerSeat]) { g_pointerSeat = -1; ClipCursor(NULL); }
    if (!g_live[g_active]) {
        g_active = 0;
        for (int s = 0; s < MAX_SEATS; s++) if (g_live[s]) { g_active = s; break; }
    }
    return count;
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

/* ---- the PC's desktop icons on every workplace's screen -------------------
 * Windows shows desktop icons only on the main screen.  Each extra workplace
 * gets a desktop of its own that shows the same icons (the user's and the
 * shared desktop folders).  Opening one opens it on that workplace's screen. */

#define MAX_ICONS 300
typedef struct { WCHAR path[MAX_PATH]; WCHAR name[128]; WCHAR args[64]; int image; } DeskItem;

static DeskItem *g_items;
static int g_itemCount;
static HWND g_desk[MAX_SEATS], g_deskList[MAX_SEATS];
static HANDLE g_deskWatch[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
static WCHAR g_deskDir[2][MAX_PATH];

static int by_name(const void *a, const void *b) { return lstrcmpiW(((const DeskItem *)a)->name, ((const DeskItem *)b)->name); }

static void add_item(const WCHAR *path, const WCHAR *args, LPCITEMIDLIST pidl)
{
    if (g_itemCount >= MAX_ICONS) return;
    SHFILEINFOW sfi;
    ZeroMemory(&sfi, sizeof(sfi));
    UINT flags = SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_DISPLAYNAME;
    if (pidl ? !SHGetFileInfoW((LPCWSTR)pidl, 0, &sfi, sizeof(sfi), flags | SHGFI_PIDL)
             : !SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), flags))
        return;
    DeskItem *it = &g_items[g_itemCount++];
    lstrcpynW(it->path, path, MAX_PATH);
    lstrcpynW(it->args, args ? args : L"", 64);
    lstrcpynW(it->name, sfi.szDisplayName, 128);
    it->image = sfi.iIcon;
}

static void load_desktop_items(void)
{
    g_itemCount = 0;
    if (!g_items) g_items = calloc(MAX_ICONS, sizeof(DeskItem));
    if (!g_items) return;
    /* This PC first, as on the main desktop. */
    LPITEMIDLIST pc = NULL;
    if (SUCCEEDED(SHGetSpecialFolderLocation(NULL, CSIDL_DRIVES, &pc)) && pc) {
        add_item(L"explorer.exe", L"shell:MyComputerFolder", pc);
        CoTaskMemFree(pc);
    }
    int fixed = g_itemCount;
    for (int k = 0; k < 2; k++) {
        WCHAR pattern[MAX_PATH + 4], full[MAX_PATH];
        WIN32_FIND_DATAW fd;
        if (!g_deskDir[k][0]) continue;
        _snwprintf(pattern, MAX_PATH + 4, L"%ls\\*", g_deskDir[k]);
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            _snwprintf(full, MAX_PATH, L"%ls\\%ls", g_deskDir[k], fd.cFileName);
            full[MAX_PATH - 1] = 0;
            add_item(full, NULL, NULL);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(g_items + fixed, (size_t)(g_itemCount - fixed), sizeof(DeskItem), by_name);
}

static void fill_desktop(int seat)
{
    HWND list = g_deskList[seat];
    if (!list) return;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (int i = 0; i < g_itemCount; i++) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        it.iItem = i;
        it.pszText = g_items[i].name;
        it.iImage = g_items[i].image;
        it.lParam = i;
        ListView_InsertItem(list, &it);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);
}

/* Open a desktop icon so that its window appears on this workplace's screen. */
static void open_item(int seat, int i)
{
    if (i < 0 || i >= g_itemCount) return;
    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = NULL;
    info.lpFile = g_items[i].path;
    info.lpParameters = g_items[i].args[0] ? g_items[i].args : NULL;
    info.nShow = SW_SHOWNORMAL;
    g_active = seat;
    g_launchSeat = seat;
    g_launchPid = 0;
    g_launchUntil = GetTickCount() + 15000;
    if (ShellExecuteExW(&info)) {
        if (info.hProcess) { g_launchPid = GetProcessId(info.hProcess); CloseHandle(info.hProcess); }
        test_log("opened %ls from workplace %d", g_items[i].name, seat + 1);
    }
    InvalidateRect(g_status, NULL, FALSE);
}

static LRESULT CALLBACK desk_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    int seat = (int)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(w, &rc);
        MoveWindow(g_deskList[seat], 0, 0, rc.right, rc.bottom - S(30), TRUE);
        InvalidateRect(w, NULL, TRUE);
        return 0;
    }
    case WM_PAINT: { /* a strip along the bottom saying whose desktop this is */
        PAINTSTRUCT ps;
        RECT rc, strip;
        HDC dc = BeginPaint(w, &ps);
        GetClientRect(w, &rc);
        strip = rc;
        strip.top = rc.bottom - S(30);
        HBRUSH bg = CreateSolidBrush(RGB(10, 30, 70));
        FillRect(dc, &strip, bg);
        DeleteObject(bg);
        WCHAR label[NAME_CHARS + 40];
        seat_name(seat, label, NAME_CHARS);
        wcscat(label, L"  \x00B7  VISION");
        strip.right -= S(14);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 215, 240));
        SelectObject(dc, g_fText);
        DrawTextW(dc, label, -1, &strip, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->hwndFrom == g_deskList[seat] && (n->code == LVN_ITEMACTIVATE || n->code == NM_DBLCLK)) {
            int i = ListView_GetNextItem(g_deskList[seat], -1, LVNI_SELECTED);
            if (i < 0 && n->code == NM_DBLCLK) i = ((NMITEMACTIVATE *)lp)->iItem;
            LVITEMW it;
            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_PARAM;
            it.iItem = i;
            if (i >= 0 && ListView_GetItem(g_deskList[seat], &it)) open_item(seat, (int)it.lParam);
            return 0;
        }
        break;
    }
    case WM_WINDOWPOSCHANGING: { /* stay behind every program window, like the real desktop */
        WINDOWPOS *pos = (WINDOWPOS *)lp;
        HWND shell = FindWindowW(L"Progman", NULL);
        HWND above = shell ? GetWindow(shell, GW_HWNDPREV) : NULL;
        if (!(pos->flags & SWP_NOZORDER)) pos->hwndInsertAfter = above && above != w ? above : HWND_BOTTOM;
        break;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* Make sure every started extra workplace has its desktop, on its own screen. */
static void sync_desktops(void)
{
    for (int s = 1; s < MAX_SEATS; s++) {
        RECT area;
        BOOL want = g_live[s] && seat_work_area(s, &area);
        if (!want) {
            if (g_desk[s]) { DestroyWindow(g_desk[s]); g_desk[s] = g_deskList[s] = NULL; }
            continue;
        }
        if (!g_desk[s]) {
            HINSTANCE inst = GetModuleHandleW(NULL);
            g_desk[s] = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"VISIONDesktop", L"VISION desktop",
                                        WS_POPUP | WS_CLIPCHILDREN, area.left, area.top, 10, 10, NULL, NULL, inst, NULL);
            if (!g_desk[s]) continue;
            SetWindowLongPtrW(g_desk[s], GWLP_USERDATA, s);
            g_deskList[s] = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                            WS_CHILD | WS_VISIBLE | LVS_ICON | LVS_AUTOARRANGE | LVS_SINGLESEL |
                                                LVS_SHAREIMAGELISTS | LVS_ALIGNLEFT, /* columns down the left, like the Windows desktop */
                                            0, 0, 10, 10, g_desk[s], NULL, inst, NULL);
            SHFILEINFOW sfi;
            HIMAGELIST images = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
            ListView_SetImageList(g_deskList[s], images, LVSIL_NORMAL);
            ListView_SetBkColor(g_deskList[s], RGB(18, 52, 110));
            ListView_SetTextBkColor(g_deskList[s], CLR_NONE);
            ListView_SetTextColor(g_deskList[s], RGB(255, 255, 255));
            ListView_SetIconSpacing(g_deskList[s], S(96), S(92));
            SendMessageW(g_deskList[s], WM_SETFONT, (WPARAM)g_fText, TRUE);
            fill_desktop(s);
        }
        RECT now;
        GetWindowRect(g_desk[s], &now);
        if (!EqualRect(&now, &area))
            SetWindowPos(g_desk[s], NULL, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        if (!IsWindowVisible(g_desk[s])) SetWindowPos(g_desk[s], HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}

/* Icons added to or removed from the desktop show up on every workplace. */
static void watch_desktop_changes(void)
{
    BOOL changed = FALSE;
    for (int k = 0; k < 2; k++) {
        if (g_deskWatch[k] == INVALID_HANDLE_VALUE) continue;
        if (WaitForSingleObject(g_deskWatch[k], 0) == WAIT_OBJECT_0) { changed = TRUE; FindNextChangeNotification(g_deskWatch[k]); }
    }
    if (!changed) return;
    load_desktop_items();
    for (int s = 1; s < MAX_SEATS; s++) fill_desktop(s);
}

static void start_desktops(void)
{
    WNDCLASSEXW cls;
    ZeroMemory(&cls, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = desk_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = CreateSolidBrush(RGB(18, 52, 110));
    cls.lpszClassName = L"VISIONDesktop";
    RegisterClassExW(&cls);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    int folders[2] = { CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY };
    for (int k = 0; k < 2; k++) {
        if (FAILED(SHGetFolderPathW(NULL, folders[k], NULL, SHGFP_TYPE_CURRENT, g_deskDir[k]))) g_deskDir[k][0] = 0;
        if (g_deskDir[k][0])
            g_deskWatch[k] = FindFirstChangeNotificationW(g_deskDir[k], FALSE,
                                                          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
    }
    load_desktop_items();
    sync_desktops();
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
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (!ot && g_launchSeat >= 0 && (int)(g_launchUntil - GetTickCount()) > 0 && (pid == g_launchPid || !g_launchPid))
            seat = g_launchSeat; /* opened from that workplace's desktop icons */
        if (seat < 0 || !g_live[seat]) return; /* that workplace is not started: leave the window alone */
        t = track(w, seat);
        /* Let the program finish positioning it, then move it. */
        PostMessageW(g_status, WM_PLACE, (WPARAM)w, 0);
    } else if (event == EVENT_SYSTEM_MOVESIZEEND && t && t->seat >= 0 && g_live[t->seat]) {
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
    if (seat < 0 || !g_live[seat]) return; /* not started, switched off or unknown: leave things as they are */
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

/* ---- follow changes made in the setup window ---------------------------- */

static FILETIME setup_time(void)
{
    WCHAR path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA d;
    FILETIME none = { 0, 0 };
    setup_file(L"setup.ini", path, MAX_PATH);
    return GetFileAttributesExW(path, GetFileExInfoStandard, &d) ? d.ftLastWriteTime : none;
}

/* Saved in the setup window, or screens changed: use the new layout at once.
 * Windows keep their workplace. */
static void reload_setup(void)
{
    g_setupTime = setup_time();
    find_hardware();
    load_setup();
    if (g_active >= g_seatCount) g_active = 0;
    update_live();
    if (g_pointerSeat >= g_seatCount || (g_pointerSeat >= 0 && seat_first_screen(g_pointerSeat) < 0)) {
        g_pointerSeat = -1;
        ClipCursor(NULL);
    }
    for (int i = 0; i < g_winCount; i++)
        if (IsWindow(g_win[i].wnd) && g_win[i].seat >= 0 && g_win[i].seat < g_seatCount && g_live[g_win[i].seat])
            place(g_win[i].wnd, g_win[i].seat);
    lstrcpynW(g_note, L"The new setup is in use.", 200);
    InvalidateRect(g_status, NULL, FALSE);
}

/* ---- status window ----------------------------------------------------------- */

static void stop_keeping_apart(void)
{
    for (int s = 0; s < MAX_SEATS; s++) if (g_desk[s]) { DestroyWindow(g_desk[s]); g_desk[s] = g_deskList[s] = NULL; }
    for (int k = 0; k < 2; k++)
        if (g_deskWatch[k] != INVALID_HANDLE_VALUE) { FindCloseChangeNotification(g_deskWatch[k]); g_deskWatch[k] = INVALID_HANDLE_VALUE; }
    for (int s = 0; s < MAX_SEATS; s++) report_status(s, WS_NOT_STARTED, NULL);
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
        WCHAR label[NAME_CHARS];
        seat_name(s, label, NAME_CHARS);
        BOOL ownDevices = s == 0 || (seat_has(s, DEV_KEYBOARD) && seat_has(s, DEV_MOUSE));
        if (!g_live[s])
            _snwprintf(line, 200, L"%ls  \x00B7  %ls", label,
                       s > 0 && !(g_share[s] & SHARE_ON) ? L"switched off in Connections and sharing"
                       : scr < 0                          ? L"no screen yet: give it one in VISION"
                                                          : L"stopped: press its Start in VISION");
        else if (scr < 0) _snwprintf(line, 200, L"%ls  \x00B7  no screen", label);
        else _snwprintf(line, 200, L"%ls  \x00B7  screen %d  \x00B7  %d window%ls%ls", label, scr + 1, windows,
                        windows == 1 ? L"" : L"s",
                        s == g_active ? L"   (working now)" : !ownDevices ? L"   (no own keyboard and mouse yet)" : L"");
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
        reload_setup(); /* screens moved: keep the seats, update their positions */
        return 0;
    case WM_TIMER: {
        FILETIME now = setup_time();
        if (CompareFileTime(&now, &g_setupTime) != 0) reload_setup();
        BOOL before[MAX_SEATS], changedLive = FALSE, anyWanted = FALSE;
        memcpy(before, g_live, sizeof(before));
        update_live();
        for (int s = 0; s < MAX_SEATS; s++) {
            if (before[s] != g_live[s]) changedLive = TRUE;
            if (s < g_seatCount && want_running(s)) anyWanted = TRUE;
        }
        if (changedLive) {
            /* A workplace was just started: put its windows where they belong. */
            for (int i = 0; i < g_winCount; i++)
                if (IsWindow(g_win[i].wnd) && g_win[i].seat >= 0 && g_live[g_win[i].seat]) place(g_win[i].wnd, g_win[i].seat);
            InvalidateRect(w, NULL, FALSE);
        }
        sync_desktops();
        watch_desktop_changes();
        if (!anyWanted) DestroyWindow(w); /* every workplace was stopped */
        return 0;
    }
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

int run_keep_apart(HINSTANCE inst, BOOL test, BOOL automatic)
{
    g_test = test;
    g_auto = automatic;
    if (automatic && !test && is_server_os()) return 0; /* Windows Server uses separate sign-ins instead */
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\VISION.KeepApart");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* It is already running and picks up saved changes by itself. */
        HWND w = FindWindowW(L"VISIONKeepApart", NULL);
        if (w && !automatic) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        return 0;
    }
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    g_fHead = font(14, FW_SEMIBOLD);
    g_fText = font(10, FW_NORMAL);

    find_hardware();
    load_setup();
    /* Started with Windows: carry on with the workplaces that were running
     * before; if none were, there is nothing to do. */
    if (!update_live() && automatic) {
        BOOL any = FALSE;
        for (int s2 = 0; s2 < g_seatCount; s2++) any |= want_running(s2);
        if (!any) return 0;
    }
    WCHAR v[8];
    if (test && GetEnvironmentVariableW(L"OPENSEAT_TEST_ACTIVE", v, 8)) g_active = _wtoi(v) - 1;
    if (g_active < 0 || g_active >= MAX_SEATS) g_active = 0;

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

    g_setupTime = setup_time();
    SetTimer(g_status, 1, 700, NULL);
    /* Started with Windows: stay out of the way, as a button on the taskbar. */
    ShowWindow(g_status, automatic ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    start_desktops();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(single);
    return 0;
}

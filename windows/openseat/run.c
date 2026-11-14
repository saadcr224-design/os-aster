/*
 * The controller: runs the extra workplaces.
 *
 * How it works
 *   - Each extra workplace is a separate Windows sign-in (a Remote Desktop
 *     session on this same PC), shown full screen on that workplace's monitor
 *     by the built-in Remote Desktop client (mstsc).
 *   - The Interception filter driver lets this program see which keyboard or
 *     mouse every key press and movement comes from.  Input from workplace 1's
 *     devices passes through untouched.  Input from another workplace's
 *     devices is taken out of the main desktop and sent over a pipe to
 *     VISION's agent inside that workplace's session, which replays it.
 *   - The mstsc windows are disabled, so workplace 1's keyboard and mouse can
 *     never reach them, and workplace 1's pointer is kept on its own screens.
 *
 * If this program stops or crashes, the driver passes all input to the main
 * desktop again, so the PC is never left without a working keyboard.
 */
#include "common.h"

#include <wtsapi32.h>
#include <lm.h>
#include <wincred.h>
#include <sddl.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define WM_IDENTIFIED (WM_APP + 1)
#define WM_REFRESH (WM_APP + 2)
#define WM_FATAL (WM_APP + 3)
#define MAX_DEV 20
#define HOTKEY_STOP 1
#define INTERCEPTION_URL L"https://github.com/oblitum/Interception/releases/download/v1.0.1/Interception.zip"

/* ---- Interception, loaded at run time ------------------------------------ */

typedef void *IContext;
typedef int (*IPredicate)(int device);
typedef struct { unsigned short code, state; unsigned int information; } IKey;
typedef struct { unsigned short state, flags; short rolling; int x, y; unsigned int information; } IMouse;
typedef union { IKey key; IMouse mouse; } IStroke;

static struct {
    HMODULE dll;
    IContext (*create)(void);
    void (*destroy)(IContext);
    void (*set_filter)(IContext, IPredicate, unsigned short);
    int (*wait_timeout)(IContext, unsigned long);
    int (*send)(IContext, int, const void *, unsigned int);
    int (*receive)(IContext, int, void *, unsigned int);
    unsigned int (*hardware_id)(IContext, int, void *, unsigned int);
    int (*is_keyboard)(int);
    int (*is_mouse)(int);
} ic;

static BOOL load_proc(void *field, size_t size, const char *name)
{
    FARPROC f = GetProcAddress(ic.dll, name);
    memcpy(field, &f, size);
    return f != NULL;
}

static BOOL load_interception(const WCHAR *dir)
{
    WCHAR path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\interception.dll", dir);
    ic.dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ic.dll) return FALSE;
#define LOAD(field, name) load_proc((void *)&ic.field, sizeof(ic.field), name)
    BOOL ok = LOAD(create, "interception_create_context") && LOAD(destroy, "interception_destroy_context") &&
              LOAD(set_filter, "interception_set_filter") && LOAD(wait_timeout, "interception_wait_with_timeout") &&
              LOAD(send, "interception_send") && LOAD(receive, "interception_receive") &&
              LOAD(hardware_id, "interception_get_hardware_id") && LOAD(is_keyboard, "interception_is_keyboard") &&
              LOAD(is_mouse, "interception_is_mouse");
#undef LOAD
    if (!ok) { FreeLibrary(ic.dll); ic.dll = NULL; }
    return ok;
}

/* ---- state ------------------------------------------------------------------ */

typedef struct {
    BOOL active;
    int screen;
    HANDLE process;
    DWORD pid, startedAt, nextLaunch;
    int quickExits;
    HWND window;
    HANDLE pipe;
    HANDLE writeEvent;
    BOOL needKeyboard, needMouse;
    WCHAR state[200];
    BOOL bad;
} Seat;

static Seat g_seat[MAX_SEATS];
static int g_route[MAX_DEV + 1];   /* Interception device -> workplace (0 = main desktop) */
static BOOL g_known[MAX_DEV + 1];  /* matched from the saved setup, never captured by "press a key" */
static WCHAR g_hw[MAX_DEV + 1][400];
static int g_identifySeat = -1, g_identifyKind;
static CRITICAL_SECTION g_lock;
static volatile LONG g_running = 1;
static IContext g_ctx;
static HWND g_main, g_prompt, g_stopButton;
static HANDLE g_router, g_server;
static BOOL g_test, g_launched;
static RECT g_clip;
static WCHAR g_installDir[MAX_PATH], g_exeDir[MAX_PATH];
static HFONT g_fBig, g_fHead, g_fText;
static int g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void say(const WCHAR *text, UINT icon)
{
    MessageBoxW(g_main, text, L"VISION", MB_OK | icon | MB_SETFOREGROUND);
}

static void set_state(int seat, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g_seat[seat].state, 200, fmt, ap);
    va_end(ap);
    g_seat[seat].state[199] = 0;
    if (g_main) InvalidateRect(g_main, NULL, FALSE);
}

static void test_log(const char *fmt, ...)
{
    char path[MAX_PATH];
    if (!g_test || !GetEnvironmentVariableA("OPENSEAT_TEST_CONTROLLER_LOG", path, MAX_PATH)) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- sending input to a workplace's agent --------------------------------- */

static void drop_pipe(int seat)
{
    if (!g_seat[seat].pipe) return;
    CancelIoEx(g_seat[seat].pipe, NULL);
    CloseHandle(g_seat[seat].pipe);
    g_seat[seat].pipe = NULL;
    PostMessageW(g_main, WM_REFRESH, 0, 0);
}

/* Called with g_lock held.  Never blocks for long: a stuck agent must not
 * freeze workplace 1's keyboard. */
static void forward(int seat, BOOL isKey, const IStroke *s)
{
    Seat *st = &g_seat[seat];
    SeatEvent e;
    ZeroMemory(&e, sizeof(e));
    e.magic = EVENT_MAGIC;
    if (isKey) {
        e.kind = EVENT_KEY;
        e.code = s->key.code;
        e.state = s->key.state;
    } else {
        e.kind = EVENT_MOUSE;
        e.state = s->mouse.state;
        e.flags = s->mouse.flags;
        e.rolling = s->mouse.rolling;
        e.x = s->mouse.x;
        e.y = s->mouse.y;
    }
    if (!st->pipe) return;
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = st->writeEvent;
    ResetEvent(ov.hEvent);
    DWORD done = 0;
    if (!WriteFile(st->pipe, &e, sizeof(e), NULL, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) { drop_pipe(seat); return; }
        if (WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0) {
            CancelIoEx(st->pipe, &ov);
            GetOverlappedResult(st->pipe, &ov, &done, TRUE);
            drop_pipe(seat);
            return;
        }
    }
    if (!GetOverlappedResult(st->pipe, &ov, &done, FALSE) || done != sizeof(e)) drop_pipe(seat);
}

static DWORD WINAPI router_thread(LPVOID unused)
{
    (void)unused;
    IStroke s;
    while (g_running) {
        int dev = ic.wait_timeout(g_ctx, 250);
        if (dev <= 0) continue;
        if (ic.receive(g_ctx, dev, &s, 1) <= 0) continue;
        if (dev > MAX_DEV) { ic.send(g_ctx, dev, &s, 1); continue; }
        BOOL isKey = ic.is_keyboard(dev) != 0;

        EnterCriticalSection(&g_lock);
        int seat = g_route[dev];
        if (g_identifySeat > 0 && seat == 0 && !g_known[dev]) {
            BOOL keyDown = isKey && !(s.key.state & IK_UP);
            BOOL click = !isKey && (s.mouse.state & (IM_LEFT_DOWN | IM_RIGHT_DOWN));
            if (keyDown && s.key.code == 0x01) { /* Esc: skip this step */
                PostMessageW(g_main, WM_IDENTIFIED, (WPARAM)g_identifySeat, 0);
                g_identifySeat = -1;
                LeaveCriticalSection(&g_lock);
                continue;
            }
            if ((g_identifyKind == DEV_KEYBOARD && keyDown) || (g_identifyKind == DEV_MOUSE && click)) {
                int target = g_identifySeat, kind = g_identifyKind;
                g_route[dev] = target;
                g_known[dev] = TRUE;
                g_identifySeat = -1;
                LeaveCriticalSection(&g_lock);
                remember_seat(dev, g_hw[dev], target);
                test_log("identified device %d -> workplace %d", dev, target + 1);
                PostMessageW(g_main, WM_IDENTIFIED, (WPARAM)target, (LPARAM)kind);
                continue; /* the press that identified it is not passed on */
            }
        }
        if (seat > 0) forward(seat, isKey, &s);
        LeaveCriticalSection(&g_lock);

        if (seat == 0) ic.send(g_ctx, dev, &s, 1);
        /* NOT_USED: the device is switched off, so the input is dropped. */
    }
    return 0;
}

/* ---- pipe server ------------------------------------------------------------ */

static int client_seat(HANDLE pipe)
{
    if (g_test) {
        WCHAR v[8];
        return GetEnvironmentVariableW(L"OPENSEAT_TEST_SEAT", v, 8) ? _wtoi(v) - 1 : -1;
    }
    ULONG session = 0;
    if (!GetNamedPipeClientSessionId(pipe, &session) || session == WTSGetActiveConsoleSessionId()) return -1;
    LPWSTR user = NULL;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &bytes)) return -1;
    int found = -1;
    for (int s = 1; s < g_seatCount; s++)
        if (g_seat[s].active && lstrcmpiW(user, g_accounts[s]) == 0) found = s;
    WTSFreeMemory(user);
    return found;
}

static DWORD WINAPI server_thread(LPVOID unused)
{
    (void)unused;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    /* SYSTEM and Administrators: full.  Signed-in users: read only, so they
     * cannot create a competing pipe instance.  The controller still checks
     * which account each reader belongs to before sending it anything. */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)",
                                                              SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
        PostMessageW(g_main, WM_FATAL, 1, 0);
        return 0;
    }
    BOOL first = TRUE;
    HANDLE connected = CreateEventW(NULL, TRUE, FALSE, NULL);
    while (g_running) {
        HANDLE h = CreateNamedPipeW(OPENSEAT_PIPE,
                                    PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                                    64 * 1024, 0, 0, &sa);
        if (h == INVALID_HANDLE_VALUE) {
            if (first) { PostMessageW(g_main, WM_FATAL, 2, 0); break; }
            Sleep(1000);
            continue;
        }
        first = FALSE;
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = connected;
        ResetEvent(connected);
        BOOL ok = ConnectNamedPipe(h, &ov);
        DWORD err = ok ? ERROR_PIPE_CONNECTED : GetLastError();
        if (err == ERROR_IO_PENDING) {
            while (g_running && WaitForSingleObject(connected, 300) == WAIT_TIMEOUT) {}
            DWORD unused2;
            ok = g_running && GetOverlappedResult(h, &ov, &unused2, FALSE);
            if (!ok) CancelIoEx(h, &ov);
        } else {
            ok = err == ERROR_PIPE_CONNECTED;
        }
        int seat = ok ? client_seat(h) : -1;
        if (seat <= 0 || seat >= g_seatCount || !g_seat[seat].active) {
            if (ok) DisconnectNamedPipe(h);
            CloseHandle(h);
            continue;
        }
        EnterCriticalSection(&g_lock);
        drop_pipe(seat);
        g_seat[seat].pipe = h;
        LeaveCriticalSection(&g_lock);
        test_log("agent connected for workplace %d", seat + 1);
        PostMessageW(g_main, WM_REFRESH, 0, 0);
    }
    CloseHandle(connected);
    LocalFree(sa.lpSecurityDescriptor);
    return 0;
}

/* ---- matching devices to workplaces ---------------------------------------- */

static void match_devices(void)
{
    for (int dev = 1; dev <= MAX_DEV; dev++) {
        WCHAR raw[512];
        ZeroMemory(raw, sizeof(raw));
        unsigned int n = ic.hardware_id(g_ctx, dev, raw, sizeof(raw) - sizeof(WCHAR) * 2);
        lstrcpynW(g_hw[dev], n ? raw : L"", 400); /* first string of the list */
        g_route[dev] = 0;
        g_known[dev] = FALSE;
    }
    for (int dev = 1; dev <= MAX_DEV; dev++) {
        if (!g_hw[dev][0]) continue;
        /* The setup window's choice wins.  Several devices of the same model
         * can only be told apart if they all go to one workplace. */
        int match = -2;
        for (int i = 0; i < g_deviceCount; i++) {
            if (!hwid_matches(&g_devices[i], g_hw[dev])) continue;
            if (match == -2) match = g_devices[i].seat;
            else if (match != g_devices[i].seat) match = -3;
        }
        if (match >= 0 || match == NOT_USED) { g_route[dev] = match; g_known[dev] = TRUE; continue; }
        /* Otherwise use what was picked by pressing it last time, if it is still on the same port. */
        int seat = learned_seat(dev, g_hw[dev]);
        if (seat >= 0) { g_route[dev] = seat; g_known[dev] = TRUE; }
    }
    for (int s = 1; s < g_seatCount; s++) {
        if (!g_seat[s].active) continue;
        BOOL kb = FALSE, mouse = FALSE;
        for (int dev = 1; dev <= MAX_DEV; dev++) {
            if (g_route[dev] != s) continue;
            if (ic.is_keyboard(dev)) kb = TRUE;
            if (ic.is_mouse(dev)) mouse = TRUE;
        }
        g_seat[s].needKeyboard = !kb;
        g_seat[s].needMouse = !mouse;
    }
    for (int dev = 1; dev <= MAX_DEV; dev++)
        if (g_hw[dev][0]) test_log("device %d hw=%ls -> %d known=%d", dev, g_hw[dev], g_route[dev] + 1, g_known[dev]);
}

/* ---- "press a key on this workplace's keyboard" prompt --------------------- */

static void next_identify(void);

static LRESULT CALLBACK prompt_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(wnd, &ps);
        GetClientRect(wnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(24, 30, 40));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        WCHAR title[40], body[200];
        _snwprintf(title, 40, L"Workplace %d", g_identifySeat + 1);
        lstrcpynW(body, g_identifyKind == DEV_KEYBOARD
                            ? L"Press any key on the keyboard that belongs to this screen."
                            : L"Now click with the mouse that belongs to this screen.", 200);
        RECT a = rc, b = rc, c = rc;
        a.bottom = rc.bottom / 2 - S(10);
        b.top = rc.bottom / 2;
        b.bottom = b.top + S(60);
        c.top = b.bottom;
        c.bottom = c.top + S(40);
        SelectObject(dc, g_fBig);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, title, -1, &a, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        SelectObject(dc, g_fHead);
        DrawTextW(dc, body, -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, g_fText);
        SetTextColor(dc, RGB(170, 180, 195));
        DrawTextW(dc, L"Press Esc to skip this step.", -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(wnd, &ps);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(wnd, msg, wp, lp);
}

static void show_prompt(int seat)
{
    RECT a = g_screens[g_seat[seat].screen].area;
    if (!g_prompt)
        g_prompt = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"VISIONPrompt", L"VISION: choose keyboard and mouse", WS_POPUP,
                                   0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetWindowPos(g_prompt, HWND_TOPMOST, a.left, a.top, a.right - a.left, a.bottom - a.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_prompt, NULL, TRUE);
}

/* ---- Remote Desktop client per workplace ------------------------------------ */

static BOOL write_rdp(int seat, WCHAR *path, size_t chars)
{
    WCHAR name[32], host[32], comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD compLen = MAX_COMPUTERNAME_LENGTH + 1;
    RECT a = g_screens[g_seat[seat].screen].area;
    _snwprintf(name, 32, L"workplace%d.rdp", seat + 1);
    setup_file(name, path, chars);
    seat_host(seat, host, 32);
    if (!GetComputerNameW(comp, &compLen)) lstrcpynW(comp, L".", MAX_COMPUTERNAME_LENGTH + 1);

    FILE *f = _wfopen(path, L"w, ccs=UTF-16LE");
    if (!f) return FALSE;
    fwprintf(f,
             L"screen mode id:i:2\nuse multimon:i:0\ndesktopwidth:i:%ld\ndesktopheight:i:%ld\nsession bpp:i:32\n"
             L"winposstr:s:0,1,%ld,%ld,%ld,%ld\nfull address:s:%ls\nusername:s:%ls\\%ls\n"
             L"prompt for credentials:i:0\npromptcredentialonce:i:0\nauthentication level:i:0\n"
             L"enablecredsspsupport:i:1\nnegotiate security layer:i:1\ndisplayconnectionbar:i:0\n"
             L"pinconnectionbar:i:0\nautoreconnection enabled:i:1\naudiomode:i:%d\nkeyboardhook:i:0\n"
             L"redirectclipboard:i:%d\nredirectprinters:i:0\nredirectcomports:i:0\nredirectsmartcards:i:0\n"
             L"redirectposdevices:i:0\ndrivestoredirect:s:\ndevicestoredirect:s:\nconnection type:i:7\nnetworkautodetect:i:0\n"
             L"bandwidthautodetect:i:1\nallow font smoothing:i:1\nsmart sizing:i:0\n",
             a.right - a.left, a.bottom - a.top, a.left, a.top, a.right, a.bottom, host, comp, g_accounts[seat],
             (g_share[seat] & SHARE_SOUND) ? 0 : 2,       /* 2 = no sound */
             (g_share[seat] & SHARE_CLIPBOARD) ? 1 : 0);  /* nothing else of workplace 1 is ever passed in */
    fclose(f);
    return TRUE;
}

static void launch_seat(int seat)
{
    Seat *st = &g_seat[seat];
    WCHAR rdp[MAX_PATH], sys[MAX_PATH], cmd[MAX_PATH * 2 + 16];
    if (g_test) { set_state(seat, L"Ready (test mode: no Remote Desktop window)"); return; }
    if (!write_rdp(seat, rdp, MAX_PATH)) { set_state(seat, L"Could not write its connection file."); st->bad = TRUE; return; }
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(cmd, MAX_PATH * 2 + 16, L"\"%ls\\mstsc.exe\" \"%ls\"", sys, rdp);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        set_state(seat, L"Could not start Remote Desktop (error %lu).", GetLastError());
        st->bad = TRUE;
        return;
    }
    CloseHandle(pi.hThread);
    st->process = pi.hProcess;
    st->pid = pi.dwProcessId;
    st->window = NULL;
    st->startedAt = GetTickCount();
    set_state(seat, L"Starting\x2026");
}

typedef struct { DWORD pid; HWND found; } FindWin;

static BOOL CALLBACK find_window_proc(HWND wnd, LPARAM lp)
{
    FindWin *f = (FindWin *)lp;
    DWORD pid = 0;
    WCHAR cls[64];
    GetWindowThreadProcessId(wnd, &pid);
    if (pid != f->pid || !IsWindowVisible(wnd)) return TRUE;
    GetClassNameW(wnd, cls, 64);
    if (lstrcmpW(cls, L"TscShellContainerClass") != 0) return TRUE;
    f->found = wnd;
    return FALSE;
}

static BOOL is_seat_window(HWND wnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetAncestor(wnd, GA_ROOT), &pid);
    for (int s = 1; s < g_seatCount; s++)
        if (g_seat[s].pid && g_seat[s].pid == pid) return TRUE;
    return FALSE;
}

static void watch_seats(void)
{
    DWORD now = GetTickCount();
    for (int s = 1; s < g_seatCount; s++) {
        Seat *st = &g_seat[s];
        if (!st->active || st->bad || st->needKeyboard || st->needMouse || g_test) continue;
        if (st->process && WaitForSingleObject(st->process, 0) == WAIT_OBJECT_0) {
            CloseHandle(st->process);
            st->process = NULL;
            st->pid = 0;
            st->window = NULL;
            st->quickExits = (now - st->startedAt < 20000) ? st->quickExits + 1 : 0;
            if (st->quickExits >= 3) {
                st->bad = TRUE;
                set_state(s, L"Windows would not start this workplace. On Windows 10/11 only one person can be signed in.");
                continue;
            }
            st->nextLaunch = now + 3000;
            set_state(s, L"Reconnecting\x2026");
        }
        if (!st->process && (int)(now - st->nextLaunch) >= 0) launch_seat(s);
        if (st->process && !st->window) {
            FindWin f = { st->pid, NULL };
            EnumWindows(find_window_proc, (LPARAM)&f);
            if (f.found) {
                RECT want = g_screens[st->screen].area, have;
                st->window = f.found;
                GetWindowRect(f.found, &have);
                if (!EqualRect(&want, &have))
                    SetWindowPos(f.found, NULL, want.left, want.top, want.right - want.left, want.bottom - want.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        if (st->window && IsWindowEnabled(st->window)) EnableWindow(st->window, FALSE); /* workplace 1 cannot type into it */
    }

    /* Keep workplace 1's pointer on its own screens. */
    if (g_launched) {
        RECT cur;
        GetClipCursor(&cur);
        if (!EqualRect(&cur, &g_clip)) ClipCursor(&g_clip);
        HWND fg = GetForegroundWindow();
        if (fg && is_seat_window(fg)) SetForegroundWindow(g_main);
    }
}

static void start_launching(void)
{
    BOOL any = FALSE;
    SetRectEmpty(&g_clip);
    for (int i = 0; i < g_screenCount; i++)
        if (g_screens[i].seat == 0) UnionRect(&g_clip, &g_clip, &g_screens[i].area);
    for (int s = 1; s < g_seatCount; s++) {
        if (!g_seat[s].active) continue;
        any = TRUE;
        if (!g_seat[s].needKeyboard && !g_seat[s].needMouse) launch_seat(s);
    }
    g_launched = any && !IsRectEmpty(&g_clip);
}

static void next_identify(void)
{
    for (int s = 1; s < g_seatCount; s++) {
        Seat *st = &g_seat[s];
        if (!st->active || (!st->needKeyboard && !st->needMouse)) continue;
        EnterCriticalSection(&g_lock);
        g_identifySeat = s;
        g_identifyKind = st->needKeyboard ? DEV_KEYBOARD : DEV_MOUSE;
        LeaveCriticalSection(&g_lock);
        set_state(s, st->needKeyboard ? L"Waiting: press a key on this workplace's keyboard."
                                      : L"Waiting: click with this workplace's mouse.");
        show_prompt(s);
        return;
    }
    if (g_prompt) ShowWindow(g_prompt, SW_HIDE);
    start_launching();
}

/* ---- one-time preparation (needs administrator rights) --------------------- */

static BOOL set_dword(HKEY root, const WCHAR *path, const WCHAR *name, DWORD value)
{
    return RegSetKeyValueW(root, path, name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

static BOOL read_password(int seat, WCHAR *out, size_t chars)
{
    WCHAR target[64];
    PCREDENTIALW cred = NULL;
    seat_cred_target(seat, target, 64);
    out[0] = 0;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &cred)) return FALSE;
    size_t n = cred->CredentialBlobSize / sizeof(WCHAR);
    if (n >= chars) n = chars - 1;
    memcpy(out, cred->CredentialBlob, n * sizeof(WCHAR));
    out[n] = 0;
    CredFree(cred);
    return TRUE;
}

static BOOL save_password(int seat, const WCHAR *password)
{
    WCHAR target[64], user[MAX_COMPUTERNAME_LENGTH + ACCOUNT_CHARS + 2], comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(comp, &n)) lstrcpynW(comp, L".", MAX_COMPUTERNAME_LENGTH + 1);
    _snwprintf(user, MAX_COMPUTERNAME_LENGTH + ACCOUNT_CHARS + 2, L"%ls\\%ls", comp, g_accounts[seat]);
    seat_cred_target(seat, target, 64);
    CREDENTIALW c;
    ZeroMemory(&c, sizeof(c));
    c.Type = CRED_TYPE_GENERIC; /* the Remote Desktop client reads TERMSRV/<host> */
    c.TargetName = target;
    c.UserName = user;
    c.CredentialBlob = (LPBYTE)password;
    c.CredentialBlobSize = (DWORD)(wcslen(password) * sizeof(WCHAR));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&c, 0);
}

/* A long random password nobody has to know: VISION signs the workplace in itself. */
static BOOL make_password(WCHAR *out, size_t chars)
{
    static const WCHAR alphabet[] = L"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    BYTE random[28];
    if (chars < sizeof(random) + 5) return FALSE;
    if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return FALSE;
    for (size_t i = 0; i < sizeof(random); i++) out[i] = alphabet[random[i] % (sizeof(alphabet) / sizeof(WCHAR) - 1)];
    lstrcpyW(out + sizeof(random), L"aK7#"); /* satisfies "must contain upper, lower, digit, symbol" rules */
    SecureZeroMemory(random, sizeof(random));
    return TRUE;
}

/* Each workplace runs in its own hidden Windows account, made and signed in
 * by VISION.  The person at the PC never creates or types one. */
static BOOL ensure_account(int seat, WCHAR *problem, size_t chars)
{
    USER_INFO_1 *info = NULL;
    WCHAR password[64], full[40], comment[] = L"Workplace account made by VISION";
    WCHAR *name = g_accounts[seat];
    BOOL haveSaved = read_password(seat, password, 64);
    SecureZeroMemory(password, sizeof(password));

    NET_API_STATUS st = NetUserGetInfo(NULL, name, 1, (LPBYTE *)&info);
    if (st == NERR_Success) {
        BOOL ours = info->usri1_comment && wcsstr(info->usri1_comment, L"VISION") != NULL;
        NetApiBufferFree(info);
        if (!ours) {
            _snwprintf(problem, chars, L"This PC already has a Windows account called \"%ls\" that VISION did not make. "
                                       L"Rename or remove it, then start again.", name);
            return FALSE;
        }
        if (!haveSaved) { /* e.g. another administrator starts VISION for the first time */
            USER_INFO_1003 set;
            if (!make_password(password, 64)) goto failed;
            set.usri1003_password = password;
            st = NetUserSetInfo(NULL, name, 1003, (LPBYTE)&set, NULL);
            if (st != NERR_Success || !save_password(seat, password)) goto failed;
        }
    } else if (st == NERR_UserNotFound) {
        USER_INFO_1 add;
        USER_INFO_1011 fullName;
        DWORD bad = 0;
        if (!make_password(password, 64)) goto failed;
        ZeroMemory(&add, sizeof(add));
        add.usri1_name = name;
        add.usri1_password = password;
        add.usri1_priv = USER_PRIV_USER;
        add.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD | UF_PASSWD_CANT_CHANGE;
        add.usri1_comment = comment;
        st = NetUserAdd(NULL, 1, (LPBYTE)&add, &bad);
        if (st != NERR_Success || !save_password(seat, password)) goto failed;
        _snwprintf(full, 40, L"Workplace %d", seat + 1);
        fullName.usri1011_full_name = full;
        NetUserSetInfo(NULL, name, 1011, (LPBYTE)&fullName, NULL);
    } else {
        goto failed;
    }
    SecureZeroMemory(password, sizeof(password));
    /* Keep these accounts off the Windows sign-in screen. */
    set_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\SpecialAccounts\\UserList", name, 0);
    goto group;

failed:
    SecureZeroMemory(password, sizeof(password));
    _snwprintf(problem, chars, L"VISION could not prepare Workplace %d (Windows error %lu).", seat + 1, (unsigned long)st);
    return FALSE;

group:;
    /* Remote Desktop Users (well-known SID, its name depends on the language). */
    PSID group = NULL;
    WCHAR groupName[256], domain[256];
    DWORD nameLen = 256, domainLen = 256;
    SID_NAME_USE use;
    BYTE userSid[SECURITY_MAX_SID_SIZE];
    DWORD sidLen = sizeof(userSid);
    domainLen = 256;
    if (ConvertStringSidToSidW(L"S-1-5-32-555", &group) &&
        LookupAccountSidW(NULL, group, groupName, &nameLen, domain, &domainLen, &use)) {
        domainLen = 256;
        if (LookupAccountNameW(NULL, g_accounts[seat], userSid, &sidLen, domain, &domainLen, &use)) {
            LOCALGROUP_MEMBERS_INFO_0 member = { userSid };
            NetLocalGroupAddMembers(NULL, groupName, 0, (LPBYTE)&member, 1); /* already a member is fine */
        }
    }
    if (group) LocalFree(group);
    return TRUE;
}

static BOOL install_files(WCHAR *problem, size_t chars)
{
    WCHAR self[MAX_PATH], target[MAX_PATH], run[MAX_PATH + 16];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    CreateDirectoryW(g_installDir, NULL);
    _snwprintf(target, MAX_PATH, L"%ls\\VISION.exe", g_installDir);
    if (lstrcmpiW(self, target) != 0 && !CopyFileW(self, target, FALSE) && GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(problem, chars, L"Could not copy VISION to %ls (error %lu).", g_installDir, GetLastError());
        return FALSE;
    }
    if (lstrcmpiW(g_exeDir, g_installDir) != 0) {
        WCHAR from[MAX_PATH], to[MAX_PATH];
        _snwprintf(from, MAX_PATH, L"%ls\\interception.dll", g_exeDir);
        _snwprintf(to, MAX_PATH, L"%ls\\interception.dll", g_installDir);
        if (GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES) CopyFileW(from, to, FALSE);
    }
    /* The agent starts in every Windows session and quits at once unless it is
     * one of VISION's workplaces. */
    _snwprintf(run, MAX_PATH + 16, L"\"%ls\" --agent", target);
    if (RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"VISIONAgent",
                        REG_SZ, run, (DWORD)((wcslen(run) + 1) * sizeof(WCHAR))) != ERROR_SUCCESS) {
        _snwprintf(problem, chars, L"Could not register the VISION helper to start with Windows.");
        return FALSE;
    }
    /* Allow Remote Desktop sign-ins (the extra workplaces use them on this PC only). */
    set_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server", L"fDenyTSConnections", 0);
    /* Local accounts sign in with NTLM, and Windows only uses a saved password
     * for that when policy allows it.  Allow it for VISION's own addresses only. */
    const WCHAR *deleg = L"SOFTWARE\\Policies\\Microsoft\\Windows\\CredentialsDelegation";
    WCHAR list[160];
    _snwprintf(list, 160, L"%ls\\AllowSavedCredentialsWhenNTLMOnly", deleg);
    set_dword(HKEY_LOCAL_MACHINE, deleg, L"AllowSavedCredentialsWhenNTLMOnly", 1);
    set_dword(HKEY_LOCAL_MACHINE, deleg, L"ConcatenateDefaults_AllowSavedNTLMOnly", 1);
    for (int s = 1; s < MAX_SEATS; s++) {
        WCHAR name[8], target[64];
        _snwprintf(name, 8, L"os%d", s + 1);
        seat_cred_target(s, target, 64);
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, list, name, REG_SZ, target, (DWORD)((wcslen(target) + 1) * sizeof(WCHAR)));
    }
    return TRUE;
}

static void run_hidden(WCHAR *cmd)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return;
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

/* The shared folder is the only place files can pass between workplaces.
 * Only workplaces ticked in "Connections and sharing" may open it. */
static void apply_folder_sharing(void)
{
    WCHAR dir[MAX_PATH], sys[MAX_PATH], me[300], cmd[2048];
    DWORD meLen = 300;
    BOOL any = FALSE;
    shared_folder(dir, MAX_PATH);
    for (int s = 1; s < g_seatCount; s++)
        if ((g_share[s] & SHARE_ON) && (g_share[s] & SHARE_FOLDER)) any = TRUE;
    if (!any && GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return;
    CreateDirectoryW(dir, NULL);
    GetSystemDirectoryW(sys, MAX_PATH);
    if (!GetUserNameW(me, &meLen)) return;
    /* Start from nothing inherited: administrators, the system and workplace 1 only. */
    _snwprintf(cmd, 2048, L"\"%ls\\icacls.exe\" \"%ls\" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F \"%ls\":(OI)(CI)F",
               sys, dir, me);
    run_hidden(cmd);
    for (int s = 1; s < MAX_SEATS; s++) {
        BOOL allowed = s < g_seatCount && (g_share[s] & SHARE_ON) && (g_share[s] & SHARE_FOLDER);
        if (allowed) _snwprintf(cmd, 2048, L"\"%ls\\icacls.exe\" \"%ls\" /grant:r %ls:(OI)(CI)M", sys, dir, g_accounts[s]);
        else _snwprintf(cmd, 2048, L"\"%ls\\icacls.exe\" \"%ls\" /remove %ls", sys, dir, g_accounts[s]);
        run_hidden(cmd);
    }
}

static BOOL install_interception(void)
{
    WCHAR cmd[2048], sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(cmd, 2048,
               L"\"%ls\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -Command \""
               L"$ErrorActionPreference='Stop';"
               L"[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;"
               L"$z=Join-Path $env:TEMP 'openseat-interception.zip';$d=Join-Path $env:TEMP 'openseat-interception';"
               L"Invoke-WebRequest -UseBasicParsing '%ls' -OutFile $z;"
               L"Expand-Archive -Force $z $d;"
               L"$i=(Get-ChildItem $d -Recurse -Filter install-interception.exe | Select-Object -First 1).FullName;"
               L"$l=(Get-ChildItem $d -Recurse -Filter interception.dll | Where-Object { $_.Directory.Name -eq 'x64' } | Select-Object -First 1).FullName;"
               L"New-Item -ItemType Directory -Force '%ls' | Out-Null;"
               L"Copy-Item -Force $l '%ls';"
               L"$p=Start-Process -FilePath $i -ArgumentList '/install' -Wait -PassThru -NoNewWindow;"
               L"exit $p.ExitCode\"",
               sys, INTERCEPTION_URL, g_installDir, g_installDir);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

/* ---- status window ---------------------------------------------------------- */

static void stop_all(void)
{
    if (!InterlockedExchange(&g_running, 0)) return;
    if (g_router) { WaitForSingleObject(g_router, 3000); CloseHandle(g_router); g_router = NULL; }
    if (g_ctx) { ic.destroy(g_ctx); g_ctx = NULL; } /* all input goes back to the main desktop */
    ClipCursor(NULL);
    for (int s = 1; s < MAX_SEATS; s++) {
        Seat *st = &g_seat[s];
        /* Closing the window only disconnects: the person's programs keep
         * running and come back next time. */
        if (st->process) { TerminateProcess(st->process, 0); CloseHandle(st->process); st->process = NULL; }
        EnterCriticalSection(&g_lock);
        drop_pipe(s);
        LeaveCriticalSection(&g_lock);
    }
    SetThreadExecutionState(ES_CONTINUOUS);
}

static void paint_status(HWND wnd, HDC dc)
{
    RECT rc;
    GetClientRect(wnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    RECT t = { S(20), S(14), rc.right - S(20), S(48) };
    SelectObject(dc, g_fHead);
    SetTextColor(dc, RGB(28, 32, 38));
    DrawTextW(dc, g_test ? L"VISION is running (test mode)" : L"VISION is running", -1, &t, DT_SINGLELINE | DT_VCENTER);

    int y = S(56);
    SelectObject(dc, g_fText);
    for (int s = 0; s < g_seatCount; s++) {
        WCHAR line[300];
        RECT r1 = { S(20), y, rc.right - S(20), y + S(22) };
        RECT r2 = { S(36), y + S(22), rc.right - S(20), y + S(44) };
        if (s == 0) {
            lstrcpynW(line, L"Workplace 1  \x00B7  you, on the main Windows desktop", 300);
        } else if (!g_seat[s].active) {
            _snwprintf(line, 300, L"Workplace %d  \x00B7  switched off", s + 1);
        } else {
            _snwprintf(line, 300, L"Workplace %d  \x00B7  screen %d%ls", s + 1, g_seat[s].screen + 1,
                       g_seat[s].pipe ? L"  \x00B7  keyboard and mouse connected" : L"");
        }
        SetTextColor(dc, RGB(28, 32, 38));
        DrawTextW(dc, line, -1, &r1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(dc, g_seat[s].bad ? RGB(180, 40, 40) : RGB(98, 105, 115));
        DrawTextW(dc, s == 0 ? L"Working normally." : g_seat[s].state, -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += S(52);
    }
    RECT foot = { S(20), y + S(4), rc.right - S(150), y + S(60) };
    SetTextColor(dc, RGB(98, 105, 115));
    DrawTextW(dc, L"Workplace 1's pointer stays on its own screens. To stop all extra workplaces, press Stop or Ctrl+Alt+Shift+S.",
              -1, &foot, DT_WORDBREAK | DT_NOPREFIX);
}

static LRESULT CALLBACK status_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        paint_status(wnd, dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(wnd, &rc);
        MoveWindow(g_stopButton, rc.right - S(130), rc.bottom - S(46), S(110), S(30), TRUE);
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    }
    case WM_TIMER:
        watch_seats();
        return 0;
    case WM_IDENTIFIED: {
        int seat = (int)wp;
        if (seat > 0 && seat < MAX_SEATS) {
            if (lp == DEV_KEYBOARD) g_seat[seat].needKeyboard = FALSE;
            else if (lp == DEV_MOUSE) g_seat[seat].needMouse = FALSE;
            else { g_seat[seat].needKeyboard = g_seat[seat].needMouse = FALSE; } /* skipped */
        }
        next_identify();
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    }
    case WM_REFRESH:
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    case WM_FATAL:
        stop_all();
        say(wp == 2 ? L"VISION is already running, or another program is using its connection name."
                    : L"VISION could not set up its connection for the workplaces.", MB_ICONERROR);
        DestroyWindow(wnd);
        return 0;
    case WM_HOTKEY:
    case WM_COMMAND:
        if ((msg == WM_HOTKEY && wp == HOTKEY_STOP) || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
            DestroyWindow(wnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (MessageBoxW(wnd, L"Stop all extra workplaces?\n\nPeople there are disconnected; their open programs keep running and come back next time.",
                        L"VISION", MB_YESNO | MB_ICONQUESTION) == IDYES)
            DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        stop_all();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
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

/* ---- entry ------------------------------------------------------------------ */

static BOOL is_elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION e = { 0 };
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(token);
    return ok && e.TokenIsElevated;
}

static BOOL check_setup(WCHAR *problem, size_t chars)
{
    int active = 0;
    for (int s = 1; s < g_seatCount; s++) {
        Seat *st = &g_seat[s];
        st->screen = seat_first_screen(s);
        if (!(g_share[s] & SHARE_ON)) { set_state(s, L"Switched off in Connections and sharing."); continue; }
        if (st->screen < 0) {
            _snwprintf(problem, chars, L"Workplace %d has no screen. Open the setup window and give it one.", s + 1);
            return FALSE;
        }
        st->active = TRUE;
        st->writeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        active++;
    }
    if (!active) {
        _snwprintf(problem, chars, L"No extra workplace is switched on. Add one in the setup window, or switch it on in Connections and sharing.");
        return FALSE;
    }
    if (seat_screens(0) == 0) {
        _snwprintf(problem, chars, L"Workplace 1 has no screen left. Keep at least one screen for workplace 1.");
        return FALSE;
    }
    RECT main;
    SetRectEmpty(&main);
    for (int i = 0; i < g_screenCount; i++)
        if (g_screens[i].seat == 0) UnionRect(&main, &main, &g_screens[i].area);
    for (int s = 1; s < g_seatCount; s++) {
        RECT overlap;
        if (g_seat[s].active && IntersectRect(&overlap, &main, &g_screens[g_seat[s].screen].area)) {
            _snwprintf(problem, chars, L"Workplace %d's screen sits between workplace 1's screens. In Windows Display settings, "
                                       L"place workplace 1's screens next to each other.", s + 1);
            return FALSE;
        }
    }
    return TRUE;
}

int run_controller(HINSTANCE inst, BOOL test, BOOL automatic)
{
    WCHAR problem[512], programFiles[MAX_PATH];
    g_test = test;
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    g_fBig = font(40, FW_BOLD);
    g_fHead = font(16, FW_SEMIBOLD);
    g_fText = font(10, FW_NORMAL);
    InitializeCriticalSection(&g_lock);

    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    WCHAR *slash = wcsrchr(g_exeDir, L'\\');
    if (slash) *slash = 0;
    if (!GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH) &&
        !GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH))
        lstrcpynW(programFiles, L"C:\\Program Files", MAX_PATH);
    _snwprintf(g_installDir, MAX_PATH, L"%ls\\VISION", programFiles);

    HANDLE single = CreateMutexW(NULL, TRUE, L"Global\\VISION.Controller");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        say(L"VISION is already running. Look for its window on workplace 1's screen.", MB_ICONINFORMATION);
        return 0;
    }
    if (!test && !is_elevated()) {
        say(L"VISION needs administrator permission to start the workplaces.", MB_ICONERROR);
        return 1;
    }

    find_hardware();
    if (test) add_saved_devices();
    load_setup();
    if (!check_setup(problem, 512)) { if (!automatic) say(problem, MB_ICONWARNING); return 1; } /* started with Windows: stay quiet until set up */

    if (!test && !is_server_os() &&
        MessageBoxW(NULL,
                    L"This PC runs Windows 10 or 11 for desktops. Those editions allow only one person to be signed in at "
                    L"a time, so starting another workplace will sign the main user out.\n\n"
                    L"Several people at once needs Windows Server (2016 or newer). Windows Server allows two extra "
                    L"people without extra licences; more need Remote Desktop Services licences.\n\nTry anyway?",
                    L"VISION", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return 1;

    if (!test) {
        for (int s = 1; s < g_seatCount; s++)
            if (g_seat[s].active && !ensure_account(s, problem, 512)) { say(problem, MB_ICONWARNING); return 1; }
        apply_folder_sharing();
        if (!install_files(problem, 512)) { say(problem, MB_ICONERROR); return 1; }
    }

    if (!load_interception(g_installDir) && !load_interception(g_exeDir)) {
        if (test) { say(L"Test mode: interception.dll is missing next to VISION.exe.", MB_ICONERROR); return 1; }
        if (MessageBoxW(NULL,
                        L"VISION needs the free Interception input driver to tell keyboards and mice apart.\n\n"
                        L"Download and install it now from github.com/oblitum/Interception? "
                        L"(Free for non-commercial use. The PC must restart afterwards.)",
                        L"VISION", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 1;
        if (!install_interception()) {
            say(L"The Interception driver could not be installed automatically.\n\n"
                L"Install it by hand: download Interception.zip from github.com/oblitum/Interception/releases, run "
                L"\"install-interception.exe /install\" as administrator, copy library\\x64\\interception.dll to "
                L"C:\\Program Files\\VISION, then restart the PC.", MB_ICONERROR);
            return 1;
        }
        say(L"The input driver is installed. Restart the PC, then press Start in VISION again.", MB_ICONINFORMATION);
        return 0;
    }
    g_ctx = ic.create();
    if (!g_ctx) {
        say(L"The Interception input driver is not active. If you just installed it, restart the PC.", MB_ICONERROR);
        return 1;
    }
    ic.set_filter(g_ctx, ic.is_keyboard, 0xFFFF);
    ic.set_filter(g_ctx, ic.is_mouse, 0xFFFF);
    match_devices();

    WNDCLASSEXW cls;
    ZeroMemory(&cls, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = status_proc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    cls.lpszClassName = L"VISIONStatus";
    RegisterClassExW(&cls);
    cls.lpfnWndProc = prompt_proc;
    cls.lpszClassName = L"VISIONPrompt";
    RegisterClassExW(&cls);

    RECT a = g_screens[seat_first_screen(0)].area;
    int w = S(600), h = S(170) + g_seatCount * S(52);
    g_main = CreateWindowExW(0, L"VISIONStatus", L"VISION", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             a.left + (a.right - a.left - w) / 2, a.top + (a.bottom - a.top - h) / 2, w, h, NULL, NULL, inst, NULL);
    g_stopButton = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_main,
                                 (HMENU)(INT_PTR)IDCANCEL, inst, NULL);
    SendMessageW(g_stopButton, WM_SETFONT, (WPARAM)g_fText, TRUE);
    RegisterHotKey(g_main, HOTKEY_STOP, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S');
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); /* the main desktop sees no input from other workplaces */

    for (int s = 1; s < g_seatCount; s++) if (g_seat[s].active) set_state(s, L"Starting\x2026");
        else if (!g_seat[s].state[0]) set_state(s, L"Switched off.");
    g_server = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    g_router = CreateThread(NULL, 0, router_thread, NULL, 0, NULL);
    ShowWindow(g_main, SW_SHOWNORMAL);
    SetTimer(g_main, 1, 400, NULL);
    next_identify();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    stop_all();
    if (g_server) { WaitForSingleObject(g_server, 2000); CloseHandle(g_server); }
    if (single) CloseHandle(single);
    return 0;
}

#include "common.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

Screen g_screens[MAX_SCREENS];
Device g_devices[MAX_DEVICES];
int g_screenCount, g_deviceCount, g_seatCount = 1;
BOOL g_demo;
WCHAR g_accounts[MAX_SEATS][ACCOUNT_CHARS];
int g_share[MAX_SEATS];
static WCHAR g_gdiNames[MAX_SCREENS][CCHDEVICENAME];
IdleScreen g_idle[MAX_IDLE];
int g_idleCount;

/* ---- saved setup -------------------------------------------------------- */

void setup_dir(WCHAR *out, size_t chars)
{
    WCHAR base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH))
        lstrcpynW(base, L".", MAX_PATH);
    _snwprintf(out, chars, L"%ls\\VISION", base);
    out[chars - 1] = 0;
    CreateDirectoryW(out, NULL);
}

void setup_file(const WCHAR *name, WCHAR *out, size_t chars)
{
    WCHAR dir[MAX_PATH];
    setup_dir(dir, MAX_PATH);
    _snwprintf(out, chars, L"%ls\\%ls", dir, name);
    out[chars - 1] = 0;
}

static void ini_path(WCHAR *out, size_t chars)
{
    setup_file(g_demo ? L"demo-setup.ini" : L"setup.ini", out, chars);
}

/* INI keys cannot contain '=' or start with '[' or ';'. */
static void ini_key(const WCHAR *key, WCHAR *out, size_t chars)
{
    size_t i = 0;
    out[i++] = L'k';
    for (; *key && i < chars - 1; key++)
        out[i++] = (*key == L'=' || *key == L'[' || *key == L']' || *key == L';') ? L'_' : *key;
    out[i] = 0;
}

static int clamp_seat(int seat) { return (seat >= 0 && seat < g_seatCount) ? seat : NOT_USED; }

void load_setup(void)
{
    WCHAR path[MAX_PATH], key[320];
    ini_path(path, MAX_PATH);
    g_seatCount = (int)GetPrivateProfileIntW(L"Setup", L"Workplaces", 1, path);
    if (g_seatCount < 1) g_seatCount = 1;
    if (g_seatCount > MAX_SEATS) g_seatCount = MAX_SEATS;
    /* Anything not in the file belongs to workplace 1, as in plain Windows. */
    for (int i = 0; i < g_screenCount; i++) {
        ini_key(g_screens[i].key, key, 320);
        g_screens[i].seat = clamp_seat((int)GetPrivateProfileIntW(L"Screens", key, 1, path) - 1);
    }
    for (int i = 0; i < g_deviceCount; i++) {
        ini_key(g_devices[i].key, key, 320);
        g_devices[i].seat = clamp_seat((int)GetPrivateProfileIntW(L"Devices", key, 1, path) - 1);
    }
    for (int s = 0; s < MAX_SEATS; s++) {
        WCHAR name[24];
        _snwprintf(name, 24, L"Workplace%d", s + 1);
        /* The person never sees or types these: VISION makes them itself. */
        _snwprintf(g_accounts[s], ACCOUNT_CHARS, L"VISION%d", s + 1);
        g_share[s] = (int)GetPrivateProfileIntW(L"Sharing", name, SHARE_DEFAULT, path) & 15;
    }
}

BOOL save_setup(void)
{
    WCHAR path[MAX_PATH], key[320], value[16];
    BOOL ok = TRUE;
    ini_path(path, MAX_PATH);
    _snwprintf(value, 16, L"%d", g_seatCount);
    ok &= WritePrivateProfileStringW(L"Setup", L"Workplaces", value, path);
    ok &= WritePrivateProfileStringW(L"Screens", NULL, NULL, path);
    ok &= WritePrivateProfileStringW(L"Devices", NULL, NULL, path);
    ok &= WritePrivateProfileStringW(L"Hardware", NULL, NULL, path);
    for (int i = 0; i < g_screenCount && ok; i++) {
        ini_key(g_screens[i].key, key, 320);
        _snwprintf(value, 16, L"%d", g_screens[i].seat + 1); /* 0 = not used */
        ok &= WritePrivateProfileStringW(L"Screens", key, value, path);
    }
    for (int i = 0; i < g_deviceCount && ok; i++) {
        ini_key(g_devices[i].key, key, 320);
        _snwprintf(value, 16, L"%d", g_devices[i].seat + 1);
        ok &= WritePrivateProfileStringW(L"Devices", key, value, path);
        if (g_devices[i].hwids[0])
            ok &= WritePrivateProfileStringW(L"Hardware", key, g_devices[i].hwids, path);
    }
    for (int s = 1; s < MAX_SEATS && ok; s++) {
        WCHAR name[24];
        _snwprintf(name, 24, L"Workplace%d", s + 1);
        _snwprintf(value, 16, L"%d", g_share[s]);
        ok &= WritePrivateProfileStringW(L"Sharing", name, value, path);
    }
    return ok;
}

/* The controller reads hardware ids from the file so it can match devices
 * even though it does not enumerate them the same way. */
static void load_hwids(void)
{
    WCHAR path[MAX_PATH], key[320];
    ini_path(path, MAX_PATH);
    for (int i = 0; i < g_deviceCount; i++) {
        if (g_devices[i].hwids[0]) continue;
        ini_key(g_devices[i].key, key, 320);
        GetPrivateProfileStringW(L"Hardware", key, L"", g_devices[i].hwids, HWID_CHARS, path);
    }
}

int learned_seat(int device, const WCHAR *hwid)
{
    WCHAR path[MAX_PATH], key[16], value[HWID_CHARS + 8];
    ini_path(path, MAX_PATH);
    _snwprintf(key, 16, L"i%d", device);
    if (!GetPrivateProfileStringW(L"Learned", key, L"", value, HWID_CHARS + 8, path)) return NOT_USED - 1;
    WCHAR *bar = wcschr(value, L'|');
    if (!bar || lstrcmpiW(bar + 1, hwid) != 0) return NOT_USED - 1;
    *bar = 0;
    int seat = _wtoi(value) - 1;
    return (seat >= 0 && seat < g_seatCount) ? seat : NOT_USED - 1;
}

void remember_seat(int device, const WCHAR *hwid, int seat)
{
    WCHAR path[MAX_PATH], key[16], value[HWID_CHARS + 8];
    ini_path(path, MAX_PATH);
    _snwprintf(key, 16, L"i%d", device);
    _snwprintf(value, HWID_CHARS + 8, L"%d|%ls", seat + 1, hwid);
    value[HWID_CHARS + 7] = 0;
    WritePrivateProfileStringW(L"Learned", key, value, path);
}

/* ---- finding hardware --------------------------------------------------- */

static BOOL CALLBACK screen_proc(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param)
{
    MONITORINFOEXW info;
    DISPLAY_DEVICEW dd;
    (void)dc; (void)rect; (void)param;
    if (g_screenCount >= MAX_SCREENS) return FALSE;
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info)) return TRUE;

    Screen *s = &g_screens[g_screenCount++];
    ZeroMemory(s, sizeof(*s));
    s->area = info.rcMonitor;
    s->primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    lstrcpynW(s->key, info.szDevice, 300);
    lstrcpynW(g_gdiNames[g_screenCount - 1], info.szDevice, CCHDEVICENAME);
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(info.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0])
        lstrcpynW(s->key, dd.DeviceID, 300); /* survives the display being renumbered */
    return TRUE;
}

static const WCHAR *plug_name(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech)
{
    switch (tech) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return L"VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return L"DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return L"HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return L"built-in";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO: return L"TV out";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return L"wireless";
    default: return L"";
    }
}

typedef struct { LUID adapter; UINT32 id; } DisplayId;

static BOOL same_display(const DisplayId *a, LUID adapter, UINT32 id)
{
    return a->id == id && a->adapter.LowPart == adapter.LowPart && a->adapter.HighPart == adapter.HighPart;
}

static void add_idle(LUID adapter, UINT32 target, BOOL mirrored)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME dst;
    if (g_idleCount >= MAX_IDLE) return;
    ZeroMemory(&dst, sizeof(dst));
    dst.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    dst.header.size = sizeof(dst);
    dst.header.adapterId = adapter;
    dst.header.id = target;
    IdleScreen *idle = &g_idle[g_idleCount++];
    ZeroMemory(idle, sizeof(*idle));
    idle->mirrored = mirrored;
    if (DisplayConfigGetDeviceInfo(&dst.header) == ERROR_SUCCESS) {
        lstrcpynW(idle->name, dst.monitorFriendlyDeviceName, 64);
        lstrcpynW(idle->connector, plug_name(dst.outputTechnology), 24);
    }
    if (!idle->name[0]) lstrcpynW(idle->name, L"Screen", 64);
}

/* Find screens that are plugged in but not their own desktop area: either
 * Windows shows the same picture on two screens, or the screen is off. */
static void find_idle_screens(void)
{
    DisplayId used[64], sources[64];
    int usedCount = 0, sourceCount = 0;
    UINT32 paths = 0, modes = 0;
    g_idleCount = 0;

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return;
    DISPLAYCONFIG_PATH_INFO *path = calloc(paths ? paths : 1, sizeof(*path));
    DISPLAYCONFIG_MODE_INFO *mode = calloc(modes ? modes : 1, sizeof(*mode));
    if (path && mode && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path, &modes, mode, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < paths && usedCount < 64; i++) {
            BOOL copy = FALSE;
            for (int k = 0; k < sourceCount; k++)
                if (same_display(&sources[k], path[i].sourceInfo.adapterId, path[i].sourceInfo.id)) copy = TRUE;
            used[usedCount].adapter = path[i].targetInfo.adapterId;
            used[usedCount++].id = path[i].targetInfo.id;
            if (copy) { add_idle(path[i].targetInfo.adapterId, path[i].targetInfo.id, TRUE); continue; }
            sources[sourceCount].adapter = path[i].sourceInfo.adapterId;
            sources[sourceCount++].id = path[i].sourceInfo.id;
        }
    }
    free(path);
    free(mode);

    paths = modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &paths, &modes) != ERROR_SUCCESS || !paths) return;
    path = calloc(paths, sizeof(*path));
    mode = calloc(modes ? modes : 1, sizeof(*mode));
    if (path && mode && QueryDisplayConfig(QDC_ALL_PATHS, &paths, path, &modes, mode, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < paths && usedCount < 64; i++) {
            BOOL known = FALSE;
            if (!path[i].targetInfo.targetAvailable) continue; /* nothing plugged into this output */
            for (int k = 0; k < usedCount; k++)
                if (same_display(&used[k], path[i].targetInfo.adapterId, path[i].targetInfo.id)) known = TRUE;
            if (known) continue;
            used[usedCount].adapter = path[i].targetInfo.adapterId;
            used[usedCount++].id = path[i].targetInfo.id;
            add_idle(path[i].targetInfo.adapterId, path[i].targetInfo.id, FALSE);
        }
    }
    free(path);
    free(mode);
}

/* Same as pressing Win+P and choosing "Extend". */
BOOL extend_displays(void)
{
    return SetDisplayConfig(0, NULL, 0, NULL, SDC_TOPOLOGY_EXTEND | SDC_APPLY) == ERROR_SUCCESS;
}

/* Friendly name and plug type of every active display, e.g. "SAMSUNG" + "HDMI". */
static void name_screens(const WCHAR gdi[][CCHDEVICENAME])
{
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || !paths) return;
    DISPLAYCONFIG_PATH_INFO *path = calloc(paths, sizeof(*path));
    DISPLAYCONFIG_MODE_INFO *mode = calloc(modes ? modes : 1, sizeof(*mode));
    if (path && mode && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path, &modes, mode, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < paths; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
            DISPLAYCONFIG_TARGET_DEVICE_NAME dst;
            ZeroMemory(&src, sizeof(src));
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = path[i].sourceInfo.adapterId;
            src.header.id = path[i].sourceInfo.id;
            ZeroMemory(&dst, sizeof(dst));
            dst.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            dst.header.size = sizeof(dst);
            dst.header.adapterId = path[i].targetInfo.adapterId;
            dst.header.id = path[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
            if (DisplayConfigGetDeviceInfo(&dst.header) != ERROR_SUCCESS) continue;
            const WCHAR *plug = plug_name(dst.outputTechnology);
            for (int k = 0; k < g_screenCount; k++) {
                if (lstrcmpiW(gdi[k], src.viewGdiDeviceName) != 0 || g_screens[k].name[0]) continue;
                lstrcpynW(g_screens[k].name, dst.monitorFriendlyDeviceName, 64);
                lstrcpynW(g_screens[k].connector, plug, 24);
            }
        }
    }
    free(path);
    free(mode);
}

static int screen_order(const void *a, const void *b)
{
    const Screen *x = a, *y = b;
    if (x->area.left != y->area.left) return x->area.left < y->area.left ? -1 : 1;
    return (x->area.top > y->area.top) - (x->area.top < y->area.top);
}

/* \\?\HID#VID_046D&PID_C31C&MI_00#7&2a1b&0&0000#{guid}  ->  HID\VID_046D&PID_C31C&MI_00\7&2a1b&0&0000 */
static BOOL instance_id(const WCHAR *path, WCHAR *out, size_t chars)
{
    const WCHAR *start = path, *end;
    if (wcsncmp(path, L"\\\\?\\", 4) == 0 || wcsncmp(path, L"\\??\\", 4) == 0) start = path + 4;
    end = wcsrchr(start, L'#');
    if (!end || (size_t)(end - start) >= chars) return FALSE;
    size_t n = (size_t)(end - start);
    for (size_t i = 0; i < n; i++) out[i] = start[i] == L'#' ? L'\\' : start[i];
    out[n] = 0;
    return TRUE;
}

static const GUID NO_CONTAINER = { 0, 0, 0, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };

/* All parts of one physical device (a keyboard with media keys shows up as
 * several "keyboards") share a container id.  Built-in devices have none. */
static void describe(const WCHAR *path, WCHAR *key, size_t keyChars, WCHAR *name, size_t nameChars,
                     WCHAR *hwid, size_t hwidChars)
{
    WCHAR id[400];
    DEVINST node;

    lstrcpynW(key, path, (int)keyChars);
    name[0] = hwid[0] = 0;

    HANDLE file = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WCHAR product[127] = L"";
        if (HidD_GetProductString(file, product, sizeof(product)) && product[0])
            lstrcpynW(name, product, (int)nameChars);
        CloseHandle(file);
    }
    if (!instance_id(path, id, 400)) return;
    if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return;

    /* First hardware id: the same string the input filter driver reports. */
    WCHAR ids[1024];
    ULONG bytes = sizeof(ids) - sizeof(WCHAR) * 2;
    ZeroMemory(ids, sizeof(ids));
    if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_HARDWAREID, NULL, ids, &bytes, 0) == CR_SUCCESS)
        lstrcpynW(hwid, ids, (int)hwidChars);

    GUID container;
    DEVPROPTYPE type;
    ULONG size = sizeof(container);
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type, (PBYTE)&container, &size, 0) == CR_SUCCESS &&
        container.Data1 != 0 && !IsEqualGUID(&container, &NO_CONTAINER)) {
        _snwprintf(key, keyChars, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                   container.Data1, container.Data2, container.Data3,
                   container.Data4[0], container.Data4[1], container.Data4[2], container.Data4[3],
                   container.Data4[4], container.Data4[5], container.Data4[6], container.Data4[7]);
        key[keyChars - 1] = 0;
    }
    if (!name[0]) {
        WCHAR desc[160];
        ULONG descBytes = sizeof(desc);
        if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_DEVICEDESC, NULL, desc, &descBytes, 0) == CR_SUCCESS)
            lstrcpynW(name, desc, (int)nameChars);
    }
}

const WCHAR *kind_name(int kinds)
{
    if (kinds == (DEV_KEYBOARD | DEV_MOUSE)) return L"Keyboard and mouse";
    return kinds == DEV_KEYBOARD ? L"Keyboard" : L"Mouse";
}

BOOL hwid_matches(const Device *d, const WCHAR *hwid)
{
    const WCHAR *p = d->hwids;
    size_t n = wcslen(hwid);
    if (!n) return FALSE;
    while (*p) {
        const WCHAR *end = wcschr(p, L'|');
        size_t len = end ? (size_t)(end - p) : wcslen(p);
        if (len == n && _wcsnicmp(p, hwid, n) == 0) return TRUE;
        if (!end) break;
        p = end + 1;
    }
    return FALSE;
}

static void add_device(const WCHAR *key, const WCHAR *name, const WCHAR *hwid, int kind, HANDLE handle)
{
    Device *d = NULL;
    for (int i = 0; i < g_deviceCount; i++)
        if (lstrcmpiW(g_devices[i].key, key) == 0) d = &g_devices[i];
    if (!d) {
        if (g_deviceCount >= MAX_DEVICES) return;
        d = &g_devices[g_deviceCount++];
        ZeroMemory(d, sizeof(*d));
        lstrcpynW(d->key, key, 300);
    }
    if (!d->name[0] && name[0]) lstrcpynW(d->name, name, 160);
    if (hwid[0] && !hwid_matches(d, hwid) && wcslen(d->hwids) + wcslen(hwid) + 2 < HWID_CHARS) {
        if (d->hwids[0]) wcscat(d->hwids, L"|");
        wcscat(d->hwids, hwid);
    }
    d->kinds |= kind;
    if (handle && d->handleCount < MAX_HANDLES) d->handles[d->handleCount++] = handle;
}

static void find_inputs(void)
{
    UINT count = 0;
    if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) return;
    RAWINPUTDEVICELIST *list = calloc(count, sizeof(*list));
    if (!list) return;
    count = GetRawInputDeviceList(list, &count, sizeof(*list));
    for (UINT i = 0; count != (UINT)-1 && i < count; i++) {
        WCHAR path[512] = L"", key[300], name[160], hwid[400];
        UINT size = 512;
        if (list[i].dwType != RIM_TYPEKEYBOARD && list[i].dwType != RIM_TYPEMOUSE) continue;
        if ((int)GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path, &size) <= 0 || !path[0]) continue;
        if (wcsstr(path, L"RDP_") || wcsstr(path, L"Root#RDP")) continue; /* Remote Desktop's virtual devices */
        describe(path, key, 300, name, 160, hwid, 400);
        add_device(key, name, hwid, list[i].dwType == RIM_TYPEKEYBOARD ? DEV_KEYBOARD : DEV_MOUSE, list[i].hDevice);
    }
    free(list);
}

static void demo_hardware(void)
{
    static const struct { RECT area; BOOL primary; } screens[] = {
        { { 0, 0, 1920, 1080 }, TRUE }, { { 1920, 0, 3840, 1080 }, FALSE }, { { 3840, 180, 5120, 1080 }, FALSE },
    };
    static const struct { const WCHAR *name; int kinds; } devices[] = {
        { L"Logitech K120 Keyboard", DEV_KEYBOARD }, { L"Logitech M100 Mouse", DEV_MOUSE },
        { L"Dell KB216 Keyboard", DEV_KEYBOARD },    { L"Dell MS116 Mouse", DEV_MOUSE },
        { L"Wireless Desktop Set", DEV_KEYBOARD | DEV_MOUSE },
    };
    for (int i = 0; i < 3; i++) {
        Screen *s = &g_screens[g_screenCount++];
        ZeroMemory(s, sizeof(*s));
        s->area = screens[i].area;
        s->primary = screens[i].primary;
        _snwprintf(s->key, 300, L"demo-screen-%d", i + 1);
        lstrcpynW(s->name, i == 0 ? L"DELL U2419H" : i == 1 ? L"LG TV" : L"Laptop screen", 64);
        lstrcpynW(s->connector, i == 0 ? L"DisplayPort" : i == 1 ? L"HDMI" : L"VGA", 24);
    }
    for (int i = 0; i < 5; i++) {
        WCHAR key[32];
        _snwprintf(key, 32, L"demo-device-%d", i + 1);
        add_device(key, devices[i].name, L"", devices[i].kinds, (HANDLE)(INT_PTR)(0x1000 + i));
    }
}

static void number_duplicates(void)
{
    for (int i = 0; i < g_deviceCount; i++) {
        Device *d = &g_devices[i];
        int earlier = 0, total = 0;
        if (!d->name[0]) lstrcpynW(d->name, kind_name(d->kinds), 160);
        for (int j = 0; j < g_deviceCount; j++) {
            WCHAR base[160];
            lstrcpynW(base, g_devices[j].name[0] ? g_devices[j].name : kind_name(g_devices[j].kinds), 160);
            WCHAR *mark = wcsstr(base, L"  (no. ");
            if (mark) *mark = 0;
            if (lstrcmpW(base, d->name) == 0) { total++; if (j < i) earlier++; }
        }
        if (total > 1) {
            WCHAR numbered[160];
            _snwprintf(numbered, 160, L"%ls  (no. %d)", d->name, earlier + 1);
            numbered[159] = 0;
            lstrcpynW(d->name, numbered, 160);
        }
    }
}

/* Testing only: OPENSEAT_TEST_SCREENS="left,top,right,bottom;..." replaces the
 * real monitors, because test machines rarely have several. */
static BOOL test_screens(void)
{
    WCHAR spec[512];
    if (!GetEnvironmentVariableW(L"OPENSEAT_TEST_SCREENS", spec, 512)) return FALSE;
    for (WCHAR *part = spec; part && *part && g_screenCount < MAX_SCREENS;) {
        WCHAR *next = wcschr(part, L';');
        if (next) *next++ = 0;
        Screen *s = &g_screens[g_screenCount];
        ZeroMemory(s, sizeof(*s));
        if (swscanf(part, L"%ld,%ld,%ld,%ld", &s->area.left, &s->area.top, &s->area.right, &s->area.bottom) == 4) {
            s->primary = g_screenCount == 0;
            _snwprintf(s->key, 300, L"test-screen-%d", g_screenCount + 1);
            static const WCHAR *names[] = { L"DELL U2419H", L"SAMSUNG TV", L"LG 24MK430", L"HP V22", L"ACER K242" };
            static const WCHAR *plugs[] = { L"DisplayPort", L"HDMI", L"VGA", L"HDMI", L"DVI" };
            lstrcpynW(s->name, names[g_screenCount % 5], 64);
            lstrcpynW(s->connector, plugs[g_screenCount % 5], 24);
            g_screenCount++;
        }
        part = next;
    }
    WCHAR idle[64];
    if (GetEnvironmentVariableW(L"OPENSEAT_TEST_IDLE", idle, 64)) { /* pretend one more screen is plugged in but off */
        lstrcpynW(g_idle[0].name, idle, 64);
        lstrcpynW(g_idle[0].connector, L"HDMI", 24);
        g_idleCount = 1;
    }
    return g_screenCount > 0;
}

void find_hardware(void)
{
    g_screenCount = g_deviceCount = g_idleCount = 0;
    if (g_demo) {
        demo_hardware();
    } else if (test_screens()) {
        find_inputs();
    } else {
        EnumDisplayMonitors(NULL, NULL, screen_proc, 0);
        name_screens((const WCHAR (*)[CCHDEVICENAME])g_gdiNames); /* before sorting: same order as g_gdiNames */
        find_idle_screens();
        qsort(g_screens, (size_t)g_screenCount, sizeof(Screen), screen_order);
        find_inputs();
    }
    number_duplicates();
    load_hwids();
}

/* Test mode only: treat every device listed in the setup file as present,
 * so the controller can be exercised without that hardware. */
void add_saved_devices(void)
{
    WCHAR path[MAX_PATH], keys[8192];
    ini_path(path, MAX_PATH);
    DWORD n = GetPrivateProfileStringW(L"Devices", NULL, L"", keys, 8192, path);
    for (WCHAR *k = keys; n && *k && g_deviceCount < MAX_DEVICES; k += wcslen(k) + 1) {
        BOOL known = FALSE;
        for (int i = 0; i < g_deviceCount; i++) {
            WCHAR have[320];
            ini_key(g_devices[i].key, have, 320);
            if (lstrcmpiW(have, k) == 0) known = TRUE;
        }
        if (known || k[0] != L'k') continue;
        Device *d = &g_devices[g_deviceCount++];
        ZeroMemory(d, sizeof(*d));
        lstrcpynW(d->key, k + 1, 300);
        lstrcpynW(d->name, k + 1, 160);
        d->kinds = DEV_KEYBOARD | DEV_MOUSE;
        GetPrivateProfileStringW(L"Hardware", k, L"", d->hwids, HWID_CHARS, path);
    }
}

/* ---- what each workplace has -------------------------------------------- */

int seat_screens(int seat)
{
    int n = 0;
    for (int i = 0; i < g_screenCount; i++) n += g_screens[i].seat == seat;
    return n;
}

int seat_first_screen(int seat)
{
    for (int i = 0; i < g_screenCount; i++)
        if (g_screens[i].seat == seat) return i;
    return -1;
}

BOOL seat_has(int seat, int kind)
{
    for (int i = 0; i < g_deviceCount; i++)
        if (g_devices[i].seat == seat && (g_devices[i].kinds & kind)) return TRUE;
    return FALSE;
}

void seat_host(int seat, WCHAR *out, size_t chars)
{
    _snwprintf(out, chars, L"127.0.0.%d", seat + 1); /* workplace 2 -> 127.0.0.2 */
    out[chars - 1] = 0;
}

void seat_cred_target(int seat, WCHAR *out, size_t chars)
{
    WCHAR host[32];
    seat_host(seat, host, 32);
    _snwprintf(out, chars, L"TERMSRV/%ls", host);
    out[chars - 1] = 0;
}

void shared_folder(WCHAR *out, size_t chars)
{
    WCHAR pub[MAX_PATH];
    if (!GetEnvironmentVariableW(L"PUBLIC", pub, MAX_PATH)) lstrcpynW(pub, L"C:\\Users\\Public", MAX_PATH);
    _snwprintf(out, chars, L"%ls\\VISION Shared", pub);
    out[chars - 1] = 0;
}

BOOL is_server_os(void)
{
    OSVERSIONINFOEXW v;
    ZeroMemory(&v, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    DWORDLONG mask = VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_EQUAL);
    v.wProductType = VER_NT_WORKSTATION;
    return !VerifyVersionInfoW(&v, VER_PRODUCT_TYPE, mask);
}

/* ---- events -> SendInput ------------------------------------------------ */

int event_to_inputs(const SeatEvent *e, INPUT out[4])
{
    int n = 0;
    ZeroMemory(out, sizeof(INPUT) * 4);
    if (e->magic != EVENT_MAGIC) return 0;
    if (e->kind == EVENT_KEY) {
        out[0].type = INPUT_KEYBOARD;
        out[0].ki.wScan = e->code;
        out[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        if (e->state & IK_UP) out[0].ki.dwFlags |= KEYEVENTF_KEYUP;
        if (e->state & IK_E0) out[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        return 1;
    }
    if (e->kind != EVENT_MOUSE) return 0;

    static const struct { uint16_t bit; DWORD flag; DWORD data; } buttons[] = {
        { IM_LEFT_DOWN, MOUSEEVENTF_LEFTDOWN, 0 },     { IM_LEFT_UP, MOUSEEVENTF_LEFTUP, 0 },
        { IM_RIGHT_DOWN, MOUSEEVENTF_RIGHTDOWN, 0 },   { IM_RIGHT_UP, MOUSEEVENTF_RIGHTUP, 0 },
        { IM_MIDDLE_DOWN, MOUSEEVENTF_MIDDLEDOWN, 0 }, { IM_MIDDLE_UP, MOUSEEVENTF_MIDDLEUP, 0 },
    };
    MOUSEINPUT *m = &out[0].mi;
    out[0].type = INPUT_MOUSE;
    if (e->x || e->y || (e->flags & IM_FLAG_ABSOLUTE)) {
        m->dx = e->x;
        m->dy = e->y;
        m->dwFlags |= MOUSEEVENTF_MOVE;
        if (e->flags & IM_FLAG_ABSOLUTE) m->dwFlags |= MOUSEEVENTF_ABSOLUTE;
        if (e->flags & IM_FLAG_VIRTUAL_DESKTOP) m->dwFlags |= MOUSEEVENTF_VIRTUALDESK;
    }
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (e->state & buttons[i].bit) m->dwFlags |= buttons[i].flag;
    n = m->dwFlags ? 1 : 0;

    /* X buttons and wheels need mouseData, so each gets its own record. */
    if (e->state & (IM_X1_DOWN | IM_X2_DOWN)) {
        out[n].type = INPUT_MOUSE;
        out[n].mi.dwFlags = MOUSEEVENTF_XDOWN;
        out[n].mi.mouseData = (e->state & IM_X1_DOWN) ? XBUTTON1 : XBUTTON2;
        n++;
    }
    if (e->state & (IM_X1_UP | IM_X2_UP)) {
        out[n].type = INPUT_MOUSE;
        out[n].mi.dwFlags = MOUSEEVENTF_XUP;
        out[n].mi.mouseData = (e->state & IM_X1_UP) ? XBUTTON1 : XBUTTON2;
        n++;
    }
    if (e->state & (IM_WHEEL | IM_HWHEEL)) {
        out[n].type = INPUT_MOUSE;
        out[n].mi.dwFlags = (e->state & IM_HWHEEL) ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        out[n].mi.mouseData = (DWORD)(LONG)e->rolling;
        n++;
    }
    return n;
}

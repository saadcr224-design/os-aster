/* Shared by the setup window, the controller and the in-session agent. */
#ifndef OPENSEAT_COMMON_H
#define OPENSEAT_COMMON_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <stdint.h>

#define MAX_SCREENS 16
#define MAX_DEVICES 48
#define MAX_HANDLES 8
#define MAX_SEATS 4
#define NOT_USED (-1)
#define HWID_CHARS 800
#define ACCOUNT_CHARS 64

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2 };

typedef struct {
    WCHAR key[300];   /* stable id used in the saved file */
    WCHAR name[64];      /* e.g. "SAMSUNG" or "LG TV" */
    WCHAR connector[24]; /* e.g. "HDMI", "VGA", "built-in" */
    RECT area;        /* position on the Windows desktop */
    BOOL primary;
    int seat;
    RECT draw;        /* where the setup window paints it */
} Screen;

typedef struct {
    WCHAR key[300];
    WCHAR name[160];
    WCHAR hwids[HWID_CHARS]; /* hardware ids of its parts, separated by '|' */
    int kinds;               /* DEV_KEYBOARD | DEV_MOUSE */
    HANDLE handles[MAX_HANDLES];
    int handleCount;
    int seat;
    DWORD flashUntil;
    RECT draw;
} Device;

extern Screen g_screens[MAX_SCREENS];
extern Device g_devices[MAX_DEVICES];
extern int g_screenCount, g_deviceCount, g_seatCount;
extern BOOL g_demo;
extern WCHAR g_accounts[MAX_SEATS][ACCOUNT_CHARS]; /* hidden Windows account OpenSeat makes per workplace; [0] unused */

/* What each extra workplace is allowed to do; everything else stays private. */
enum {
    SHARE_ON = 1,      /* the workplace may start */
    SHARE_CLIPBOARD = 2, /* copy and paste with workplace 1 */
    SHARE_FOLDER = 4,  /* may use the shared folder */
    SHARE_SOUND = 8,   /* may play sound on the PC's speakers */
    SHARE_DEFAULT = SHARE_ON | SHARE_SOUND
};
extern int g_share[MAX_SEATS];
void shared_folder(WCHAR *out, size_t chars);

void setup_dir(WCHAR *out, size_t chars);
void setup_file(const WCHAR *name, WCHAR *out, size_t chars);
void find_hardware(void);
void add_saved_devices(void);
void load_setup(void);
BOOL save_setup(void);
const WCHAR *kind_name(int kinds);
int seat_screens(int seat);
int seat_first_screen(int seat);
BOOL seat_has(int seat, int kind);
BOOL hwid_matches(const Device *d, const WCHAR *hwid);

/* Each extra workplace signs in over Remote Desktop to its own loopback
 * address, so Windows keeps a separate saved password for each one. */
void seat_host(int seat, WCHAR *out, size_t chars);
void seat_cred_target(int seat, WCHAR *out, size_t chars);
BOOL is_server_os(void);

/* Learned: Interception device number -> workplace, remembered between runs. */
int learned_seat(int device, const WCHAR *hwid);
void remember_seat(int device, const WCHAR *hwid, int seat);

/* ---- pipe protocol between controller and agents ------------------------ */

#define OPENSEAT_PIPE L"\\\\.\\pipe\\OpenSeat.Input.v1"
#define EVENT_MAGIC 0x4F534531u /* "OSE1" */

enum { EVENT_KEY = 1, EVENT_MOUSE = 2 };

typedef struct {
    uint32_t magic;
    uint16_t kind;
    uint16_t code;     /* key: scan code */
    uint16_t state;    /* key: Interception key state; mouse: button state */
    uint16_t flags;    /* mouse: Interception mouse flags */
    int16_t rolling;   /* mouse: wheel */
    int16_t reserved;
    int32_t x, y;
} SeatEvent;

/* Interception key states and mouse bits (from its public header). */
#define IK_UP 0x01
#define IK_E0 0x02
#define IK_E1 0x04
#define IM_LEFT_DOWN 0x001
#define IM_LEFT_UP 0x002
#define IM_RIGHT_DOWN 0x004
#define IM_RIGHT_UP 0x008
#define IM_MIDDLE_DOWN 0x010
#define IM_MIDDLE_UP 0x020
#define IM_X1_DOWN 0x040
#define IM_X1_UP 0x080
#define IM_X2_DOWN 0x100
#define IM_X2_UP 0x200
#define IM_WHEEL 0x400
#define IM_HWHEEL 0x800
#define IM_FLAG_ABSOLUTE 0x001
#define IM_FLAG_VIRTUAL_DESKTOP 0x002

/* Convert one event into SendInput records; returns how many were written. */
int event_to_inputs(const SeatEvent *e, INPUT out[4]);

#endif

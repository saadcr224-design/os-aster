/* Test double for interception.dll: replays a script of key presses and mouse
 * events and logs everything the controller passes back to the system.
 *   OPENSEAT_FAKE_SCRIPT  lines: "hw DEV ID" | "sleep MS" | "k DEV CODE STATE" |
 *                                "m DEV STATE FLAGS ROLLING X Y"
 *   OPENSEAT_FAKE_LOG     where "send ..." lines are written
 * Build: x86_64-w64-mingw32-gcc -shared -O2 fakeintercept.c -o interception.dll */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned short code, state; unsigned int information; } IKey;
typedef struct { unsigned short state, flags; short rolling; int x, y; unsigned int information; } IMouse;
typedef struct { int dev, sleep; IKey key; IMouse mouse; } Ev;

static Ev evs[512];
static int nev, pos;
static WCHAR hw[21][200];
static FILE *logf;
static CRITICAL_SECTION lock;

#define API __declspec(dllexport)

API int interception_is_keyboard(int d) { return d >= 1 && d <= 10; }
API int interception_is_mouse(int d) { return d >= 11 && d <= 20; }

API void *interception_create_context(void)
{
    char path[MAX_PATH], line[512];
    InitializeCriticalSection(&lock);
    if (GetEnvironmentVariableA("OPENSEAT_FAKE_LOG", path, MAX_PATH)) logf = fopen(path, "w");
    if (!GetEnvironmentVariableA("OPENSEAT_FAKE_SCRIPT", path, MAX_PATH)) return (void *)1;
    FILE *f = fopen(path, "r");
    if (!f) return (void *)1;
    while (fgets(line, sizeof(line), f) && nev < 512) {
        Ev *e = &evs[nev];
        int d, a, b, c, x, y;
        char id[200];
        memset(e, 0, sizeof(*e));
        if (sscanf(line, "hw %d %199s", &d, id) == 2 && d >= 1 && d <= 20) MultiByteToWideChar(CP_UTF8, 0, id, -1, hw[d], 200);
        else if (sscanf(line, "sleep %d", &a) == 1) { e->sleep = a; nev++; }
        else if (sscanf(line, "k %d %d %d", &d, &a, &b) == 3) { e->dev = d; e->key.code = a; e->key.state = b; nev++; }
        else if (sscanf(line, "m %d %d %d %d %d %d", &d, &a, &b, &c, &x, &y) == 6) {
            e->dev = d; e->mouse.state = a; e->mouse.flags = b; e->mouse.rolling = c; e->mouse.x = x; e->mouse.y = y; nev++;
        }
    }
    fclose(f);
    return (void *)1;
}

API void interception_destroy_context(void *c) { (void)c; if (logf) { fprintf(logf, "destroyed\n"); fclose(logf); logf = NULL; } }
API void interception_set_filter(void *c, void *p, unsigned short f) { (void)c; (void)p; (void)f; }

API int interception_wait_with_timeout(void *c, unsigned long ms)
{
    (void)c;
    while (pos < nev && evs[pos].sleep) { Sleep(evs[pos].sleep); pos++; }
    if (pos >= nev) { Sleep(ms); return 0; }
    return evs[pos].dev;
}

API int interception_receive(void *c, int dev, void *stroke, unsigned int n)
{
    (void)c; (void)n;
    if (pos >= nev || evs[pos].dev != dev) return 0;
    if (interception_is_keyboard(dev)) memcpy(stroke, &evs[pos].key, sizeof(IKey));
    else memcpy(stroke, &evs[pos].mouse, sizeof(IMouse));
    pos++;
    return 1;
}

API int interception_send(void *c, int dev, const void *stroke, unsigned int n)
{
    (void)c;
    EnterCriticalSection(&lock);
    if (logf) {
        if (interception_is_keyboard(dev)) {
            const IKey *k = stroke;
            fprintf(logf, "send key dev=%d code=%u state=%u\n", dev, k->code, k->state);
        } else {
            const IMouse *m = stroke;
            fprintf(logf, "send mouse dev=%d state=%u x=%d y=%d\n", dev, m->state, m->x, m->y);
        }
        fflush(logf);
    }
    LeaveCriticalSection(&lock);
    return (int)n;
}

API unsigned int interception_get_hardware_id(void *c, int dev, void *buf, unsigned int size)
{
    (void)c;
    if (dev < 1 || dev > 20 || !hw[dev][0]) return 0;
    unsigned int bytes = (unsigned int)((wcslen(hw[dev]) + 2) * sizeof(WCHAR));
    if (bytes > size) return 0;
    memset(buf, 0, bytes);
    memcpy(buf, hw[dev], wcslen(hw[dev]) * sizeof(WCHAR));
    return bytes;
}

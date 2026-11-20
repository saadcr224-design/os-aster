#ifndef VISION_UPDATE_H
#define VISION_UPDATE_H
#include <windows.h>

#define WM_UPDATE_FOUND (WM_APP + 40)
#define WM_UPDATE_PROGRESS (WM_APP + 41)
#define WM_UPDATE_READY (WM_APP + 42)
#define WM_UPDATE_FAILED (WM_APP + 43)

typedef struct {
    char version[32];
    WCHAR file[260];
    char sha256[80];
    WCHAR notes[300];
} UpdateInfo;

void update_check(HWND notify);           /* in the background; posts WM_UPDATE_FOUND */
const UpdateInfo *update_found(void);
void update_download(HWND notify);        /* posts WM_UPDATE_PROGRESS, then READY or FAILED */
BOOL update_run(const WCHAR *setup);
int update_relaunch(const WCHAR *args);
int compare_versions(const char *a, const char *b);
#endif

/*
 * The agent runs inside each extra workplace's Windows session.  It receives
 * that workplace's keyboard and mouse input from the controller and replays it
 * with SendInput, so it lands in this session only.
 *
 * It is started for every user by the Run key and quits at once in the main
 * (console) session.
 */
#include "common.h"

#include <stdio.h>

static FILE *g_log;

static void log_inputs(const INPUT *in, int n)
{
    for (int i = 0; i < n; i++) {
        if (in[i].type == INPUT_KEYBOARD)
            fprintf(g_log, "key scan=%u flags=%lu\n", in[i].ki.wScan, in[i].ki.dwFlags);
        else
            fprintf(g_log, "mouse dx=%ld dy=%ld flags=%lu data=%ld\n", in[i].mi.dx, in[i].mi.dy, in[i].mi.dwFlags,
                    (long)in[i].mi.mouseData);
    }
    fflush(g_log);
}

int run_agent(HINSTANCE inst, BOOL test)
{
    (void)inst;
    char logPath[MAX_PATH];
    if (!test && !GetSystemMetrics(SM_REMOTESESSION)) return 0;
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\OpenSeat.Agent");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    if (test && GetEnvironmentVariableA("OPENSEAT_TEST_AGENT_LOG", logPath, MAX_PATH)) g_log = fopen(logPath, "a");

    for (;;) {
        HANDLE pipe = CreateFileW(OPENSEAT_PIPE, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(OPENSEAT_PIPE, 2000);
            else Sleep(2000);
            continue;
        }
        BYTE buf[sizeof(SeatEvent) * 64];
        DWORD have = 0, got = 0;
        while (ReadFile(pipe, buf + have, sizeof(buf) - have, &got, NULL) && got > 0) {
            have += got;
            DWORD used = 0;
            while (have - used >= sizeof(SeatEvent)) {
                SeatEvent e;
                INPUT in[4];
                memcpy(&e, buf + used, sizeof(e));
                used += sizeof(e);
                int n = event_to_inputs(&e, in);
                if (n <= 0) continue;
                if (g_log) log_inputs(in, n);
                else SendInput((UINT)n, in, sizeof(INPUT));
            }
            memmove(buf, buf + used, have - used);
            have -= used;
        }
        CloseHandle(pipe);
        Sleep(1000);
    }
    CloseHandle(single);
    return 0;
}

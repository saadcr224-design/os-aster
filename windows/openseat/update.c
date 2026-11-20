/*
 * Updates: VISION asks VISION_UPDATE_BASE/version.txt for the newest version.
 * If it is newer, the setup window offers it; clicking downloads the setup,
 * checks its SHA-256 against version.txt, runs it silently and quits.  The
 * setup replaces the program and starts VISION again.
 *
 * version.txt:   version=1.2.0
 *                file=VISION-Setup.exe
 *                sha256=<64 hex digits>
 *                notes=One line about what is new
 */
#include "common.h"
#include "update.h"
#include "version.h"

#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <wchar.h>

static HWND g_notify;
static UpdateInfo g_found;

static void base_url(WCHAR *out, size_t chars)
{
    DWORD size = (DWORD)(chars * sizeof(WCHAR));
    /* Test and company setups can point elsewhere. */
    if (GetEnvironmentVariableW(L"VISION_UPDATE_URL", out, (DWORD)chars)) return;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\VISION", L"UpdateUrl", RRF_RT_REG_SZ, NULL, out, &size) == ERROR_SUCCESS && out[0])
        return;
    MultiByteToWideChar(CP_UTF8, 0, VISION_UPDATE_BASE, -1, out, (int)chars);
}

/* Download a URL into memory.  progress: posts WM_UPDATE_PROGRESS with 0-100. */
static BOOL fetch(const WCHAR *url, BYTE **data, DWORD *size, BOOL progress, WCHAR *why, size_t whyChars)
{
    URL_COMPONENTSW u;
    WCHAR host[256], path[2048];
    ZeroMemory(&u, sizeof(u));
    u.dwStructSize = sizeof(u);
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 2048;
    *data = NULL;
    *size = 0;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) { _snwprintf(why, whyChars, L"the update address is not valid"); return FALSE; }
    HINTERNET s = WinHttpOpen(L"VISION/" TEXT(VISION_VERSION), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    BOOL ok = r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, NULL);
    DWORD status = 0, len = sizeof(status), total = 0;
    if (ok) {
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &len, NULL);
        len = sizeof(total);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &total, &len, NULL);
    }
    if (!ok) _snwprintf(why, whyChars, L"no connection to the update site");
    else if (status != 200) { _snwprintf(why, whyChars, L"the update site answered %lu", status); ok = FALSE; }
    DWORD cap = 0, have = 0;
    BYTE *buf = NULL;
    while (ok) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(r, &avail)) { ok = FALSE; _snwprintf(why, whyChars, L"the download was interrupted"); break; }
        if (!avail) break;
        if (have + avail > 64u * 1024 * 1024) { ok = FALSE; _snwprintf(why, whyChars, L"the download is too large"); break; }
        if (have + avail > cap) {
            cap = (have + avail) * 2;
            BYTE *more = realloc(buf, cap);
            if (!more) { ok = FALSE; _snwprintf(why, whyChars, L"not enough memory"); break; }
            buf = more;
        }
        if (!WinHttpReadData(r, buf + have, avail, &got)) { ok = FALSE; _snwprintf(why, whyChars, L"the download was interrupted"); break; }
        have += got;
        if (progress && total && g_notify) PostMessageW(g_notify, WM_UPDATE_PROGRESS, (WPARAM)((have * 100ull) / total), 0);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!ok) { free(buf); return FALSE; }
    *data = buf;
    *size = have;
    return TRUE;
}

/* "1.10.2" > "1.9.9" */
int compare_versions(const char *a, const char *b)
{
    for (;;) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a != '.' && *b != '.') return 0;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
}

static BOOL parse(const char *text, UpdateInfo *out)
{
    ZeroMemory(out, sizeof(*out));
    const char *p = text;
    while (*p) {
        const char *end = strpbrk(p, "\r\n");
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char line[600];
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char *v = eq + 1;
            if (!strcmp(line, "version")) lstrcpynA(out->version, v, sizeof(out->version));
            else if (!strcmp(line, "file")) MultiByteToWideChar(CP_UTF8, 0, v, -1, out->file, 260);
            else if (!strcmp(line, "sha256")) lstrcpynA(out->sha256, v, sizeof(out->sha256));
            else if (!strcmp(line, "notes")) MultiByteToWideChar(CP_UTF8, 0, v, -1, out->notes, 300);
        }
        if (!end) break;
        p = end + 1;
    }
    /* only a plain file name: never a path somewhere else */
    BOOL plainName = out->file[0] && !wcspbrk(out->file, L"/\\:?#");
    return out->version[0] && plainName && strlen(out->sha256) == 64;
}

static DWORD WINAPI check_thread(LPVOID unused)
{
    (void)unused;
    WCHAR url[2100], why[200];
    BYTE *data;
    DWORD size;
    base_url(url, 2048);
    wcscat(url, L"version.txt");
    UpdateInfo *info = calloc(1, sizeof(UpdateInfo));
    if (!info) return 0;
    if (fetch(url, &data, &size, FALSE, why, 200)) {
        char *text = malloc(size + 1);
        if (text) {
            memcpy(text, data, size);
            text[size] = 0;
            if (parse(text, info) && compare_versions(info->version, VISION_VERSION) > 0) {
                g_found = *info;
                PostMessageW(g_notify, WM_UPDATE_FOUND, 0, 0);
            }
            free(text);
        }
        free(data);
    }
    free(info);
    return 0;
}

void update_check(HWND notify)
{
    g_notify = notify;
    HANDLE t = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

const UpdateInfo *update_found(void) { return g_found.version[0] ? &g_found : NULL; }

static void sha256_hex(const BYTE *data, DWORD size, char out[65])
{
    BYTE digest[32];
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    out[0] = 0;
    BOOL ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
              BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0 &&
              BCryptHashData(hash, (PUCHAR)data, size, 0) == 0 &&
              BCryptFinishHash(hash, digest, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return;
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", digest[i]);
}

static DWORD WINAPI install_thread(LPVOID unused)
{
    (void)unused;
    static WCHAR why[200];
    WCHAR url[2400], temp[MAX_PATH], target[MAX_PATH];
    BYTE *data;
    DWORD size;
    base_url(url, 2048);
    wcscat(url, g_found.file);
    if (!fetch(url, &data, &size, TRUE, why, 200)) { PostMessageW(g_notify, WM_UPDATE_FAILED, (WPARAM)why, 0); return 0; }
    char hex[65];
    sha256_hex(data, size, hex);
    if (_stricmp(hex, g_found.sha256) != 0) {
        free(data);
        _snwprintf(why, 200, L"the download was damaged (check failed)");
        PostMessageW(g_notify, WM_UPDATE_FAILED, (WPARAM)why, 0);
        return 0;
    }
    GetTempPathW(MAX_PATH, temp);
    _snwprintf(target, MAX_PATH, L"%lsVISION-Setup-%hs.exe", temp, g_found.version);
    HANDLE f = CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data, size, &written, NULL) && written == size;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(data);
    if (!ok) {
        _snwprintf(why, 200, L"the update could not be saved on this PC");
        PostMessageW(g_notify, WM_UPDATE_FAILED, (WPARAM)why, 0);
        return 0;
    }
    static WCHAR ready[MAX_PATH];
    lstrcpynW(ready, target, MAX_PATH);
    PostMessageW(g_notify, WM_UPDATE_READY, (WPARAM)ready, 0);
    return 0;
}

void update_download(HWND notify)
{
    g_notify = notify;
    HANDLE t = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* Run the checked setup silently.  A copy of VISION in the temp folder waits
 * for the setup to finish and then opens the new VISION, as the normal user. */
BOOL update_run(const WCHAR *setup)
{
    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = g_notify;
    info.lpVerb = L"open"; /* the setup asks Windows for administrator permission itself */
    info.lpFile = setup;
    info.lpParameters = L"/S";
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return FALSE;
    DWORD pid = info.hProcess ? GetProcessId(info.hProcess) : 0;
    if (info.hProcess) CloseHandle(info.hProcess);

    WCHAR self[MAX_PATH], helper[MAX_PATH], temp[MAX_PATH], cmd[MAX_PATH * 2 + 64];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    GetTempPathW(MAX_PATH, temp);
    _snwprintf(helper, MAX_PATH, L"%lsVISION-restart.exe", temp); /* not "VISION.exe": the setup closes those */
    if (!pid || !CopyFileW(self, helper, FALSE)) return TRUE;  /* updated anyway; open VISION by hand */
    _snwprintf(cmd, MAX_PATH * 2 + 64, L"\"%ls\" --relaunch %lu \"%ls\"", helper, pid, self);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return TRUE;
}

/* "--relaunch <setup pid> <path to VISION.exe>": wait for the setup, then open VISION. */
int update_relaunch(const WCHAR *args)
{
    WCHAR path[MAX_PATH] = L"";
    unsigned long pid = 0;
    const WCHAR *p = wcsstr(args, L"--relaunch");
    if (!p) return 1;
    p += 10;
    pid = wcstoul(p, (WCHAR **)&p, 10);
    const WCHAR *q = wcschr(p, L'"');
    if (q) {
        const WCHAR *end = wcschr(q + 1, L'"');
        if (end && (size_t)(end - q - 1) < MAX_PATH) { memcpy(path, q + 1, (end - q - 1) * sizeof(WCHAR)); path[end - q - 1] = 0; }
    }
    if (!path[0]) return 1;
    HANDLE setup = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    if (setup) { WaitForSingleObject(setup, 10 * 60 * 1000); CloseHandle(setup); }
    Sleep(800);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR cmd[MAX_PATH + 4];
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    _snwprintf(cmd, MAX_PATH + 4, L"\"%ls\"", path);
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

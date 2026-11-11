/* Update check and one-button update.
 *
 * The game can't be shipped ready-made (it is built from the player's own ROM), so an update is
 * a new builder: the game downloads the release's MagicalPopn-Builder.zip, unpacks it, and
 * starts that builder on the ROM embedded in this executable, with this game's folder as the
 * output. The builder rebuilds the game there and starts it again.
 *
 *   check     HEAD github.com/<repo>/releases/latest  -> the redirect names the newest tag
 *   download  github.com/<repo>/releases/download/<tag>/MagicalPopn-Builder.zip
 *   unpack    Windows' own tar.exe (Windows 10 1803 and later), into
 *             %LOCALAPPDATA%\MagicalPopnBuilder\update
 *
 * Only that one repository over HTTPS is ever contacted, and nothing about the player or the
 * game is sent. Test switches: POPN_UPDATE_PRETEND=VERSION (this build's version for the check),
 * POPN_UPDATE_ZIP=FILE (use that builder zip instead of downloading). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "update.h"

#define UPD_HOST  L"github.com"
#define UPD_REPO  L"/looking-glassinsects/MagicalPopnRecomp"
#define UPD_ASSET L"MagicalPopn-Builder.zip"
#define UPD_AGENT L"MagicalPopn-updater"

enum { ST_IDLE, ST_CHECKING, ST_AVAILABLE, ST_NONE, ST_WORKING, ST_READY, ST_FAILED };
static volatile LONG g_state = ST_IDLE, g_pct;
static char g_current[32], g_latest[32];
static wchar_t g_game_dir[MAX_PATH], g_upd_dir[MAX_PATH];
static const uint8_t *g_rom; static size_t g_rom_size;

/* "v0.9.5" / "0.9.5" -> up to four numbers; 0 if it isn't a version */
static int parse_version(const char *s, int v[4]) {
    v[0] = v[1] = v[2] = v[3] = 0;
    if (*s == 'v' || *s == 'V') s++;
    if (*s < '0' || *s > '9') return 0;
    for (int i = 0; i < 4; i++) {
        char *end; v[i] = (int)strtol(s, &end, 10);
        if (end == s) return 0;
        if (*end != '.') return *end == 0;
        s = end + 1;
    }
    return 1;
}
static int is_newer(const char *latest, const char *current) {
    int a[4], b[4];
    if (!parse_version(latest, a) || !parse_version(current, b)) return 0;
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

static int file_exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* delete a folder and everything in it (only ever called on g_upd_dir) */
static void remove_tree(const wchar_t *dir) {
    wchar_t pat[MAX_PATH + 8]; WIN32_FIND_DATAW fd;
    if (_snwprintf(pat, MAX_PATH + 8, L"%ls\\*", dir) < 0) return;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH * 2];
            if (_snwprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName) < 0) continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void builder_path(wchar_t *out, int n) { _snwprintf(out, n, L"%ls\\MagicalPopn-Builder\\Builder.exe", g_upd_dir); out[n - 1] = 0; }

/* a finished update leaves its builder behind: remove it, unless it is running right now */
static void remove_leftovers(void) {
    wchar_t exe[MAX_PATH * 2]; builder_path(exe, MAX_PATH * 2);
    if (!file_exists(g_upd_dir)) return;
    if (file_exists(exe) && !DeleteFileW(exe)) return;
    remove_tree(g_upd_dir);
}

/* ---------------------------------------------------------------- network */
static HINTERNET open_request(HINTERNET *session, HINTERNET *conn, const wchar_t *verb, const wchar_t *path, int follow) {
    *conn = NULL;
    *session = WinHttpOpen(UPD_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!*session) return NULL;
    WinHttpSetTimeouts(*session, 8000, 8000, 15000, 30000);
    *conn = WinHttpConnect(*session, UPD_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!*conn) return NULL;
    HINTERNET r = WinHttpOpenRequest(*conn, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!r) return NULL;
    if (!follow) { DWORD o = WINHTTP_DISABLE_REDIRECTS; WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &o, sizeof o); }
    if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, NULL)) { WinHttpCloseHandle(r); return NULL; }
    return r;
}
static void close_request(HINTERNET session, HINTERNET conn, HINTERNET r) {
    if (r) WinHttpCloseHandle(r);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
}

/* the newest release's tag ("v0.9.5"), from where /releases/latest redirects to */
static int latest_tag(char *out, int size) {
    HINTERNET s, c, r = open_request(&s, &c, L"HEAD", UPD_REPO L"/releases/latest", 0);
    int ok = 0;
    if (r) {
        wchar_t loc[512]; DWORD n = sizeof loc;
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &n, WINHTTP_NO_HEADER_INDEX)) {
            const wchar_t *t = wcsstr(loc, UPD_REPO L"/releases/tag/");
            if (t) {
                t += wcslen(UPD_REPO L"/releases/tag/");
                int i = 0;
                for (; t[i] && i < size - 1; i++) {
                    wchar_t ch = t[i];
                    if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'.' || ch == L'-')) break;
                    out[i] = (char)ch;
                }
                out[i] = 0;
                ok = i > 0 && t[i] == 0;
            }
        }
    }
    close_request(s, c, r);
    return ok;
}

static int download(const char *tag, const wchar_t *dest) {
    wchar_t path[256], wtag[40];
    MultiByteToWideChar(CP_ACP, 0, tag, -1, wtag, 40);
    _snwprintf(path, 256, UPD_REPO L"/releases/download/%ls/" UPD_ASSET, wtag); path[255] = 0;
    HINTERNET s, c, r = open_request(&s, &c, L"GET", path, 1);
    int ok = 0;
    if (r) {
        DWORD status = 0, n = sizeof status, total = 0;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &n, WINHTTP_NO_HEADER_INDEX);
        n = sizeof total;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &n, WINHTTP_NO_HEADER_INDEX);
        HANDLE f = status == 200 ? CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        if (f != INVALID_HANDLE_VALUE) {
            static uint8_t buf[1 << 16];
            unsigned long long got = 0;
            for (;;) {
                DWORD rd = 0, wr = 0;
                if (!WinHttpReadData(r, buf, sizeof buf, &rd)) break;
                if (rd == 0) { ok = got > (1 << 20) && (!total || got == total); break; }   /* the real file is ~90 MB */
                if (!WriteFile(f, buf, rd, &wr, NULL) || wr != rd) break;
                got += rd;
                if (total) InterlockedExchange(&g_pct, (LONG)(got * 90 / total));
            }
            CloseHandle(f);
            if (!ok) DeleteFileW(dest);
        }
    }
    close_request(s, c, r);
    return ok;
}

/* run a program hidden and wait for it; 1 if it exited with 0 */
static int run_wait(wchar_t *cmdline, const wchar_t *cwd, DWORD timeout_ms) {
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) return 0;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}

/* ---------------------------------------------------------------- threads */
static DWORD WINAPI check_thread(void *arg) {
    (void)arg;
    char tag[32];
    if (latest_tag(tag, sizeof tag) && is_newer(tag, g_current)) {
        snprintf(g_latest, sizeof g_latest, "%s", tag);
        InterlockedExchange(&g_state, ST_AVAILABLE);
    } else {
        remove_leftovers();
        InterlockedExchange(&g_state, ST_NONE);
    }
    return 0;
}

static DWORD WINAPI work_thread(void *arg) {
    (void)arg;
    wchar_t zip[MAX_PATH * 2], rom[MAX_PATH * 2], exe[MAX_PATH * 2], parent[MAX_PATH];
    int ok = 0;
    remove_tree(g_upd_dir);
    wcsncpy(parent, g_upd_dir, MAX_PATH); parent[MAX_PATH - 1] = 0;
    { wchar_t *sl = wcsrchr(parent, L'\\'); if (sl) *sl = 0; }
    CreateDirectoryW(parent, NULL); CreateDirectoryW(g_upd_dir, NULL);
    _snwprintf(zip, MAX_PATH * 2, L"%ls\\builder.zip", g_upd_dir); zip[MAX_PATH * 2 - 1] = 0;
    _snwprintf(rom, MAX_PATH * 2, L"%ls\\rom.sfc", g_upd_dir); rom[MAX_PATH * 2 - 1] = 0;
    builder_path(exe, MAX_PATH * 2);

    const wchar_t *local = _wgetenv(L"POPN_UPDATE_ZIP");
    int have_zip = local ? CopyFileW(local, zip, FALSE) != 0 : download(g_latest, zip);
    InterlockedExchange(&g_pct, 90);
    if (have_zip) {
        wchar_t sys[MAX_PATH], cmd[MAX_PATH * 2];
        GetSystemDirectoryW(sys, MAX_PATH);
        _snwprintf(cmd, MAX_PATH * 2, L"\"%ls\\tar.exe\" -xf builder.zip", sys); cmd[MAX_PATH * 2 - 1] = 0;
        if (run_wait(cmd, g_upd_dir, 10 * 60 * 1000) && file_exists(exe)) {
            DeleteFileW(zip);
            InterlockedExchange(&g_pct, 98);
            HANDLE f = CreateFileW(rom, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                ok = WriteFile(f, g_rom, (DWORD)g_rom_size, &wr, NULL) && wr == g_rom_size;
                CloseHandle(f);
            }
        }
    }
    if (!ok) remove_tree(g_upd_dir);
    InterlockedExchange(&g_state, ok ? ST_READY : ST_FAILED);
    return 0;
}

static void start_thread(LPTHREAD_START_ROUTINE fn) {
    HANDLE t = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_state, ST_FAILED);
}

/* ---------------------------------------------------------------- interface */
void update_init(const char *version, const char *game_dir, const uint8_t *rom, size_t rom_size) {
    const char *pretend = getenv("POPN_UPDATE_PRETEND");
    snprintf(g_current, sizeof g_current, "%s", pretend ? pretend : version);
    MultiByteToWideChar(CP_ACP, 0, game_dir, -1, g_game_dir, MAX_PATH);
    size_t n = wcslen(g_game_dir);
    while (n && g_game_dir[n - 1] == L'\\') g_game_dir[--n] = 0;      /* a backslash before a closing quote would escape it */
    g_rom = rom; g_rom_size = rom_size;
    wchar_t local[MAX_PATH];
    DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (got == 0 || got >= MAX_PATH || _snwprintf(g_upd_dir, MAX_PATH, L"%ls\\MagicalPopnBuilder\\update", local) < 0) g_upd_dir[0] = 0;
    g_upd_dir[MAX_PATH - 1] = 0;
}

void update_check(void) {
    if (!g_upd_dir[0] || InterlockedCompareExchange(&g_state, ST_CHECKING, ST_IDLE) != ST_IDLE) return;
    start_thread(check_thread);
}

int update_available(char *version, int size) {
    if (InterlockedCompareExchange(&g_state, ST_NONE, ST_AVAILABLE) != ST_AVAILABLE) return 0;
    snprintf(version, size, "%s", g_latest[0] == 'v' ? g_latest + 1 : g_latest);
    return 1;
}

void update_begin(void) {
    InterlockedExchange(&g_pct, 0);
    InterlockedExchange(&g_state, ST_WORKING);
    start_thread(work_thread);
}

int update_progress(void) {
    LONG s = g_state;
    if (s == ST_READY) return 100;
    if (s != ST_WORKING) return -1;
    LONG p = g_pct;
    return p < 0 ? 0 : p > 99 ? 99 : (int)p;
}

int update_launch(void) {
    wchar_t exe[MAX_PATH * 2], dir[MAX_PATH * 2], cmd[MAX_PATH * 5];
    builder_path(exe, MAX_PATH * 2);
    _snwprintf(dir, MAX_PATH * 2, L"%ls\\MagicalPopn-Builder", g_upd_dir); dir[MAX_PATH * 2 - 1] = 0;
    _snwprintf(cmd, MAX_PATH * 5, L"\"%ls\" \"%ls\\rom.sfc\" --out \"%ls\" --update", exe, g_upd_dir, g_game_dir);
    cmd[MAX_PATH * 5 - 1] = 0;
    /* the test switches are for this run only: the rebuilt game must not inherit them */
    SetEnvironmentVariableW(L"POPN_UPDATE_PRETEND", NULL); SetEnvironmentVariableW(L"POPN_UPDATE_ZIP", NULL);
    SetEnvironmentVariableW(L"POPN_UPDATE_AUTO", NULL);
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) return 0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 1;
}

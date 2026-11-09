#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <GL/gl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>
#include "platform.h"
#include "settings.h"
#include "menu.h"
#include "pad.h"

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int);

static HWND g_wnd;
static HDC g_dc;
static HGLRC g_gl;
static PFNWGLSWAPINTERVALEXTPROC p_wglSwapIntervalEXT;
static int g_quit, g_flags, g_is_fullscreen, g_swap = -1;
static WINDOWPLACEMENT g_wp = {sizeof(WINDOWPLACEMENT)};
static LARGE_INTEGER g_freq;
static DWORD g_last_mouse;

/* frame */
static uint32_t g_frame[544 * 224];
static int g_frame_w = 256;

/* GL textures */
static GLuint t_game, t_scan, t_text_fps[2], t_text_toast[2], t_menu;   /* text: two each, used in turn */
static int t_menu_w, t_menu_h;

/* fps / toast */
static int g_fps_frames, g_emu_frames; static double g_fps, g_speed; static DWORD g_fps_tick;
static wchar_t g_toast[128]; static DWORD g_toast_until; static int g_toast_dirty;
static void text_tick(void);
static int g_fps_tw, g_fps_th, g_toast_tw, g_toast_th;

/* key queue for the menu */
static int g_keyq[64], g_kh, g_kt;
static void push_key(int vk) { int n = (g_kt + 1) % 64; if (n != g_kh) { g_keyq[g_kt] = vk; g_kt = n; } }
int plat_pop_key(int *vk) { if (g_kh == g_kt) return 0; *vk = g_keyq[g_kh]; g_kh = (g_kh + 1) % 64; return 1; }
void plat_clear_keys(void) { g_kh = g_kt; }
int plat_take_flags(void) { int f = g_flags; g_flags = 0; return f; }

void plat_exe_dir(char *out, int size) {
    GetModuleFileNameA(NULL, out, size);
    char *s = strrchr(out, '\\'); if (s) *s = 0;
}

double plat_now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_freq.QuadPart; }

static HANDLE g_timer;   /* high-resolution waitable timer (Windows 10 1803+), else Sleep */
#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL _Thread_local
#endif
static THREAD_LOCAL HANDLE t_timer;     /* one timer per thread (main + display thread) */
static THREAD_LOCAL int t_timer_made;

void plat_sleep_until_ms(double t) {
    if (!t_timer_made) {
        t_timer_made = 1;
        t_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    }
    HANDLE g_timer = t_timer;
    for (;;) {
        double left = t - plat_now_ms();
        if (left <= 0) return;
        if (left > 1.2) {
            double ms = left - 1.0;
            if (g_timer) {
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(ms * 10000.0);
                SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE);
                WaitForSingleObject(g_timer, INFINITE);
            } else Sleep((DWORD)ms);
        } else YieldProcessor();          /* spin the last ~1 ms for precision */
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    pad_message(msg, wp, lp);
    switch (msg) {
    case WM_CLOSE: g_quit = 1; return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { plat_toggle_fullscreen(); return 0; }   /* Alt+Enter */
        if (wp == VK_F10) return 0;
        break;
    case WM_KEYDOWN: {
        int repeat = (lp >> 30) & 1;
        if (wp == VK_F1 && !repeat) { g_flags |= PF_MENU; return 0; }
        if (wp == VK_F11 && !repeat) { plat_toggle_fullscreen(); return 0; }
        if (wp == VK_F12 && !repeat) { g_flags |= PF_SCREENSHOT; return 0; }
        if (!menu_is_open()) {
            if (wp == VK_ESCAPE && !repeat) { g_flags |= PF_MENU; return 0; }
            if (wp == VK_F5 && !repeat) { g_flags |= PF_SAVE; return 0; }
            if (wp == VK_F9 && !repeat) { g_flags |= PF_LOAD; return 0; }
            if (wp == VK_F6 && !repeat) { g_flags |= PF_SLOT_PREV; return 0; }
            if (wp == VK_F7 && !repeat) { g_flags |= PF_SLOT_NEXT; return 0; }
            if (wp == 'P' && !repeat) { g_flags |= PF_PAUSE; return 0; }
        }
        push_key((int)wp);
        return 0;
    }
    case WM_MOUSEMOVE:
        g_last_mouse = GetTickCount();
        if (menu_is_open()) menu_mouse_move((short)LOWORD(lp), (short)HIWORD(lp));
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        if (menu_is_open()) {
            menu_mouse_move((short)LOWORD(lp), (short)HIWORD(lp));
            push_key(msg == WM_LBUTTONDOWN ? MENU_VK_CLICK_L : MENU_VK_CLICK_R);
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_is_fullscreen && GetTickCount() - g_last_mouse > 1500) { SetCursor(NULL); return TRUE; }
        break;
    case WM_MOVE:
        if (!g_is_fullscreen && !IsIconic(h)) {
            RECT r; GetWindowRect(h, &r); g_set.win_x = r.left; g_set.win_y = r.top;
        }
        break;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); plat_redraw(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: plat_redraw(); break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void client_size_for_scale(int *w, int *h) {
    double base_w = (256 + 2 * settings_ws_extra()) * settings_par();
    *w = (int)(base_w * g_set.scale + 0.5); *h = 224 * g_set.scale;
}

static GLuint make_tex(int w, int h, int linear) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
    return t;
}

int plat_init(const char *title) {
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);
    g_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"MagicalPopn";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = NULL;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    int cw, ch; client_size_for_scale(&cw, &ch);
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (g_set.win_x != -32000 && g_set.win_y != -32000) {       /* restore position if it's on a monitor */
        POINT pt = {g_set.win_x + 40, g_set.win_y + 20};
        if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) { x = g_set.win_x; y = g_set.win_y; }
    }
    wchar_t wtitle[128]; MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 128);
    g_wnd = CreateWindowW(wc.lpszClassName, wtitle, WS_OVERLAPPEDWINDOW, x, y, r.right - r.left, r.bottom - r.top,
                          NULL, NULL, inst, NULL);
    if (!g_wnd) return 0;

    g_dc = GetDC(g_wnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 0; pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd)) return 0;
    g_gl = wglCreateContext(g_dc);
    if (!g_gl || !wglMakeCurrent(g_dc, g_gl)) return 0;
    p_wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");

    t_game = make_tex(1024, 256, 0);
    t_text_fps[0] = make_tex(512, 64, 1); t_text_fps[1] = make_tex(512, 64, 1);
    t_text_toast[0] = make_tex(1024, 64, 1); t_text_toast[1] = make_tex(1024, 64, 1);
    /* scanline mask: 1x2, second row darkened (premultiplied alpha) */
    glGenTextures(1, &t_scan); glBindTexture(GL_TEXTURE_2D, t_scan);
    static const uint32_t scan[2] = {0x00000000, 0x60000000};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 2, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, scan);

    {
        char ini[MAX_PATH + 32]; plat_exe_dir(ini, MAX_PATH);
        strcat(ini, "\\settings.ini");
        pad_init(g_wnd, ini);
    }
    ShowWindow(g_wnd, SW_SHOW);
    if (g_set.fullscreen) { g_set.fullscreen = 0; plat_toggle_fullscreen(); }
    g_fps_tick = GetTickCount();
    g_last_mouse = GetTickCount();
    return 1;
}

void plat_shutdown(void) {
    plat_display_thread(NULL);
    timeEndPeriod(1);
    if (g_timer) CloseHandle(g_timer);
    if (g_gl) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_gl); }
    if (g_wnd) DestroyWindow(g_wnd);
}

static HANDLE g_dt;                     /* display thread (motion smoothing) owns GL while running */

void plat_set_swap_interval(int n) {
    if (g_dt || n == g_swap) return;
    g_swap = n;
    if (p_wglSwapIntervalEXT) p_wglSwapIntervalEXT(n);
}

double plat_refresh_hz(void) {
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&mi)) return 60.0;
    DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) return 60.0;
    double hz = dm.dmDisplayFrequency;
    if (hz == 59 || hz == 119 || hz == 143 || hz == 239) hz += 0.94;   /* NTSC-style fractional rates */
    return hz;
}

int plat_minimized(void) { return IsIconic(g_wnd); }

/* ---------------------------------------------------------------- input */
static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
int plat_focused(void) { return GetForegroundWindow() == g_wnd; }

static uint32_t g_pad_prev;
static void poll_pad_edges(uint32_t w) {
    uint32_t down = w & ~g_pad_prev;
    g_pad_prev = w;
    if (menu_pad_setup_active()) return;                /* the setup reads the controller itself */
    if (down & VPB(VP_MENU)) g_flags |= PF_MENU;
    if (!menu_is_open()) return;
    if (down & VPB(VP_UP)) push_key(PAD_VK_BASE | VK_UP);
    if (down & VPB(VP_DOWN)) push_key(PAD_VK_BASE | VK_DOWN);
    if (down & VPB(VP_LEFT)) push_key(PAD_VK_BASE | VK_LEFT);
    if (down & VPB(VP_RIGHT)) push_key(PAD_VK_BASE | VK_RIGHT);
    if (down & VPB(VP_L)) push_key(PAD_VK_TAB_PREV);
    if (down & VPB(VP_R)) push_key(PAD_VK_TAB_NEXT);
    if (down & (VPB(VP_SOUTH) | VPB(VP_START))) push_key(PAD_VK_BASE | VK_RETURN);
    if (down & (VPB(VP_EAST) | VPB(VP_SELECT))) push_key(PAD_VK_BASE | VK_ESCAPE);
}

int plat_pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    poll_pad_edges(pad_state());
    text_tick();
    return !g_quit;
}

uint16_t plat_buttons(void) {
    uint16_t m = 0;
    if (plat_focused())
        for (int i = 0; i < BTN_COUNT; i++) if (g_set.keys[i] && key(g_set.keys[i])) m |= (uint16_t)(1 << i);
    uint32_t p = pad_state();
    static const int MAP[][2] = {{VP_SOUTH, BTN_B}, {VP_EAST, BTN_A}, {VP_WEST, BTN_Y}, {VP_NORTH, BTN_X},   /* positions */
                                 {VP_L, BTN_L}, {VP_R, BTN_R}, {VP_SELECT, BTN_SELECT}, {VP_START, BTN_START},
                                 {VP_UP, BTN_UP}, {VP_DOWN, BTN_DOWN}, {VP_LEFT, BTN_LEFT}, {VP_RIGHT, BTN_RIGHT}};
    for (int i = 0; i < (int)(sizeof MAP / sizeof MAP[0]); i++) if (p & VPB(MAP[i][0])) m |= (uint16_t)(1 << MAP[i][1]);
    if ((m & (1 << BTN_UP)) && (m & (1 << BTN_DOWN))) m &= (uint16_t)~(1 << BTN_DOWN);
    if ((m & (1 << BTN_LEFT)) && (m & (1 << BTN_RIGHT))) m &= (uint16_t)~(1 << BTN_RIGHT);
    return m;
}

int plat_fast_forward(void) {
    if (!plat_focused()) return 0;
    return key(VK_TAB) || (pad_state() & VPB(VP_FF)) != 0;
}

/* ---------------------------------------------------------------- display */
void plat_apply_display(void) {
    if (g_is_fullscreen != g_set.fullscreen) { plat_toggle_fullscreen(); return; }
    if (!g_is_fullscreen) {
        int cw, ch; client_size_for_scale(&cw, &ch);
        RECT r = {0, 0, cw, ch};
        AdjustWindowRect(&r, (DWORD)GetWindowLongW(g_wnd, GWL_STYLE), FALSE);
        SetWindowPos(g_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    plat_redraw();
}

void plat_toggle_fullscreen(void) {
    DWORD style = (DWORD)GetWindowLongW(g_wnd, GWL_STYLE);
    if (!g_is_fullscreen) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(g_wnd, &g_wp) && GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(g_wnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        g_is_fullscreen = 1;
    } else {
        SetWindowLongW(g_wnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(g_wnd, &g_wp);
        SetWindowPos(g_wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_is_fullscreen = 0;
    }
    g_set.fullscreen = g_is_fullscreen;
    plat_redraw();
}

void plat_note_emulated_frame(void) { g_emu_frames++; }

void plat_toast(const char *utf8) {
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, g_toast, 128);
    g_toast_until = GetTickCount() + 2200;
    g_toast_dirty = 1;
    text_tick();                        /* draw its text now, so it never shows the old one */
}

static void game_rect(int cw, int ch, RECT *out) {
    double ar = g_set.aspect == ASPECT_STRETCH ? (double)cw / ch : g_frame_w * settings_par() / 224.0;
    int w, h;
    if (g_set.integer_scale && g_set.aspect != ASPECT_STRETCH) {
        int n = ch / 224; if (n < 1) n = 1;
        while (n > 1 && (int)(224 * n * ar + 0.5) > cw) n--;
        h = 224 * n; w = (int)(h * ar + 0.5);
    } else if (cw / ar <= ch) { w = cw; h = (int)(cw / ar + 0.5); }
    else { h = ch; w = (int)(ch * ar + 0.5); }
    out->left = (cw - w) / 2; out->top = (ch - h) / 2; out->right = out->left + w; out->bottom = out->top + h;
}

/* Text overlays (FPS counter, messages). Drawing text with GDI takes ~2 ms, too long for the
 * motion-smoothing display thread, which has to present on time every refresh. So text is
 * drawn into a bitmap on the main thread (text_tick, from plat_pump) and the display side
 * only uploads the finished bitmap (text_upload), which is quick. */
typedef struct {
    int tw, th, font_px;
    uint8_t *bits;            /* finished bitmap, premultiplied white on transparent */
    int w, h;                 /* size of the text in it */
    volatile LONG ready;      /* a new bitmap waits to be uploaded */
    int cur;                  /* which of its two textures is shown */
} TextImg;
static TextImg g_txt_fps = {512, 64, 18}, g_txt_toast = {1024, 64, 20};
static CRITICAL_SECTION g_txt_cs;

static void text_render(TextImg *t, const wchar_t *text) {
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = t->tw; bi.bmiHeader.biHeight = -t->th;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits; HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm) { DeleteDC(dc); return; }
    HGDIOBJ ob = SelectObject(dc, bm);
    memset(bits, 0, (size_t)t->tw * t->th * 4);
    HFONT f = CreateFontW(-t->font_px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                          g_set.language == LANG_JA ? L"Yu Gothic UI" : L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    SIZE sz; GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    TextOutW(dc, 2, 2, text, (int)wcslen(text));
    GdiFlush();
    uint8_t *p = (uint8_t *)bits;
    for (int i = 0; i < t->tw * t->th; i++) { uint8_t a = p[i * 4 + 1]; p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = a; p[i * 4 + 3] = a; }
    EnterCriticalSection(&g_txt_cs);
    if (!t->bits) t->bits = malloc((size_t)t->tw * t->th * 4);
    if (t->bits) {
        memcpy(t->bits, bits, (size_t)t->tw * t->th * 4);
        t->w = sz.cx + 4 < t->tw ? sz.cx + 4 : t->tw; t->h = sz.cy + 4 < t->th ? sz.cy + 4 : t->th;
        t->ready = 1;
    }
    LeaveCriticalSection(&g_txt_cs);
    SelectObject(dc, of); DeleteObject(f); SelectObject(dc, ob); DeleteObject(bm); DeleteDC(dc);
}

/* on whichever thread draws (it owns GL): upload a finished bitmap; returns 1 if t has text */
static int text_upload(TextImg *t, const GLuint tex[2], int *w, int *h) {
    if (t->ready) {
        /* into the texture not in use: the GPU may still be drawing the last picture with the
         * other one, and writing to it would make the driver wait for that */
        EnterCriticalSection(&g_txt_cs);
        glBindTexture(GL_TEXTURE_2D, tex[t->cur ^ 1]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->tw, t->th, GL_BGRA_EXT, GL_UNSIGNED_BYTE, t->bits);
        t->cur ^= 1;
        *w = t->w; *h = t->h; t->ready = 0;
        LeaveCriticalSection(&g_txt_cs);
    }
    return *w > 0;
}

/* main thread, every pump: redo the FPS text once a second and a new message's text */
static DWORD g_txt_fps_tick;
static void text_tick(void) {
    static int init;
    if (!init) { InitializeCriticalSection(&g_txt_cs); init = 1; }
    DWORD now = GetTickCount();
    if (g_set.show_fps && now - g_txt_fps_tick >= 1000) {
        g_txt_fps_tick = now;
        wchar_t b[96]; swprintf(b, 96, L"%.0f fps    %.0f%% speed    %.0f Hz", g_fps, g_speed, plat_refresh_hz());
        text_render(&g_txt_fps, b);
    }
    if (g_toast_dirty) { g_toast_dirty = 0; text_render(&g_txt_toast, g_toast); }
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
    glEnd();
}

/* Menu: drawn by menu.c into a DIB with premultiplied alpha, over the dimmed game. */
static void draw_menu(int cw, int ch) {
    int tw = 1, th = 1;
    while (tw < cw) tw <<= 1;
    while (th < ch) th <<= 1;
    if (!t_menu || tw != t_menu_w || th != t_menu_h) {
        if (t_menu) glDeleteTextures(1, &t_menu);
        t_menu = make_tex(tw, th, 0); t_menu_w = tw; t_menu_h = th;
    }
    /* one bitmap, kept while the size stays; only the panel's area is cleared, drawn and
     * uploaded each time (everything else stays transparent) */
    static HDC dc; static HBITMAP bm; static uint8_t *bits; static int bw, bh, full = 1; static RECT last;
    static GLuint last_tex;
    if (!dc || cw != bw || ch != bh) {
        if (bm) DeleteObject(bm);
        if (!dc) dc = CreateCompatibleDC(NULL);
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        if (!bm) { bits = NULL; bw = bh = 0; return; }
        SelectObject(dc, bm);
        bw = cw; bh = ch; full = 1;
        memset(bits, 0, (size_t)cw * ch * 4);
        SetRect(&last, 0, 0, 0, 0);
    }
    if (last_tex != t_menu) { full = 1; last_tex = t_menu; }
    for (int y = last.top; y < last.bottom; y++)
        memset(bits + ((size_t)y * cw + last.left) * 4, 0, (size_t)(last.right - last.left) * 4);
    menu_draw(dc, bits, cw, ch);
    GdiFlush();
    RECT now; menu_panel_rect(&now);
    RECT up; UnionRect(&up, &now, &last);
    if (full) SetRect(&up, 0, 0, cw, ch);
    glBindTexture(GL_TEXTURE_2D, t_menu);
    if (up.right > up.left && up.bottom > up.top) {
        glPixelStorei(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, cw);
        glTexSubImage2D(GL_TEXTURE_2D, 0, up.left, up.top, up.right - up.left, up.bottom - up.top, GL_BGRA_EXT,
                        GL_UNSIGNED_BYTE, bits + ((size_t)up.top * cw + up.left) * 4);
        glPixelStorei(0x0CF2, 0);
    }
    last = now; full = 0;
    /* dim the game, then the menu */
    glDisable(GL_TEXTURE_2D);
    glColor4f(0, 0, 0, 0.45f);
    quad(0, 0, (float)cw, (float)ch, 0, 0, 1, 1);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, t_menu);
    quad(0, 0, (float)cw, (float)ch, 0, 0, (float)cw / tw, (float)ch / th);
}

static void draw_all(void) {
    RECT cr; GetClientRect(g_wnd, &cr);
    int cw = cr.right, ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return;
    glViewport(0, 0, cw, ch);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, cw, ch, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    RECT gr; game_rect(cw, ch, &gr);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, t_game);
    GLint filt = g_set.smooth ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glColor4f(1, 1, 1, 1);
    quad((float)gr.left, (float)gr.top, (float)gr.right, (float)gr.bottom, 0, 0, g_frame_w / 1024.0f, 224 / 256.0f);
    glEnable(GL_BLEND);
    if (g_set.scanlines && gr.bottom - gr.top >= 448) {
        glBindTexture(GL_TEXTURE_2D, t_scan);
        quad((float)gr.left, (float)gr.top, (float)gr.right, (float)gr.bottom, 0, 0, 1, 224);
    }

    DWORD now = GetTickCount();
    if (now - g_fps_tick >= 1000) {
        double secs = (now - g_fps_tick) / 1000.0;
        g_fps = g_fps_frames / secs; g_speed = g_emu_frames / secs / 60.0988 * 100.0;
        g_fps_frames = g_emu_frames = 0; g_fps_tick = now;
    }
    text_upload(&g_txt_fps, t_text_fps, &g_fps_tw, &g_fps_th);
    text_upload(&g_txt_toast, t_text_toast, &g_toast_tw, &g_toast_th);
    if (g_set.show_fps && !menu_is_open() && g_fps_tw) {
        glBindTexture(GL_TEXTURE_2D, t_text_fps[g_txt_fps.cur]);
        glColor4f(0, 0, 0, 0.8f); quad(9, 9, 9.0f + g_fps_tw, 9.0f + g_fps_th, 0, 0, g_fps_tw / 512.0f, g_fps_th / 64.0f);
        glColor4f(1, 1, 1, 1); quad(8, 8, 8.0f + g_fps_tw, 8.0f + g_fps_th, 0, 0, g_fps_tw / 512.0f, g_fps_th / 64.0f);
    }
    if (now < g_toast_until) {
        float x = (float)(cw - g_toast_tw - 14), y = (float)(ch - g_toast_th - 12);
        float a = (g_toast_until - now) < 300 ? (g_toast_until - now) / 300.0f : 1.0f;
        glBindTexture(GL_TEXTURE_2D, t_text_toast[g_txt_toast.cur]);
        glColor4f(0, 0, 0, 0.8f * a); quad(x + 1, y + 1, x + 1 + g_toast_tw, y + 1 + g_toast_th, 0, 0, g_toast_tw / 1024.0f, g_toast_th / 64.0f);
        glColor4f(a, a, a, a); quad(x, y, x + g_toast_tw, y + g_toast_th, 0, 0, g_toast_tw / 1024.0f, g_toast_th / 64.0f);
    }
    if (menu_is_open()) draw_menu(cw, ch);
    SwapBuffers(g_dc);
}

void plat_present(const uint8_t *px, int width) {
    if (g_dt || width < 1 || width > 544) return;
    memcpy(g_frame, px, (size_t)width * 224 * 4);
    g_frame_w = width;
    glBindTexture(GL_TEXTURE_2D, t_game);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 224, GL_BGRA_EXT, GL_UNSIGNED_BYTE, g_frame);
    g_fps_frames++;
    if (IsIconic(g_wnd)) return;
    draw_all();
    glFinish();                 /* don't let the driver queue frames ahead: lower latency */
}

double plat_shown_fps(void) { return g_fps; }

void plat_redraw(void) {
    if (g_dt || !g_gl || IsIconic(g_wnd)) return;
    draw_all();
}

/* ---- display thread: a new picture on every refresh (motion smoothing) ----
 * `render(when, out)` returns the picture for the refresh at time `when` (ms). The thread
 * presents with vsync, so each loop is one refresh; the main thread keeps the window, input
 * and emulation, and must not use GL while the thread runs. */
static volatile LONG g_dt_run;
static int (*g_dt_render)(double, uint8_t *);
static uint8_t g_dt_buf[544 * 224 * 4];
static volatile LONG g_dt_fps = 120;

void plat_display_rate(int fps) { g_dt_fps = fps < 61 ? 61 : fps; }

/* refreshes per picture: the most that still gives (about) 120 fps, so a 240 Hz screen shows
 * a new picture every 2nd refresh, a 120 Hz one every refresh, and a 144 Hz one every refresh
 * (120 can't divide it evenly; every refresh is the smooth choice there) */
static int dt_refreshes_per_picture(double hz) {
    int k = 1;
    while (k < 8 && hz / (k + 1) >= g_dt_fps * 0.9) k++;
    return k;
}

/* Is refresh number idx due a new picture? Never more than 120 a second: screens that divide
 * 120 evenly use every k-th refresh; others (144 Hz, 165 Hz) skip refreshes as evenly as they
 * can, so a 144 Hz screen gets a new picture on 5 refreshes out of every 6. */
static int dt_due(long long idx, double hz, int k) {
    if (hz < 125.0 || fabs(hz / k - 120.0) < 3.0) return idx % k == 0;
    return floor((double)(idx + 1) * 120.0 / hz) != floor((double)idx * 120.0 / hz);
}

/* The monitor's real vertical blank, from the display driver (gdi32's D3DKMT calls). Swap
 * calls alone don't say when the screen refreshes: with a frame queue they often return at
 * once, so timing taken from them drifts against the real refresh and motion jitters. */
typedef LONG (APIENTRY *PFN_KMT)(void *);
typedef struct { HDC hDc; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } KMT_OPENADAPTERFROMHDC;
typedef struct { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; } KMT_WAITFORVBLANK;
static PFN_KMT p_kmt_open, p_kmt_wait, p_kmt_close;
static KMT_WAITFORVBLANK g_vb;
static int g_vb_ok;
static HMONITOR g_vb_mon;

static void vblank_close(void) {
    if (g_vb_ok && p_kmt_close) { UINT a = g_vb.hAdapter; p_kmt_close(&a); }
    g_vb_ok = 0;
}
/* (re)open the adapter of the monitor the window is on; returns 1 if vblank waits work */
static int vblank_open(void) {
    HMONITOR mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY);
    if (g_vb_ok && mon == g_vb_mon) return 1;
    vblank_close();
    if (!p_kmt_open) {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) return 0;
        p_kmt_open = (PFN_KMT)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
        p_kmt_wait = (PFN_KMT)GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent");
        p_kmt_close = (PFN_KMT)GetProcAddress(gdi, "D3DKMTCloseAdapter");
        if (!p_kmt_open || !p_kmt_wait || !p_kmt_close) { p_kmt_open = NULL; return 0; }
    }
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return 0;
    HDC dc = CreateDCW(NULL, mi.szDevice, NULL, NULL);
    if (!dc) return 0;
    KMT_OPENADAPTERFROMHDC oa; memset(&oa, 0, sizeof oa); oa.hDc = dc;
    LONG st = p_kmt_open(&oa);
    DeleteDC(dc);
    if (st != 0) return 0;
    g_vb.hAdapter = oa.hAdapter; g_vb.hDevice = 0; g_vb.VidPnSourceId = oa.VidPnSourceId;
    g_vb_mon = mon; g_vb_ok = 1;
    return 1;
}

/* ---- refresh clock ----
 * The display driver signals each vblank, but inside a process that is also presenting with
 * GL the signal is unreliable: now and then it arrives a refresh late, and in fullscreen it
 * often fires twice. So a watcher thread (no GL) doesn't hand the signals on one by one; it
 * keeps a clock locked to them: the time of one refresh and the exact refresh period. An
 * early signal is the most trustworthy (wake-up delay only ever makes them later), so the
 * clock follows earlier signals fast and later ones slowly, and ignores ones far off. The
 * display thread then wakes on a precise timer just after each predicted refresh. */
static HANDLE g_vb_thread;
static SRWLOCK g_vb_lock = SRWLOCK_INIT;
static double g_vb_anchor, g_vb_period;     /* a refresh time and the period (ms); anchor 0 = not locked */
static long long g_vb_anchor_n;             /* that refresh's number */
static volatile LONG g_vb_run;

static void thread_for_timing(void);
static DWORD WINAPI vblank_thread(LPVOID arg) {
    (void)arg;
    thread_for_timing();
    double P0 = 1000.0 / plat_refresh_hz(), A = 0, P = P0;
    long long N = 0;
    int fails = 0, lost = 0;
    while (g_vb_run) {
        if (p_kmt_wait(&g_vb) != 0) {
            if (++fails > 10) break;
            Sleep(2); continue;
        }
        fails = 0;
        double e = plat_now_ms();
        if (A > 0) {
            double n = floor((e - A) / P + 0.5);
            double err = e - (A + n * P);
            if (n < 1) continue;                             /* a duplicate of the last one */
            if (fabs(err) > 1.0) {                           /* far off: late, or the clock is lost */
                if (++lost > 8) { N += (long long)n; A = e; P = P0; lost = 0; }
                else continue;
            } else {
                lost = 0;
                double gain = err < 0 ? 0.5 : 0.03;
                N += (long long)n;
                A += n * P + gain * err;
                P += gain * err / n * 0.2;
                if (P < P0 * 0.97 || P > P0 * 1.03) P = P0;
            }
        } else A = e;
        AcquireSRWLockExclusive(&g_vb_lock);
        g_vb_anchor = A; g_vb_anchor_n = N; g_vb_period = P;
        ReleaseSRWLockExclusive(&g_vb_lock);
    }
    AcquireSRWLockExclusive(&g_vb_lock);
    g_vb_anchor = 0;
    ReleaseSRWLockExclusive(&g_vb_lock);
    g_vb_run = 0;
    return 0;
}

static int vblank_clock(double *anchor, long long *anchor_n, double *period) {
    AcquireSRWLockShared(&g_vb_lock);
    *anchor = g_vb_anchor; *anchor_n = g_vb_anchor_n; *period = g_vb_period;
    ReleaseSRWLockShared(&g_vb_lock);
    return *anchor > 0;
}

static void vblank_thread_stop(void) {
    if (!g_vb_thread) return;
    g_vb_run = 0;
    WaitForSingleObject(g_vb_thread, 500);                  /* the wait returns at the next refresh */
    CloseHandle(g_vb_thread); g_vb_thread = NULL;
}
static int vblank_thread_start(void) {
    g_vb_anchor = 0;
    g_vb_run = 1;
    g_vb_thread = CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);
    if (!g_vb_thread) { g_vb_run = 0; return 0; }
    SetThreadPriority(g_vb_thread, THREAD_PRIORITY_TIME_CRITICAL);
    return 1;
}

/* Paced by the refresh clock: just after each refresh where a new picture is due, draw it
 * and present it; it appears at the next refresh and stays up until the next new one, so a
 * 240 Hz screen at 120 fps shows every picture for exactly two refreshes. */
static void display_loop_vblank(FILE *pace) {
    int pace_n = 0, waited = 0;
    long long last_idx = LLONG_MIN;
    DWORD mon_tick = GetTickCount();
    if (p_wglSwapIntervalEXT) p_wglSwapIntervalEXT(1);
    if (!vblank_thread_start()) return;
    while (g_dt_run) {
        if (IsIconic(g_wnd)) { Sleep(20); continue; }
        if (GetTickCount() - mon_tick > 1000) {             /* moved to another monitor? */
            mon_tick = GetTickCount();
            if (MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY) != g_vb_mon) {
                vblank_thread_stop();
                if (!vblank_open() || !vblank_thread_start()) break;    /* fall back to the swap-timed loop */
                last_idx = LLONG_MIN;
            }
        }
        if (!g_vb_run) break;                               /* the driver stopped answering */
        double A, P; long long AN;
        if (!vblank_clock(&A, &AN, &P)) {                   /* not locked yet */
            if (++waited > 200) break;
            Sleep(2); continue;
        }
        waited = 0;
        int k = dt_refreshes_per_picture(1000.0 / P);
        /* the next refresh that should get a new picture, numbered by the clock (N counts
         * refreshes since it locked, so both threads mean the same refresh by the same number) */
        /* The picture for refresh v is drawn ahead, from DT_AHEAD ms before v, and presented
         * just after v (it appears at v + P). Presenting a finished picture takes ~0.2 ms, so a
         * slow drawing (~2 ms normally, now and then 3+) or a driver hiccup never makes it
         * miss its refresh. */
        enum { DT_AHEAD_US = 3000 };
        const double ahead = DT_AHEAD_US / 1000.0;
        double now = plat_now_ms();
        long long idx = AN + (long long)ceil((now + 1.0 - A) / P);   /* at least 1 ms away */
        if (last_idx != LLONG_MIN && idx <= last_idx) idx = last_idx + 1;
        { double nominal = floor(1000.0 / P + 0.5); while (!dt_due(idx, nominal, k)) idx++; }
        double v = A + (double)(idx - AN) * P;
        if (v - now > 50) { Sleep(5); continue; }           /* clock jumped: re-read it */
        plat_sleep_until_ms(v - ahead);
        if (!g_dt_run) break;
        last_idx = idx;
        /* The game moment drawn is a fixed 3.25 ms before drawing starts, so every picture is
         * equally late (motion stays even) and a game frame may come up to ~4 ms late
         * without the picture having to wait for it. */
        double target = v - ahead - 3.25;
        double t_draw = plat_now_ms();
        int w = g_dt_render(target, g_dt_buf);
        if (w > 0 && w <= 544) {
            memcpy(g_frame, g_dt_buf, (size_t)w * 224 * 4);
            g_frame_w = w;
            glBindTexture(GL_TEXTURE_2D, t_game);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, 224, GL_BGRA_EXT, GL_UNSIGNED_BYTE, g_frame);
        }
        g_fps_frames++;
        /* present 1 ms after the refresh: clearly after it (so it can only be shown at the
         * next one, whatever small error the clock has), with ~3 ms to spare before that */
        plat_sleep_until_ms(v + 1.0);
        double t_up = plat_now_ms();
        draw_all();
        double t_sw = plat_now_ms();
        glFlush();
        if (pace) {
            extern volatile long hw_snap_clamps;
            fprintf(pace, "%.3f %ld %.3f %.3f %.3f %.3f %.3f %.4f\n", v, hw_snap_clamps, t_draw - v, t_up - v, t_sw - v,
                    plat_now_ms() - v, (double)k, P);
            if (++pace_n % 240 == 0) fflush(pace);
        }
    }
    vblank_thread_stop();
}

/* Opt the calling (timing-critical) thread out of power throttling, which on hybrid CPUs can
 * move it to a slow efficiency core and make it miss a refresh. Looked up at run time; on
 * Windows versions without it nothing changes. */
static void thread_for_timing(void) {
    typedef BOOL (WINAPI *SetInfo)(HANDLE, int, LPVOID, DWORD);
    SetInfo si = (SetInfo)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation");
    if (si) {
        struct { ULONG Version, ControlMask, StateMask; } pt = {1, 1 /* execution speed */, 0 /* not throttled */};
        si(GetCurrentThread(), 3 /* ThreadPowerThrottling */, &pt, sizeof pt);
    }
}

static DWORD WINAPI display_thread(LPVOID arg) {
    (void)arg;
    thread_for_timing();
    wglMakeCurrent(g_dc, g_gl);
    FILE *pace = getenv("POPN_PACE_LOG") ? fopen(getenv("POPN_PACE_LOG"), "w") : NULL;   /* diagnostics */
    if (!getenv("POPN_NO_VBLANK") && vblank_open()) display_loop_vblank(pace);
    if (p_wglSwapIntervalEXT) p_wglSwapIntervalEXT(1);
    double hz = plat_refresh_hz(), P = 1000.0 / hz, last = plat_now_ms();
    DWORD hz_tick = GetTickCount();
    int k = dt_refreshes_per_picture(hz), phase = 0;
    int pace_n = 0;
    while (g_dt_run) {
        if (IsIconic(g_wnd)) { Sleep(20); last = plat_now_ms(); continue; }
        if (GetTickCount() - hz_tick > 1000) { hz = plat_refresh_hz(); P = 1000.0 / hz; hz_tick = GetTickCount(); }
        double now = plat_now_ms(), target = last + P;      /* the refresh this picture will appear on */
        if (target < now + 0.5) target = now + P * 0.5;
        if (phase == 0) {                                   /* a new picture; otherwise show it again */
            k = dt_refreshes_per_picture(hz);
            int w = g_dt_render(target, g_dt_buf);
            if (w > 0 && w <= 544) {
                memcpy(g_frame, g_dt_buf, (size_t)w * 224 * 4);
                g_frame_w = w;
                glBindTexture(GL_TEXTURE_2D, t_game);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, 224, GL_BGRA_EXT, GL_UNSIGNED_BYTE, g_frame);
            }
            g_fps_frames++;
        }
        int was_new = phase == 0;
        phase = (phase + 1) % k;
        double t_draw = plat_now_ms();
        draw_all();
        glFinish();                                         /* returns at the refresh */
        double t2 = plat_now_ms(), t_fin = t2;
        if (t2 - last < P * 0.6) { plat_sleep_until_ms(last + P); t2 = plat_now_ms(); }   /* swap didn't wait */
        if (pace) {
            fprintf(pace, "%.3f %d %.3f %.3f %.3f %.3f\n", now, was_new, target, t_draw, t_fin, t2);
            if (++pace_n % 240 == 0) fflush(pace);
        }
        last = t2;
    }
    glFinish();
    wglMakeCurrent(NULL, NULL);
    vblank_close();
    if (pace) fclose(pace);
    return 0;
}

void plat_display_thread(int (*render)(double when_ms, uint8_t *bgrx)) {
    if (render && !g_dt && g_gl) {
        g_dt_render = render;
        wglMakeCurrent(NULL, NULL);
        g_dt_run = 1;
        g_dt = CreateThread(NULL, 0, display_thread, NULL, 0, NULL);
        if (g_dt) SetThreadPriority(g_dt, THREAD_PRIORITY_HIGHEST);
        else wglMakeCurrent(g_dc, g_gl);
    } else if (!render && g_dt) {
        g_dt_run = 0;
        WaitForSingleObject(g_dt, INFINITE);
        CloseHandle(g_dt); g_dt = NULL;
        wglMakeCurrent(g_dc, g_gl);
        g_swap = -1;                                        /* re-apply the swap interval */
    }
}

int plat_display_thread_running(void) { return g_dt != NULL; }

void plat_message(const char *title, const char *text) {
    wchar_t t[128], m[1024];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, t, 128);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, m, 1024);
    MessageBoxW(g_wnd, m, t, MB_OK | MB_ICONINFORMATION);
}

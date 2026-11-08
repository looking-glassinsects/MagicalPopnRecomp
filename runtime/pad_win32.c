/* Game controllers.
 *
 * - XInput (Xbox-style pads and everything that imitates one: most modern third-party pads,
 *   8BitDo in X mode, Steam Input, DS4Windows): all four slots.
 * - Every other game controller through Raw Input (HID joystick / gamepad / multi-axis), read
 *   from its own HID report descriptor, so any USB or Bluetooth controller gives buttons, axes
 *   and a hat. Known pads get their layout by vendor and product id; PlayStation 4/5 reports
 *   that Windows doesn't describe (Bluetooth full reports) are read directly, and Nintendo
 *   Switch Pro / NSO SNES controllers are woken up (USB handshake) and switched to their full
 *   report, which is read directly too.
 * - Each controller's layout can be changed in the menu (Controls > Set up controller) and is
 *   kept in settings.ini, per vendor:product (all XInput pads share one layout).
 *
 * Every controller is turned into the same "raw" state (buttons, 8 axes, a hat) and then into
 * VP_* buttons by its layout: a list of up to two inputs per VP_* button. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <winternl.h>        /* NTSTATUS, for the HID headers */
#include <hidusage.h>
#include <hidpi.h>
#include <hidsdi.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pad.h"
#include "platform.h"
#include "menu.h"

#ifndef RIDEV_DEVNOTIFY
#define RIDEV_DEVNOTIFY 0x00002000
#endif
#ifndef WM_INPUT_DEVICE_CHANGE
#define WM_INPUT_DEVICE_CHANGE 0x00FE
#endif
#ifndef GIDC_ARRIVAL
#define GIDC_ARRIVAL 1
#define GIDC_REMOVAL 2
#endif

enum { EK_NONE, EK_BTN, EK_AXIS, EK_HAT };
/* one input: button idx (0-based), axis idx with direction, or hat direction idx (0 up, 1 right,
 * 2 down, 3 left). `center`: only counts once the axis has been seen resting near the middle
 * (protects the default stick fallback from axes that rest at one end or don't exist) */
typedef struct { uint8_t kind, idx; int8_t dir; uint8_t center; } Elem;
#define NAX 8
typedef struct { uint64_t btn; float ax[NAX]; int hat; uint8_t centered; } Raw;   /* hat: -1, or 0..7 = up, clockwise */
typedef struct { Elem e[VP_COUNT][2]; } Layout;

enum { DK_XINPUT, DK_HID, DK_SONY, DK_NINTENDO };
typedef struct {
    int used, kind;
    HANDLE h;                           /* raw input device (HID kinds) */
    WORD vid, pid;
    PHIDP_PREPARSED_DATA ppd;
    HIDP_VALUE_CAPS *vc; int nvc;
    Raw raw; Layout lay;
    uint32_t prev;                      /* VP bits last time (to notice which controller is in use) */
    char key[24], name[96];
} Dev;

#define NXI 4                           /* g_dev[0..3]: XInput slots */
#define MAXDEV 20
static Dev g_dev[MAXDEV];
static char g_ini[MAX_PATH + 32];
static char g_last[96];
static DWORD g_start;
static DWORD g_xi_retry[NXI];
static double g_xi_polled = -1;

static const char *VPN[VP_COUNT] = {"b", "a", "y", "x", "l", "r", "select", "start",
                                    "up", "down", "left", "right", "menu", "fastforward"};

/* ---------------------------------------------------------------- layouts */
static Elem EB(int i) { Elem e = {EK_BTN, (uint8_t)i, 0, 0}; return e; }
static Elem EA(int i, int d, int center) { Elem e = {EK_AXIS, (uint8_t)i, (int8_t)d, (uint8_t)center}; return e; }
static Elem EH(int d) { Elem e = {EK_HAT, (uint8_t)d, 0, 0}; return e; }
static int same(Elem a, Elem b) { return a.kind && a.kind == b.kind && a.idx == b.idx && a.dir == b.dir; }

/* face buttons by position, shoulders, select/start, two menu buttons, fast-forward (-1 = none) */
static void set_buttons(Layout *L, int s, int e, int w, int n, int l, int r, int sel, int st, int m1, int m2, int ff) {
    int v[] = {s, e, w, n, l, r, sel, st}, vp[] = {VP_SOUTH, VP_EAST, VP_WEST, VP_NORTH, VP_L, VP_R, VP_SELECT, VP_START};
    for (int i = 0; i < 8; i++) if (v[i] >= 0) L->e[vp[i]][0] = EB(v[i]);
    if (m1 >= 0) L->e[VP_MENU][0] = EB(m1);
    if (m2 >= 0) L->e[VP_MENU][1] = EB(m2);
    if (ff >= 0) L->e[VP_FF][0] = EB(ff);
}

static void layout_default(Dev *d) {
    Layout *L = &d->lay;
    memset(L, 0, sizeof *L);
    /* d-pad: the hat, or the left stick (cheap pads send their d-pad as the X/Y axes) */
    L->e[VP_UP][0] = EH(0);    L->e[VP_UP][1] = EA(1, -1, 1);
    L->e[VP_RIGHT][0] = EH(1); L->e[VP_RIGHT][1] = EA(0, +1, 1);
    L->e[VP_DOWN][0] = EH(2);  L->e[VP_DOWN][1] = EA(1, +1, 1);
    L->e[VP_LEFT][0] = EH(3);  L->e[VP_LEFT][1] = EA(0, -1, 1);
    WORD v = d->vid, p = d->pid;
    if (d->kind == DK_XINPUT) {                                    /* A B X Y LB RB Back Start LS RS */
        set_buttons(L, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1);
        L->e[VP_FF][0] = EA(5, +1, 0);                              /* right trigger */
    } else if (v == 0x054C) {                                      /* PlayStation: Square Cross Circle Triangle ... */
        set_buttons(L, 1, 2, 0, 3, 4, 5, 8, 9, 12, 10, 7);
    } else if (v == 0x057E) {                                      /* Nintendo: B A Y X L R ZL ZR - + LS RS Home */
        set_buttons(L, 0, 1, 2, 3, 4, 5, 8, 9, 12, 10, 7);
    } else if (v == 0x2DC8) {                                      /* 8BitDo, D-input mode */
        set_buttons(L, 1, 0, 4, 3, 6, 7, 10, 11, 12, 13, 9);
    } else if ((v == 0x0079 && p == 0x0011) || (v == 0x0810 && p == 0xE501) || (v == 0x12BD && p == 0xD015)) {
        set_buttons(L, 2, 1, 3, 0, 4, 5, 8, 9, -1, -1, -1);         /* USB SNES-style pads: X A B Y L R . . Sel Start */
    } else if (v == 0x0583 && p == 0x2060) {                       /* iBuffalo SNES pad: A B X Y L R Sel Start */
        set_buttons(L, 1, 0, 3, 2, 4, 5, 6, 7, -1, -1, -1);
    } else {                                                       /* most PC pads (Logitech-style D-input) */
        set_buttons(L, 1, 2, 0, 3, 4, 5, 8, 9, 12, 10, 7);
    }
}

static int parse_elem(const char *s, Elem *e) {
    memset(e, 0, sizeof *e);
    if (!s[0]) return 1;                                           /* explicitly nothing */
    if (s[0] == 'b' && s[1] >= '0' && s[1] <= '9') { int i = atoi(s + 1); if (i < 1 || i > 64) return 0; *e = EB(i - 1); return 1; }
    if (s[0] == 'a' && s[1] >= '0' && s[1] <= '9') {
        int i = atoi(s + 1); const char *q = s + 1; while (*q >= '0' && *q <= '9') q++;
        if (i < 1 || i > NAX || (*q != '+' && *q != '-')) return 0;
        *e = EA(i - 1, *q == '+' ? 1 : -1, 0); return 1;
    }
    if (s[0] == 'h' && s[1]) {
        const char *dirs = "urdl", *f = strchr(dirs, s[1]);
        if (!f) return 0;
        *e = EH((int)(f - dirs)); return 1;
    }
    return 0;
}

static void elem_str(Elem e, char *out, int size) {
    switch (e.kind) {
    case EK_BTN: snprintf(out, size, "b%d", e.idx + 1); break;
    case EK_AXIS: snprintf(out, size, "a%d%c", e.idx + 1, e.dir > 0 ? '+' : '-'); break;
    case EK_HAT: snprintf(out, size, "h%c", "urdl"[e.idx & 3]); break;
    default: out[0] = 0;
    }
}

/* the default layout, then the custom one from settings.ini on top of it */
static void layout_load(Dev *d) {
    layout_default(d);
    char sec[48], v[32];
    snprintf(sec, sizeof sec, "pad %s", d->key);
    Elem custom[VP_COUNT]; int has[VP_COUNT] = {0};
    for (int vp = 0; vp < VP_COUNT; vp++) {
        GetPrivateProfileStringA(sec, VPN[vp], "?", v, sizeof v, g_ini);
        if (!strcmp(v, "?")) continue;
        if (parse_elem(v, &custom[vp])) has[vp] = 1;
    }
    for (int vp = 0; vp < VP_COUNT; vp++) if (has[vp]) d->lay.e[vp][0] = custom[vp];
    /* default extras that are now used for something else are dropped */
    for (int vp = 0; vp < VP_COUNT; vp++)
        for (int k = 0; k < 2; k++) {
            if (k == 0 && has[vp]) continue;
            for (int u = 0; u < VP_COUNT; u++)
                if (has[u] && u != vp && same(d->lay.e[vp][k], custom[u])) memset(&d->lay.e[vp][k], 0, sizeof(Elem));
        }
}

/* ---------------------------------------------------------------- state */
static int hat_has(int h, int d) {
    if (h < 0) return 0;
    switch (d) {
    case 0: return h == 7 || h == 0 || h == 1;
    case 1: return h >= 1 && h <= 3;
    case 2: return h >= 3 && h <= 5;
    default: return h >= 5 && h <= 7;
    }
}

static int dirs_to_hat(int u, int r, int d, int l) {
    if (u && d) u = d = 0;
    if (l && r) l = r = 0;
    if (u) return r ? 1 : l ? 7 : 0;
    if (d) return r ? 3 : l ? 5 : 4;
    if (r) return 2;
    if (l) return 6;
    return -1;
}

static int elem_on(const Raw *r, Elem e) {
    switch (e.kind) {
    case EK_BTN: return (int)((r->btn >> e.idx) & 1);
    case EK_AXIS:
        if (e.center && !((r->centered >> e.idx) & 1)) return 0;
        return e.dir > 0 ? r->ax[e.idx] > 0.5f : r->ax[e.idx] < -0.5f;
    case EK_HAT: return hat_has(r->hat, e.idx);
    }
    return 0;
}

static uint32_t dev_bits(Dev *d) {
    uint32_t m = 0;
    for (int vp = 0; vp < VP_COUNT; vp++)
        if (elem_on(&d->raw, d->lay.e[vp][0]) || elem_on(&d->raw, d->lay.e[vp][1])) m |= VPB(vp);
    /* Select + Start together opens the menu (pads without a spare button) */
    if ((m & VPB(VP_SELECT)) && (m & VPB(VP_START))) m = (m & ~(VPB(VP_SELECT) | VPB(VP_START))) | VPB(VP_MENU);
    return m;
}

static void note_axes(Raw *r) {
    for (int i = 0; i < NAX; i++) if (r->ax[i] > -0.25f && r->ax[i] < 0.25f) r->centered |= (uint8_t)(1 << i);
}

/* POPN_PAD_LOG=file: write what each controller is and sends (for diagnosing a controller) */
static FILE *g_log;
static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fflush(g_log);
}

static void announce(Dev *d) {
    logf_("connected: %s  kind %d  key %s  values %d\n", d->name, d->kind, d->key, d->nvc);
    if (GetTickCount() - g_start < 3000) return;                   /* already there at startup */
    char t[160]; snprintf(t, sizeof t, tr(T_PAD_CONNECTED), d->name);
    plat_toast(t);
}

/* ---------------------------------------------------------------- XInput */
static void xinput_poll(void) {
    double now_ms = plat_now_ms();
    if (now_ms - g_xi_polled < 1.0 && g_xi_polled >= 0) return;    /* several callers per frame */
    g_xi_polled = now_ms;
    DWORD now = GetTickCount();
    for (int s = 0; s < NXI; s++) {
        Dev *d = &g_dev[s];
        if (!d->used && (int)(now - g_xi_retry[s]) < 0) continue;  /* probing an empty slot is slow */
        XINPUT_STATE st;
        if (XInputGetState((DWORD)s, &st) != ERROR_SUCCESS) {
            if (d->used && !strcmp(g_last, d->name)) g_last[0] = 0;
            d->used = 0;
            g_xi_retry[s] = now + 2000 + (DWORD)s * 300;
            continue;
        }
        if (!d->used) {
            memset(d, 0, sizeof *d);
            d->used = 1; d->kind = DK_XINPUT;
            snprintf(d->key, sizeof d->key, "xinput");
            snprintf(d->name, sizeof d->name, "Xbox controller");
            layout_load(d);
            announce(d);
        }
        static const WORD XB[10] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK,
                                    XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB};
        WORD w = st.Gamepad.wButtons;
        Raw *r = &d->raw;
        r->btn = 0;
        for (int i = 0; i < 10; i++) if (w & XB[i]) r->btn |= 1ull << i;
        r->hat = dirs_to_hat(w & XINPUT_GAMEPAD_DPAD_UP, w & XINPUT_GAMEPAD_DPAD_RIGHT, w & XINPUT_GAMEPAD_DPAD_DOWN,
                             w & XINPUT_GAMEPAD_DPAD_LEFT);
        r->ax[0] = st.Gamepad.sThumbLX / 32767.0f;  r->ax[1] = -st.Gamepad.sThumbLY / 32767.0f;
        r->ax[2] = st.Gamepad.sThumbRX / 32767.0f;  r->ax[3] = -st.Gamepad.sThumbRY / 32767.0f;
        r->ax[4] = st.Gamepad.bLeftTrigger / 127.5f - 1; r->ax[5] = st.Gamepad.bRightTrigger / 127.5f - 1;
        r->centered = 0xFF;
    }
}

/* ---------------------------------------------------------------- HID */
static float byte_axis(BYTE b) { return b / 127.5f - 1.0f; }

/* PlayStation 4 / 5: every report kind, USB and Bluetooth */
static int sony_parse(Dev *d, const BYTE *r, ULONG n) {
    int ds5 = d->pid == 0x0CE6 || d->pid == 0x0DF2;
    const BYTE *p; int five;
    if (r[0] == 0x01 && ds5 && n >= 64) { p = r + 1; five = 1; }   /* DualSense USB */
    else if (r[0] == 0x01 && n >= 10) { p = r + 1; five = 0; }     /* DS4 USB / Bluetooth basic, DualSense BT basic */
    else if (r[0] == 0x11 && !ds5 && n >= 13) { p = r + 3; five = 0; }   /* DS4 Bluetooth full */
    else if (r[0] == 0x31 && ds5 && n >= 13) { p = r + 2; five = 1; }    /* DualSense Bluetooth full */
    else return 0;
    Raw *o = &d->raw;
    BYTE hatb = five ? p[7] : p[4], sh = five ? p[8] : p[5], ps = five ? p[9] : p[6];
    uint64_t b = 0;
    if (hatb & 0x10) b |= 1ull << 0;      /* square */
    if (hatb & 0x20) b |= 1ull << 1;      /* cross */
    if (hatb & 0x40) b |= 1ull << 2;      /* circle */
    if (hatb & 0x80) b |= 1ull << 3;      /* triangle */
    for (int i = 0; i < 8; i++) if (sh & (1 << i)) b |= 1ull << (4 + i);   /* L1 R1 L2 R2 share options L3 R3 */
    if (ps & 1) b |= 1ull << 12;
    if (ps & 2) b |= 1ull << 13;
    o->btn = b;
    o->hat = (hatb & 0x0F) <= 7 ? (hatb & 0x0F) : -1;
    o->ax[0] = byte_axis(p[0]); o->ax[1] = byte_axis(p[1]); o->ax[2] = byte_axis(p[2]); o->ax[5] = byte_axis(p[3]);
    o->ax[3] = byte_axis(five ? p[4] : p[7]); o->ax[4] = byte_axis(five ? p[5] : p[8]);
    return 1;
}

/* Nintendo full report (0x30): Switch Pro, NSO SNES / N64 controllers */
static void nintendo_parse(Dev *d, const BYTE *r) {
    BYTE rt = r[3], mid = r[4], lf = r[5];
    uint64_t b = 0;
    if (rt & 0x04) b |= 1ull << 0;        /* B */
    if (rt & 0x08) b |= 1ull << 1;        /* A */
    if (rt & 0x01) b |= 1ull << 2;        /* Y */
    if (rt & 0x02) b |= 1ull << 3;        /* X */
    if (lf & 0x40) b |= 1ull << 4;        /* L */
    if (rt & 0x40) b |= 1ull << 5;        /* R */
    if (lf & 0x80) b |= 1ull << 6;        /* ZL */
    if (rt & 0x80) b |= 1ull << 7;        /* ZR */
    if (mid & 0x01) b |= 1ull << 8;       /* - */
    if (mid & 0x02) b |= 1ull << 9;       /* + */
    if (mid & 0x08) b |= 1ull << 10;      /* left stick */
    if (mid & 0x04) b |= 1ull << 11;      /* right stick */
    if (mid & 0x10) b |= 1ull << 12;      /* home */
    if (mid & 0x20) b |= 1ull << 13;      /* capture */
    Raw *o = &d->raw;
    o->btn = b;
    o->hat = dirs_to_hat(lf & 0x02, lf & 0x04, lf & 0x01, lf & 0x08);
    int lx = r[6] | ((r[7] & 0x0F) << 8), ly = (r[7] >> 4) | (r[8] << 4);
    int rx = r[9] | ((r[10] & 0x0F) << 8), ry = (r[10] >> 4) | (r[11] << 4);
    float s = 1.0f / 1600.0f;
    o->ax[0] = (lx - 2048) * s; o->ax[1] = -(ly - 2048) * s;
    o->ax[3] = (rx - 2048) * s; o->ax[4] = -(ry - 2048) * s;
}

/* anything else, as its report descriptor says */
static void hid_parse(Dev *d, BYTE *r, ULONG n) {
    USAGE us[128]; ULONG nu = 128;
    if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, us, &nu, d->ppd, (PCHAR)r, n) == HIDP_STATUS_SUCCESS) {
        uint64_t b = 0;
        for (ULONG i = 0; i < nu; i++) if (us[i] >= 1 && us[i] <= 64) b |= 1ull << (us[i] - 1);
        d->raw.btn = b;
    }
    for (int i = 0; i < d->nvc; i++) {
        HIDP_VALUE_CAPS *vc = &d->vc[i];
        if (vc->UsagePage != HID_USAGE_PAGE_GENERIC) continue;
        USAGE u0 = vc->IsRange ? vc->Range.UsageMin : vc->NotRange.Usage, u1 = vc->IsRange ? vc->Range.UsageMax : u0;
        for (USAGE u = u0; u <= u1 && u >= u0; u++) {
            if (!((u >= 0x30 && u <= 0x37) || u == HID_USAGE_GENERIC_HATSWITCH)) continue;
            ULONG v;
            if (HidP_GetUsageValue(HidP_Input, vc->UsagePage, 0, u, &v, d->ppd, (PCHAR)r, n) != HIDP_STATUS_SUCCESS) continue;
            int bits = vc->BitSize;
            long long val = v, mn = vc->LogicalMin, mx = vc->LogicalMax;
            if (mn < 0 && bits > 0 && bits < 32 && (v & (1u << (bits - 1)))) val = (long long)v - (1ll << bits);
            if (mx <= mn) { mn = 0; mx = bits >= 32 || bits <= 0 ? 0xFFFFFFFFll : (1ll << bits) - 1; }   /* broken descriptors */
            if (u == HID_USAGE_GENERIC_HATSWITCH) {
                long long h = val - mn, range = mx - mn;
                d->raw.hat = range == 7 && h >= 0 && h <= 7 ? (int)h : range == 3 && h >= 0 && h <= 3 ? (int)h * 2 : -1;
                continue;
            }
            double f = (double)(val - mn) * 2.0 / (double)(mx - mn) - 1.0;
            d->raw.ax[u - 0x30] = (float)(f < -1 ? -1 : f > 1 ? 1 : f);
        }
    }
}

/* Switch Pro over USB says nothing until it gets a handshake; then both USB and Bluetooth are
 * asked for the full report (0x30) and player light 1. Runs on its own thread: writes to a
 * Bluetooth device can take a while. Errors (e.g. USB-only commands on Bluetooth) are ignored. */
static DWORD WINAPI nintendo_wake(LPVOID arg) {
    wchar_t *path = (wchar_t *)arg;
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    free(path);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD len = 64, w;
    PHIDP_PREPARSED_DATA ppd; HIDP_CAPS caps;
    if (HidD_GetPreparsedData(f, &ppd)) {
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS && caps.OutputReportByteLength >= 12 && caps.OutputReportByteLength <= 128)
            len = caps.OutputReportByteLength;
        HidD_FreePreparsedData(ppd);
    }
    BYTE b[128];
    static const BYTE usb[] = {0x02, 0x03, 0x02, 0x04};             /* handshake, 3 Mbit, handshake, no USB timeout */
    for (int i = 0; i < 4; i++) {
        memset(b, 0, sizeof b); b[0] = 0x80; b[1] = usb[i];
        WriteFile(f, b, len, &w, NULL);
        Sleep(40);
    }
    static const BYTE rumble_off[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    static const BYTE sub[][2] = {{0x03, 0x30}, {0x30, 0x01}};      /* full report mode; player 1 light */
    for (int i = 0; i < 2; i++) {
        memset(b, 0, sizeof b); b[0] = 0x01; b[1] = (BYTE)i;
        memcpy(b + 2, rumble_off, 8); b[10] = sub[i][0]; b[11] = sub[i][1];
        WriteFile(f, b, len, &w, NULL);
        Sleep(40);
    }
    CloseHandle(f);
    return 0;
}

static Dev *hid_find(HANDLE h) {
    for (int i = NXI; i < MAXDEV; i++) if (g_dev[i].used && g_dev[i].h == h) return &g_dev[i];
    return NULL;
}

static void hid_remove(HANDLE h) {
    Dev *d = hid_find(h);
    if (!d) return;
    if (!strcmp(g_last, d->name)) g_last[0] = 0;
    free(d->ppd); free(d->vc);
    memset(d, 0, sizeof *d);
}

static Dev *hid_add(HANDLE h) {
    Dev *d = hid_find(h);
    if (d) return d;
    RID_DEVICE_INFO info; memset(&info, 0, sizeof info); info.cbSize = sizeof info;
    UINT sz = sizeof info;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1 || info.dwType != RIM_TYPEHID) return NULL;
    USHORT page = info.hid.usUsagePage, usage = info.hid.usUsage;
    if (page != HID_USAGE_PAGE_GENERIC || (usage != 4 && usage != 5 && usage != 8)) return NULL;
    wchar_t path[512]; sz = 512;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, path, &sz) == (UINT)-1) return NULL;
    logf_("game controller device %04X:%04X usage %d: %ls\n", (unsigned)info.hid.dwVendorId,
          (unsigned)info.hid.dwProductId, usage, path);
    if (wcsstr(path, L"IG_") || wcsstr(path, L"ig_")) return NULL;   /* an XInput pad: read through XInput */
    for (int i = NXI; i < MAXDEV && !d; i++) if (!g_dev[i].used) d = &g_dev[i];
    if (!d) return NULL;
    UINT psz = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, NULL, &psz) != 0 || !psz) return NULL;
    PHIDP_PREPARSED_DATA ppd = (PHIDP_PREPARSED_DATA)malloc(psz);
    HIDP_CAPS caps;
    if (!ppd || GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, ppd, &psz) == (UINT)-1 ||
        HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS) { free(ppd); return NULL; }
    USHORT nv = caps.NumberInputValueCaps;
    HIDP_VALUE_CAPS *vc = nv ? (HIDP_VALUE_CAPS *)calloc(nv, sizeof *vc) : NULL;
    if (nv && (!vc || HidP_GetValueCaps(HidP_Input, vc, &nv, ppd) != HIDP_STATUS_SUCCESS)) nv = 0;

    memset(d, 0, sizeof *d);
    d->used = 1; d->h = h; d->ppd = ppd; d->vc = vc; d->nvc = nv;
    d->vid = (WORD)info.hid.dwVendorId; d->pid = (WORD)info.hid.dwProductId;
    d->raw.hat = -1;
    WORD v = d->vid, p = d->pid;
    if (v == 0x054C && (p == 0x05C4 || p == 0x09CC || p == 0x0BA0 || p == 0x0CE6 || p == 0x0DF2)) d->kind = DK_SONY;
    else if (v == 0x057E && (p == 0x2009 || p == 0x2017 || p == 0x2019)) d->kind = DK_NINTENDO;
    else d->kind = DK_HID;
    snprintf(d->key, sizeof d->key, "%04X-%04X", v, p);

    HANDLE f = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    wchar_t pn[128] = {0};
    if (f != INVALID_HANDLE_VALUE) {
        if (!HidD_GetProductString(f, pn, sizeof pn)) pn[0] = 0;
        CloseHandle(f);
    }
    while (pn[0] && pn[wcslen(pn) - 1] == L' ') pn[wcslen(pn) - 1] = 0;
    if (pn[0]) WideCharToMultiByte(CP_UTF8, 0, pn, -1, d->name, sizeof d->name, NULL, NULL);
    if (!d->name[0] || !strcmp(d->name, "Wireless Controller"))      /* what PlayStation pads call themselves */
        snprintf(d->name, sizeof d->name, "%s", v == 0x054C ? (p == 0x0CE6 || p == 0x0DF2 ? "DualSense" : "DualShock 4")
                 : v == 0x057E ? "Nintendo controller" : "Controller");
    layout_load(d);
    if (d->kind == DK_NINTENDO) {
        wchar_t *copy = _wcsdup(path);
        HANDLE t = copy ? CreateThread(NULL, 0, nintendo_wake, copy, 0, NULL) : NULL;
        if (t) CloseHandle(t); else free(copy);
    }
    announce(d);
    return d;
}

static void hid_input(HRAWINPUT ri_handle) {
    static BYTE buf[16384];
    UINT sz = 0;
    if (GetRawInputData(ri_handle, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)) != 0 || sz > sizeof buf || !sz) return;
    if (GetRawInputData(ri_handle, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    RAWINPUT *ri = (RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEHID) return;
    Dev *d = hid_add(ri->header.hDevice);
    if (!d) return;
    DWORD each = ri->data.hid.dwSizeHid, count = ri->data.hid.dwCount;
    BYTE *rep = ri->data.hid.bRawData;
    if (!each || (size_t)each * count + offsetof(RAWINPUT, data.hid.bRawData) > sz) return;
    for (DWORD i = 0; i < count; i++, rep += each) {
        if (d->kind == DK_SONY && sony_parse(d, rep, each)) continue;
        if (d->kind == DK_NINTENDO && rep[0] == 0x30 && each >= 12) { nintendo_parse(d, rep); continue; }
        hid_parse(d, rep, each);
    }
    note_axes(&d->raw);
}

/* ---------------------------------------------------------------- public */
void pad_init(void *hwnd, const char *ini_path) {
    snprintf(g_ini, sizeof g_ini, "%s", ini_path);
    g_start = GetTickCount();
    const char *lp = getenv("POPN_PAD_LOG");
    if (lp && lp[0]) g_log = fopen(lp, "w");
    RAWINPUTDEVICE rid[3];
    USHORT usages[3] = {4, 5, 8};                                  /* joystick, gamepad, multi-axis controller */
    for (int i = 0; i < 3; i++) {
        rid[i].usUsagePage = HID_USAGE_PAGE_GENERIC; rid[i].usUsage = usages[i];
        rid[i].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;        /* also while another window is active, like XInput */
        rid[i].hwndTarget = (HWND)hwnd;
    }
    RegisterRawInputDevices(rid, 3, sizeof rid[0]);
    /* controllers that are already connected */
    UINT n = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) == 0 && n) {
        RAWINPUTDEVICELIST *list = (RAWINPUTDEVICELIST *)calloc(n, sizeof *list);
        if (list) {
            UINT got = GetRawInputDeviceList(list, &n, sizeof *list);
            for (UINT i = 0; got != (UINT)-1 && i < got; i++) if (list[i].dwType == RIM_TYPEHID) hid_add(list[i].hDevice);
            free(list);
        }
    }
    xinput_poll();
}

void pad_message(unsigned msg, uintptr_t wp, intptr_t lp) {
    if (msg == WM_INPUT) hid_input((HRAWINPUT)lp);
    else if (msg == WM_INPUT_DEVICE_CHANGE) {
        if (wp == GIDC_ARRIVAL) {
            hid_add((HANDLE)lp);
            for (int s = 0; s < NXI; s++) g_xi_retry[s] = GetTickCount();   /* maybe an Xbox pad: look now */
            g_xi_polled = -1;
        } else if (wp == GIDC_REMOVAL) hid_remove((HANDLE)lp);
    }
}

uint32_t pad_state(void) {
    xinput_poll();
    uint32_t all = 0;
    for (int i = 0; i < MAXDEV; i++) {
        Dev *d = &g_dev[i];
        if (!d->used) continue;
        uint32_t v = dev_bits(d);
        if (v & ~d->prev) snprintf(g_last, sizeof g_last, "%s", d->name);
        if (g_log && v != d->prev)
            logf_("%s: buttons %llx hat %d axes %.2f %.2f %.2f %.2f %.2f %.2f -> %04x\n", d->name,
                  (unsigned long long)d->raw.btn, d->raw.hat, d->raw.ax[0], d->raw.ax[1], d->raw.ax[2], d->raw.ax[3],
                  d->raw.ax[4], d->raw.ax[5], (unsigned)v);
        d->prev = v;
        all |= v;
    }
    if (!g_last[0])                                                /* nothing pressed yet: the first one connected */
        for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used) { snprintf(g_last, sizeof g_last, "%s", g_dev[i].name); break; }
    return all;
}

const char *pad_last_name(void) {
    for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used) return g_last;
    return "";
}

/* ---------------------------------------------------------------- setup */
typedef struct { uint64_t b; uint8_t ap, an, h; } Act;
static Act act_of(const Raw *r) {
    Act a = {r->btn, 0, 0, 0};
    for (int i = 0; i < NAX; i++) {
        if (r->ax[i] > 0.5f) a.ap |= (uint8_t)(1 << i);
        if (r->ax[i] < -0.5f) a.an |= (uint8_t)(1 << i);
    }
    for (int d = 0; d < 4; d++) if (hat_has(r->hat, d)) a.h |= (uint8_t)(1 << d);
    return a;
}
static int g_setup, g_sdev = -1, g_nused;
static Act g_sprev[MAXDEV];
static Elem g_snew[VP_COUNT], g_sused[VP_COUNT];
static uint8_t g_sset[VP_COUNT];

int pad_setup_begin(void) {
    pad_state();
    int any = 0;
    for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used) { g_sprev[i] = act_of(&g_dev[i].raw); any = 1; }
    if (!any) return 0;
    g_setup = 1; g_sdev = -1; g_nused = 0;
    memset(g_sset, 0, sizeof g_sset);
    return 1;
}

int pad_setup_poll(int vp) {
    if (!g_setup) return 0;
    pad_state();
    int hit = 0;
    for (int i = 0; i < MAXDEV; i++) {
        Dev *d = &g_dev[i];
        if (!d->used) continue;
        Act a = act_of(&d->raw), p = g_sprev[i];
        g_sprev[i] = a;
        if (hit || (g_sdev >= 0 && g_sdev != i)) continue;
        Elem c[64 + 2 * NAX + 4]; int nc = 0;
        for (int k = 0; k < 4; k++) if ((a.h & ~p.h) & (1 << k)) c[nc++] = EH(k);
        for (int k = 0; k < 64; k++) if ((a.b & ~p.b) >> k & 1) c[nc++] = EB(k);
        for (int k = 0; k < NAX; k++) {
            if ((a.ap & ~p.ap) & (1 << k)) c[nc++] = EA(k, +1, 0);
            if ((a.an & ~p.an) & (1 << k)) c[nc++] = EA(k, -1, 0);
        }
        for (int k = 0; k < nc && !hit; k++) {
            int used = 0;
            for (int u = 0; u < g_nused; u++) if (same(g_sused[u], c[k])) used = 1;
            if (used) continue;
            g_sdev = i; g_snew[vp] = c[k]; g_sset[vp] = 1;
            if (g_nused < VP_COUNT) g_sused[g_nused++] = c[k];
            hit = 1;
        }
    }
    return hit;
}

const char *pad_setup_device(void) {
    return g_setup && g_sdev >= 0 && g_dev[g_sdev].used ? g_dev[g_sdev].name : "";
}

void pad_setup_end(int save) {
    if (save && g_setup && g_sdev >= 0 && g_dev[g_sdev].used) {
        Dev *d = &g_dev[g_sdev];
        char sec[48], v[16];
        snprintf(sec, sizeof sec, "pad %s", d->key);
        for (int vp = 0; vp < VP_COUNT; vp++) {
            if (!g_sset[vp]) continue;
            elem_str(g_snew[vp], v, sizeof v);
            WritePrivateProfileStringA(sec, VPN[vp], v, g_ini);
        }
        for (int i = 0; i < MAXDEV; i++)                           /* every connected pad of that kind */
            if (g_dev[i].used && !strcmp(g_dev[i].key, d->key)) layout_load(&g_dev[i]);
    }
    g_setup = 0; g_sdev = -1;
}

void pad_reset_layout(void) {
    static char names[8192];                                       /* "a\0b\0\0" */
    DWORD n = GetPrivateProfileSectionNamesA(names, sizeof names, g_ini);
    for (char *s = names; n && s < names + n && *s; s += strlen(s) + 1)
        if (!strncmp(s, "pad ", 4)) WritePrivateProfileStringA(s, NULL, NULL, g_ini);
    for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used) layout_load(&g_dev[i]);
}

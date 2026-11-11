/* Magical Pop'n (recompiled) entry point.
 *   MagicalPopn.exe                                   play
 *   MagicalPopn.exe --headless FRAMES [--shots DIR --every N] [--wav FILE] [--bot SEED] [--ws N]
 *                   [--state-test FRAME]              automated test runs
 * The ROM the build was made from is embedded as a resource (never shipped in the repo). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <time.h>
#include <math.h>
#include "rt.h"
#include "rt_state.h"
#include "hw.h"
#include "audio.h"
#include "platform.h"
#include "settings.h"
#include "menu.h"
#include "png_write.h"
#include "update.h"
#include "gen.h"

enum { EXIT_QUIT = 2, EXIT_RESET = 4, EXIT_LOAD = 5 };
#define FRAME_MS (1000.0 / 60.0988)

static Cpu g_cpu;
static int g_headless;
static uint32_t g_frames_run, g_max_frames, g_shot_every;
static const char *g_shot_dir;
static char g_dir[MAX_PATH], g_ini[MAX_PATH + 32];
static FILE *g_wav; static uint32_t g_wav_frames;
static int16_t g_native[4096 * 2];
static uint8_t g_wide[HW_WIDE_MAX_W * 224 * 4];
static uint8_t g_mid[HW_WIDE_MAX_W * 224 * 4];     /* motion smoothing: in-between frame */
static int g_ip_state = -1;                         /* recording in-between data (hw_interp_enable) */
static double g_ip_cost = 1.0;                      /* ms to draw one in-between frame (average) */
static int g_ws_override = -1;

/* ================================================================ save states */
#pragma pack(push, 1)
typedef struct StateHeader {
    char magic[8];                  /* "MPOPNST1" */
    char rom_sha1[40];
    uint32_t pc;
    uint16_t a, x, y, s, d;
    uint8_t db, p, e, pad;
    uint32_t hw_size;
    uint32_t frames;
} StateHeader;
#pragma pack(pop)

static int g_capture_slot;          /* 0 = auto (continue), 1..9 = slots, -1 = test file */
static int g_quit_after_capture, g_quit_wait;
static uint8_t *g_load_buf; static int g_load_slot;
static char g_test_state[MAX_PATH + 32];

static void slot_path(int slot, char *out, int size) {
    if (slot < 0) { snprintf(out, size, "%s", g_test_state); return; }
    char dir[MAX_PATH + 16]; snprintf(dir, sizeof dir, "%s\\states", g_dir); _mkdir(dir);
    if (slot == 0) snprintf(out, size, "%s\\continue.state", dir);
    else snprintf(out, size, "%s\\slot%d.state", dir, slot);
}

void state_slot_info(int slot, char *out, int size) {
    char path[MAX_PATH + 64]; slot_path(slot, path, sizeof path);
    WIN32_FILE_ATTRIBUTE_DATA fa; out[0] = 0;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return;
    FILETIME lt; SYSTEMTIME st;
    FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st);
    snprintf(out, size, "%04d-%02d-%02d  %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static void toastf(int id, int n) { char b[128]; snprintf(b, sizeof b, tr(id), n); if (!g_headless) plat_toast(b); }

/* reached a capture point (exact CPU state at pc) */
static void on_capture(const HwRegs *r, uint32_t pc) {
    int size = hw_state_size();
    uint8_t *buf = malloc(sizeof(StateHeader) + size);
    if (!buf) { if (g_capture_slot > 0) toastf(T_SAVE_FAILED, g_capture_slot); return; }
    StateHeader *h = (StateHeader *)buf;
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "MPOPNST1", 8); memcpy(h->rom_sha1, GEN_ROM_SHA1, 40);
    h->pc = pc; h->a = r->a; h->x = r->x; h->y = r->y; h->s = r->s; h->d = r->d;
    h->db = r->db; h->p = r->p; h->e = r->e; h->frames = hw_frame_count();
    h->hw_size = (uint32_t)hw_save_state(buf + sizeof *h);
    /* write a temporary file and swap it in, so a crash or a forced close mid-write never
     * leaves a damaged state (the autosave rewrites "continue" every 10 s) */
    char path[MAX_PATH + 64], tmp[MAX_PATH + 72]; slot_path(g_capture_slot, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    size_t n = sizeof *h + h->hw_size;
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(buf, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = 0;
    if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileA(tmp);
    free(buf);
    if (g_capture_slot > 0) toastf(ok ? T_SAVED : T_SAVE_FAILED, g_capture_slot);
    if (g_quit_after_capture) { snprintf(rt.halt_reason, sizeof rt.halt_reason, "quit"); longjmp(rt.exit_jmp, EXIT_QUIT); }
}

static void request_save(int slot) { g_capture_slot = slot; rt_capture_req = 1; }

static int read_state(int slot) {
    char path[MAX_PATH + 64]; slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = n > (long)sizeof(StateHeader) ? malloc((size_t)n) : NULL;
    int ok = b && fread(b, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    StateHeader *h = (StateHeader *)b;
    if (!ok || memcmp(h->magic, "MPOPNST1", 8) || memcmp(h->rom_sha1, GEN_ROM_SHA1, 40) ||
        sizeof(StateHeader) + h->hw_size != (size_t)n) { free(b); return 0; }
    free(g_load_buf); g_load_buf = b; g_load_slot = slot;
    return 1;
}

static void request_load(int slot) {
    if (!read_state(slot)) { toastf(T_NO_STATE, slot); return; }
    longjmp(rt.exit_jmp, EXIT_LOAD);
}

/* apply g_load_buf: hardware state + CPU registers; returns resume pc or 0 */
static uint32_t apply_loaded_state(void) {
    StateHeader *h = (StateHeader *)g_load_buf;
    if (!hw_load_state(g_load_buf + sizeof *h, (int)h->hw_size)) return 0;
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.a = h->a; g_cpu.x = h->x; g_cpu.y = h->y; g_cpu.s = h->s; g_cpu.d = h->d;
    g_cpu.db = h->db; g_cpu.p = h->p; g_cpu.e = h->e;
    return h->pc;
}

/* ================================================================ frames */
static int save_frame_png(const char *path) {
    static uint8_t rgb[HW_WIDE_MAX_W * 224 * 3];
    int w = hw_get_pixels_wide(g_wide);
    for (int i = 0; i < w * 224; i++) {
        rgb[i * 3] = g_wide[i * 4 + 2]; rgb[i * 3 + 1] = g_wide[i * 4 + 1]; rgb[i * 3 + 2] = g_wide[i * 4];
    }
    return png_write_rgb(path, rgb, w, 224);
}

static void screenshot(void) {
    char dir[MAX_PATH + 32], path[MAX_PATH + 96];
    snprintf(dir, sizeof dir, "%s\\screenshots", g_dir); _mkdir(dir);
    time_t t = time(NULL); struct tm tm; localtime_s(&tm, &t);
    snprintf(path, sizeof path, "%s\\MagicalPopn_%04d-%02d-%02d_%02d-%02d-%02d.png", dir,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    for (int i = 2; GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && i < 100; i++)   /* same second */
        snprintf(path, sizeof path, "%s\\MagicalPopn_%04d-%02d-%02d_%02d-%02d-%02d_%d.png", dir,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, i);
    if (save_frame_png(path) == 0) plat_toast(tr(T_TOAST_SCREENSHOT));
}

/* ---- test bot for --headless --bot SEED (same idea as the build-time profiler) ---- */
static int g_bot; static uint32_t g_rng = 1; static uint16_t g_hold; static int g_hold_left, g_stall;
static uint8_t g_prev_wram[0x20000];
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint16_t bot_input(uint32_t f) {
    if (f % 60 == 59) {
        const uint8_t *w = hw_wram(); int diff = 0;
        for (int i = 0; i < 0x20000; i++) diff += w[i] != g_prev_wram[i];
        memcpy(g_prev_wram, w, sizeof g_prev_wram);
        if (diff < 64) g_stall = 6;
    }
    { static int tour = -1; if (tour < 0) tour = getenv("POPN_TOUR") != NULL; if (tour) g_stall = 0; }
    if (g_stall > 0) { g_stall--; return 1 << BTN_START; }
    if (f % 45 < 4 && f < 1200) return 1 << BTN_A;
    if (g_hold_left-- <= 0) {
        uint32_t r = rnd() % 100;
        uint16_t m = r < 35 ? 1 << BTN_RIGHT : r < 55 ? 1 << BTN_LEFT : r < 65 ? 1 << BTN_UP : r < 75 ? 1 << BTN_DOWN : 0;
        static const int b[] = {BTN_B, BTN_Y, BTN_A, BTN_X};
        for (int i = 0; i < 4; i++) if (rnd() % 100 < 30) m |= (uint16_t)(1 << b[i]);
        g_hold = m; g_hold_left = 6 + (int)(rnd() % 40);
    }
    return g_hold;
}

/* ---- test: POPN_SWEEP (with POPN_TOUR) walks the player through the whole room, lane by lane
 * (up to the top, left to the edge, then right/left lanes going down), by setting her position
 * every frame, and measures the wide picture on the way. Prints one SWEEP line at the end. */
static struct {
    int on, phase, dir, px, py, stable, moved, inphase, desc, lanes, last_cx, last_cy, done, stage, area, hp, rescues, mode;
    int cxmin, cxmax, cymin, cymax, pbl, pbr, pcx, valid;
    int bk[3], ck[3]; unsigned flashes, cflashes, barjumps, first_flash, first_jump, first_cflash;
    double seam[2], ref[2]; unsigned seamn[2];
    unsigned long long tr_cmp, tr_bad, tr_ctrl; unsigned tr_frames, tr_first;
    LARGE_INTEGER t0; double tsum, tmax; unsigned tn, tslow;
} g_sw;
static uint8_t g_sw_px[HW_WIDE_MAX_W * 224 * 4];
static inline int sw_w16(const uint8_t *w, int a) { return w[a] | w[a + 1] << 8; }
static inline void sw_s16(uint8_t *w, int a, int v) { w[a] = (uint8_t)v; w[a + 1] = (uint8_t)(v >> 8); }
static inline int sw_dark(const uint8_t *p) { return p[0] < 8 && p[1] < 8 && p[2] < 8; }
static inline int sw_diff(const uint8_t *a, const uint8_t *b) { return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 48; }

static void sweep_frame(uint8_t *w, uint32_t f) {
    const int SPD = 3;
    if (!g_sw.on) {
        g_sw.on = 1; g_sw.px = sw_w16(w, 0x4C0); g_sw.py = sw_w16(w, 0x4C2); g_sw.phase = 0; g_sw.hp = w[0x51A];
        g_sw.stage = w[0x480]; g_sw.area = w[0x482]; g_sw.mode = w[0x484];
        g_sw.cxmin = g_sw.cymin = 1 << 30; g_sw.cxmax = g_sw.cymax = -(1 << 30);
        g_sw.last_cx = sw_w16(w, 0x62); g_sw.last_cy = sw_w16(w, 0x64);
        hw_ws_stats(); QueryPerformanceCounter(&g_sw.t0);
        return;
    }
    {   /* host time per emulated frame */
        LARGE_INTEGER t1, fq; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
        double ms = (double)(t1.QuadPart - g_sw.t0.QuadPart) * 1000.0 / (double)fq.QuadPart; g_sw.t0 = t1;
        g_sw.tsum += ms; g_sw.tn++; if (ms > g_sw.tmax) g_sw.tmax = ms; g_sw.tslow += ms > 8.0;
    }
    if (g_sw.done) return;
    if (w[0x480] != g_sw.stage || w[0x482] != g_sw.area) { g_sw.done = 2; return; }   /* left the room */
    int cx = sw_w16(w, 0x62), cy = sw_w16(w, 0x64);
    if (cx < g_sw.cxmin) g_sw.cxmin = cx; if (cx > g_sw.cxmax) g_sw.cxmax = cx;
    if (cy < g_sw.cymin) g_sw.cymin = cy; if (cy > g_sw.cymax) g_sw.cymax = cy;
    int horiz = g_sw.phase == 1 || g_sw.phase == 2 || g_sw.phase == 4;
    int camc = horiz ? cx != g_sw.last_cx : cy != g_sw.last_cy;
    g_sw.inphase++;
    if (camc) { g_sw.moved = 1; g_sw.stable = 0; } else g_sw.stable++;
    /* She walks the room with the game's own gravity and floors: first to its left end, then
     * all the way to its right end, jumping now and then. Only her X is set (so walls don't stop
     * her); a fall into a pit puts her back at the height she last stood at; health stays full.
     * A lane ends when the camera has stopped following her and she nears that screen edge. */
    int ob = sw_w16(w, 0x4C6), okob = ob >= 0x1000 && ob < 0x1F00;
    int oy = okob ? sw_w16(w, ob + 8) : g_sw.py;
    if (oy == g_sw.py) g_sw.desc++; else g_sw.desc = 0;             /* frames at the same height */
    static int ground = -1; if (g_sw.desc >= 3 || ground < 0) ground = oy;
    g_sw.py = oy;
    if (w[0x51A] < g_sw.hp) w[0x51A] = (uint8_t)g_sw.hp; else g_sw.hp = w[0x51A];
    int sx = g_sw.px - cx, sy = oy - cy, still = g_sw.stable >= 8, cap = g_sw.inphase > 6000, rescue = 0;
    if (sy > 200 && cy == g_sw.last_cy) { rescue = 1; g_sw.py = ground; g_sw.rescues++; }    /* in a pit */
    switch (g_sw.phase) {
    case 0: g_sw.phase = 1; break;
    case 1: g_sw.px -= SPD; if ((still && sx < 24) || cap) { g_sw.phase = 2; g_sw.dir = 1; } break;   /* to the left end */
    case 2:                                                                                      /* to the right end */
        g_sw.px += SPD;
        if ((still && sx > 232) || cap) { g_sw.lanes++; g_sw.done = 1; }
        break;
    }
    { static int dbg = -1; if (dbg < 0) dbg = getenv("POPN_SWEEP") && atoi(getenv("POPN_SWEEP")) == 2;
      if (dbg && f % 20 == 0) fprintf(stderr, "sw f %u ph %d px %d py %d cx %d cy %d sx %d sy %d st %d\n", f, g_sw.phase, g_sw.px, g_sw.py, cx, cy, sx, sy, g_sw.stable); }
    static int last_phase = -1;
    if (g_sw.phase != last_phase) { last_phase = g_sw.phase; g_sw.inphase = 0; g_sw.moved = 0; g_sw.stable = 0; }
    g_sw.last_cx = cx; g_sw.last_cy = cy;
    {   /* the camera leads in the direction she faces, which follows the d-pad */
        int d = g_sw.phase == 1 ? -1 : g_sw.phase == 2 ? 1 : 0;
        uint16_t b = (uint16_t)(d > 0 ? 1 << BTN_RIGHT : d < 0 ? 1 << BTN_LEFT : 0);
        if (g_sw.inphase % 64 >= 40 && g_sw.inphase % 64 < 56) b |= 1 << BTN_B;     /* jump */
        hw_set_buttons(b);
    }
    {   /* the player object (its address is at $04C6): X at +4 (+6 fraction), Y at +8 (+10 fraction, +12/+14 Y speed) */
        if (okob) {
            sw_s16(w, ob + 4, g_sw.px); sw_s16(w, ob + 6, 0);
            if (rescue) { sw_s16(w, ob + 8, g_sw.py); sw_s16(w, ob + 10, 0); sw_s16(w, ob + 12, 0); sw_s16(w, ob + 14, 0); }
        }
    }

    /* ---- measure the picture */
    int ww = hw_get_pixels_wide(g_sw_px);
    if (ww <= 256) { g_sw.valid = 0; return; }
    int es = (ww - 256) / 2;
    const uint8_t *P = g_sw_px;
    #define PX(x, y) (P + ((y) * ww + (x)) * 4)
    int bl = 0, br = 0;                         /* level-edge bars: columns black all the way down */
    for (; bl < es; bl++) { int all = 1; for (int y = 0; y < 224 && all; y += 3) all = sw_dark(PX(bl, y)); if (!all) break; }
    for (; br < es; br++) { int all = 1; for (int y = 0; y < 224 && all; y += 3) all = sw_dark(PX(ww - 1 - br, y)); if (!all) break; }
    int bk = 0, ck = 0;                         /* dark pixels in the side areas (outside bars) / in 4:3 */
    for (int y = 32; y < 224; y += 2) {
        for (int x = bl; x < es; x++) bk += sw_dark(PX(x, y));
        for (int x = es + 256; x < ww - br; x++) bk += sw_dark(PX(x, y));
        for (int x = es; x < es + 256; x += 2) ck += sw_dark(PX(x, y));
    }
    const char *info; int failed = hw_ws_failed(&info) != 0;
    if (g_sw.valid) {
        int dcx = cx - g_sw.pcx; if (dcx < 0) dcx = -dcx;
        if ((abs(bl - g_sw.pbl) > dcx + 8 || abs(br - g_sw.pbr) > dcx + 8) && !failed) { if (!g_sw.barjumps) g_sw.first_jump = f; g_sw.barjumps++; }
        int T = 400;
        if (g_sw.bk[1] > g_sw.bk[0] + T && g_sw.bk[1] > bk + T) { if (!g_sw.flashes) g_sw.first_flash = f - 1; g_sw.flashes++; }
        if (g_sw.ck[1] > g_sw.ck[0] + 1500 && g_sw.ck[1] > ck + 1500) { if (!g_sw.cflashes) g_sw.first_cflash = f - 1; g_sw.cflashes++; }
    }
    g_sw.bk[0] = g_sw.bk[1]; g_sw.bk[1] = bk; g_sw.ck[0] = g_sw.ck[1]; g_sw.ck[1] = ck;
    g_sw.pbl = bl; g_sw.pbr = br; g_sw.pcx = cx; g_sw.valid = 1;
    /* POPN_SWEEP_TRUTH=1 or 2 (run with only that BG layer shown, POPN_HIDE_BG): every side-area
     * row is kept with the scroll its line had; when the 4:3 picture later shows the same spot
     * (that line scrolled 71 px further, same vertical scroll) the two must agree. */
    {
        static int layer = -1; if (layer < 0) layer = getenv("POPN_SWEEP_TRUTH") ? atoi(getenv("POPN_SWEEP_TRUTH")) : 0;
        enum { TN = 384, E = 71 };
        typedef struct { int16_t h[224], v[224]; int used, f; uint8_t L[224][E][3], R[224][E][3], CL[224][E][3], CR[224][E][3]; } TrFrame;
        static TrFrame *tr; static int trn;
        if (layer >= 1 && layer <= 2 && es == E && !failed) {
            if (!tr) tr = (TrFrame *)calloc(TN, sizeof(TrFrame));
            const uint16_t *lh = hw_line_hscroll(layer - 1), *lv = hw_line_vscroll(layer - 1);
            unsigned cmp = 0, bad = 0, ctrl = 0;
            /* each match also compares a 71 px band of the 4:3 picture both frames show (control):
             * when that differs too (lightning, palette or tile animation), it isn't the side area */
            #define TRD(p, q) (abs((p)[2] - (q)[0]) + abs((p)[1] - (q)[1]) + abs((p)[0] - (q)[2]) > 30)
            for (int y = 32; y < 224; y++) {
                int h = lh[y + 1] & 0x3ff, v = lv[y + 1] & 0x3ff, doneR = 0, doneL = 0;
                for (int i = 0; i < TN && !(doneR && doneL); i++) {
                    const TrFrame *e = &tr[i];
                    if (!e->used || e->v[y] != v || (int)f - e->f > 200) continue;   /* (the scroll wraps every 1024 px) */
                    int eh = e->h[y];
                    if (!doneR && eh == ((h - E) & 0x3ff)) {
                        doneR = 1; unsigned b0 = bad, c0 = ctrl;
                        for (int x = 0; x < E; x++) {
                            cmp++; bad += TRD(PX(es + 256 - E + x, y), e->R[y][x]);
                            ctrl += TRD(PX(es + 256 - 2 * E + x, y), e->CR[y][x]);
                        }
                        static int dbg = -1; if (dbg < 0) dbg = getenv("POPN_TRUTH_DEBUG") ? atoi(getenv("POPN_TRUTH_DEBUG")) : 0;
                        if (dbg && (int)f == dbg && y % 16 == 0)
                            fprintf(stderr, "truth R f%u y%d h%d v%d matched f%d bad %u ctrl %u\n", f, y, h, v, e->f, bad - b0, ctrl - c0);
                    }
                    if (!doneL && eh == ((h + E) & 0x3ff)) {
                        doneL = 1; unsigned b0 = bad, c0 = ctrl;
                        for (int x = 0; x < E; x++) {
                            cmp++; bad += TRD(PX(es + x, y), e->L[y][x]);
                            ctrl += TRD(PX(es + E + x, y), e->CL[y][x]);
                        }
                        static int dbg = -1; if (dbg < 0) dbg = getenv("POPN_TRUTH_DEBUG") ? atoi(getenv("POPN_TRUTH_DEBUG")) : 0;
                        if (dbg && (int)f == dbg && y % 16 == 0)
                            fprintf(stderr, "truth L f%u y%d h%d v%d matched f%d bad %u ctrl %u\n", f, y, h, v, e->f, bad - b0, ctrl - c0);
                    }
                }
            }
            #undef TRD
            g_sw.tr_cmp += cmp; g_sw.tr_bad += bad; g_sw.tr_ctrl += ctrl;
            { static int dbg = -1; if (dbg < 0) dbg = getenv("POPN_TRUTH_DEBUG") ? atoi(getenv("POPN_TRUTH_DEBUG")) : 0;
              if (dbg && (int)f >= dbg && (int)f < dbg + 40) fprintf(stderr, "truth frame %u cmp %u bad %u ctrl %u\n", f, cmp, bad, ctrl); }
            if (cmp >= 400 && bad * 5 > cmp && bad > 3 * ctrl + cmp / 20) {
                if (!g_sw.tr_frames) g_sw.tr_first = f;
                g_sw.tr_frames++;
                if (getenv("POPN_TRUTH_LOG")) fprintf(stderr, "truth bad frame %u cmp %u bad %u ctrl %u\n", f, cmp, bad, ctrl);
            }
            TrFrame *e = &tr[trn]; trn = (trn + 1) % TN;
            e->used = 1; e->f = (int)f;
            for (int y = 0; y < 224; y++) {
                e->h[y] = (int16_t)(lh[y + 1] & 0x3ff); e->v[y] = (int16_t)(lv[y + 1] & 0x3ff);
                for (int x = 0; x < E; x++) {
                    const uint8_t *p = PX(x, y), *q = PX(es + 256 + x, y), *c = PX(es + x, y), *d = PX(es + 256 - E + x, y);
                    e->L[y][x][0] = p[2]; e->L[y][x][1] = p[1]; e->L[y][x][2] = p[0];
                    e->R[y][x][0] = q[2]; e->R[y][x][1] = q[1]; e->R[y][x][2] = q[0];
                    e->CL[y][x][0] = c[2]; e->CL[y][x][1] = c[1]; e->CL[y][x][2] = c[0];
                    e->CR[y][x][0] = d[2]; e->CR[y][x][1] = d[1]; e->CR[y][x][2] = d[0];
                }
            }
        }
    }
    /* seams: how often a pixel differs from its neighbour across each 4:3 edge, against the same
     * measure 16/32/48 px inside the 4:3 picture (same tile phase) */
    if (!failed) for (int side = 0; side < 2; side++) {
        if (side == 0 ? bl >= es - 1 : br >= es - 1) continue;
        int xs = side == 0 ? es : es + 256, s = 0, r = 0;
        for (int y = 40; y < 224; y++) {
            s += sw_diff(PX(xs - 1, y), PX(xs, y));
            for (int k = 1; k <= 3; k++) { int xr = side == 0 ? xs + 16 * k : xs - 16 * k; r += sw_diff(PX(xr - 1, y), PX(xr, y)); }
        }
        g_sw.seam[side] += s; g_sw.ref[side] += r / 3.0; g_sw.seamn[side]++;
    }
    #undef PX
}

static void sweep_print(void) {
    const uint32_t *o = hw_ws_stats();
    printf("SWEEP %d %d done %d lanes %d rescue %d camx %d-%d camy %d-%d flash %u@%u cflash %u@%u barjump %u@%u "
           "seamL %.2f seamR %.2f n %u %u obj %u %u %u %u ms %.2f max %.1f slow %u truth %llu %llu %u@%u ctrl %llu mode %d\n",
           g_sw.stage, g_sw.area, g_sw.done, g_sw.lanes, g_sw.rescues, g_sw.cxmin, g_sw.cxmax, g_sw.cymin, g_sw.cymax,
           g_sw.flashes, g_sw.first_flash, g_sw.cflashes, g_sw.first_cflash, g_sw.barjumps, g_sw.first_jump,
           g_sw.seamn[0] ? (g_sw.seam[0] + 1) / (g_sw.ref[0] + 1) : 0.0, g_sw.seamn[1] ? (g_sw.seam[1] + 1) / (g_sw.ref[1] + 1) : 0.0,
           g_sw.seamn[0], g_sw.seamn[1], o[0], o[1], o[2], o[3],
           g_sw.tn ? g_sw.tsum / g_sw.tn : 0.0, g_sw.tmax, g_sw.tslow, g_sw.tr_cmp, g_sw.tr_bad, g_sw.tr_frames, g_sw.tr_first, g_sw.tr_ctrl, g_sw.mode);
}

static uint32_t g_state_test = 0;
static const char *g_load_state_arg;   /* --load-state FILE: start from this state */   /* --state-test FRAME: save at FRAME, load at FRAME+180 */

static void on_vblank_headless(void) {
    g_frames_run++;
    {   /* debug: POPN_DUMP_WRAM=file -> per frame: BG1 hscroll (u16) + WRAM $0000-$1FFF */
        static FILE *df; static int init;
        if (!init) { const char *p = getenv("POPN_DUMP_WRAM"); if (p) df = fopen(p, "wb"); init = 1; }
        static int size = -1; if (size < 0) size = getenv("POPN_DUMP_SIZE") ? (int)strtol(getenv("POPN_DUMP_SIZE"), NULL, 0) : 0x2000;
        if (df) { uint16_t h = hw_bg_hscroll(0); fwrite(&h, 2, 1, df); fwrite(hw_wram(), 1, (size_t)size, df); }
    }
    int n = hw_take_samples(g_native, 4096);
    if (g_wav) { fwrite(g_native, 4, n, g_wav); g_wav_frames += n; }
    if (g_bot) hw_set_buttons(bot_input(g_frames_run));
    {   /* POPN_TOUR="stage,area,frame": jump there at that frame (the game's own debug warp) and,
         * from then on, stay invincible and log per-frame health of that area to stdout at the end */
        static int init, stage, area, chapter, at = -1; static uint32_t pc_hist_pc[32], pc_hist_n[32]; static int pc_n;
        static unsigned frames, fail, blacksides; static uint32_t lag_pc;
        static uint8_t px[HW_WIDE_MAX_W * 224 * 4];
        static unsigned rooms[16][16];
        if (!init) { init = 1; const char *e = getenv("POPN_TOUR"); if (e && sscanf(e, "%d,%d,%d,%d", &stage, &area, &at, &chapter) != 4) at = -1; }
        if (at >= 0) {
            uint8_t *w = (uint8_t *)hw_wram();
            if ((int)g_frames_run == at) { w[0x480] = (uint8_t)chapter; w[0x481] = 0; w[0x482] = 1; w[0x483] = 0; w[0x486] = 1; w[0x487] = 0; }
            {   /* POPN_TOUR_STEPS=1: go through the stage's first area first (it loads the stage's graphics) */
                static int steps = -1; if (steps < 0) steps = getenv("POPN_TOUR_STEPS") != NULL;
                int t1 = at + 260, t2 = steps ? at + 520 : at + 260;
                if (steps && (int)g_frames_run == t1) { w[0x480] = (uint8_t)stage; w[0x481] = 0; w[0x482] = 1; w[0x483] = 0; w[0x486] = 1; w[0x487] = 0; }
                if ((int)g_frames_run == t2 && !(steps && area == 1)) { w[0x480] = (uint8_t)stage; w[0x481] = 0; w[0x482] = (uint8_t)area; w[0x483] = 0; w[0x486] = 1; w[0x487] = 0; }
            }
            if ((int)g_frames_run > at) {
                if (getenv("POPN_TOUR_SLOW")) w[0x5C] = 1;     /* debug slow motion ($60 extra vblanks per frame) */
                static int settle = -1; if (settle < 0) settle = getenv("POPN_TOUR_STEPS") ? 720 : 460;
                if ((int)g_frames_run > at + settle) {
                    static int sweep = -1, ending;
                    if (sweep < 0) sweep = getenv("POPN_SWEEP") != NULL;
                    if (sweep) {
                        sweep_frame(w, g_frames_run);
                        if (g_sw.done && !ending) { ending = 1; if (g_max_frames > g_frames_run + 3) g_max_frames = g_frames_run + 3; }
                    }
                    const char *info;
                    frames++;
                    fail += hw_ws_failed(&info) != 0;
                    { int s = w[0x480], a = w[0x482] & 15; if (s == stage && a < 16) rooms[1][a]++; else rooms[0][0]++; }
                    int found = 0;
                    for (int i = 0; i < pc_n; i++) if (pc_hist_pc[i] == rt_loop_pc) { pc_hist_n[i]++; found = 1; break; }
                    if (!found && pc_n < 32) { pc_hist_pc[pc_n] = rt_loop_pc; pc_hist_n[pc_n++] = 1; }
                    int ww = hw_get_pixels_wide(px);
                    if (ww > 256) {
                        int es = (ww - 256) / 2, black = 0, cb = 0;
                        for (int y = 32; y < 224; y += 2) {
                            for (int x = 0; x < es; x += 2) { const uint8_t *p = px + (y * ww + x) * 4; black += !p[0] && !p[1] && !p[2]; }
                            for (int x = es; x < es + 256; x += 2) { const uint8_t *p = px + (y * ww + x) * 4; cb += !p[0] && !p[1] && !p[2]; }
                        }
                        blacksides += black > 150 && cb < 2500;
                    }
                }
            }
            if (g_frames_run + 1 >= g_max_frames) {
                uint32_t top = 0; unsigned tn = 0, all = 0;
                for (int i = 0; i < pc_n; i++) { all += pc_hist_n[i]; if (pc_hist_n[i] > tn) { tn = pc_hist_n[i]; top = pc_hist_pc[i]; } }
                (void)lag_pc;
                printf("TOUR %d %d frames %u ws_fail %u black_sides %u spin_pc %06X lag %u rooms", stage, area, frames, fail, blacksides, top, all - tn);
                for (int s = 0; s < 16; s++) for (int a = 0; a < 16; a++) if (rooms[s][a]) printf(" %d-%d:%u", s, a, rooms[s][a]);
                printf("\n");
                if (getenv("POPN_SWEEP")) sweep_print();
            }
        }
    }
    {   /* debug: POPN_INPUT="FROM-TO:MASK,..." holds buttons (BTN_* bit mask, hex) on those frames */
        static int init, ni; static unsigned from[64], to[64], mask[64];
        if (!init) {
            init = 1;
            const char *p = getenv("POPN_INPUT");
            while (p && *p && ni < 64 && sscanf(p, "%u-%u:%x", &from[ni], &to[ni], &mask[ni]) == 3) {
                ni++; p = strchr(p, ','); if (p) p++;
            }
        }
        if (ni) {
            unsigned m = 0;
            for (int i = 0; i < ni; i++) if (g_frames_run >= from[i] && g_frames_run <= to[i]) m |= mask[i];
            hw_set_buttons((uint16_t)m);
        }
    }
    static int shot_from = -1; if (shot_from < 0) { const char *e = getenv("POPN_SHOT_FROM"); shot_from = e ? atoi(e) : 0; }
    if (g_shot_dir && g_shot_every && g_frames_run % g_shot_every == 0 && (int)g_frames_run >= shot_from) {
        char path[1024]; snprintf(path, sizeof path, "%s/f%05u.png", g_shot_dir, g_frames_run);
        save_frame_png(path);
        /* POPN_INTERP_TEST=1: also the in-between frame (t = 0.5) before this one */
        if (getenv("POPN_INTERP_TEST") && hw_interp_ready()) {
            static uint8_t rgb[HW_WIDE_MAX_W * 224 * 3];
            if (getenv("POPN_IP_DEBUG")) fprintf(stderr, "== frame %u\n", g_frames_run);
            int w = hw_render_interp(0.5f, g_mid);
            for (int i = 0; i < w * 224; i++) { rgb[i * 3] = g_mid[i * 4 + 2]; rgb[i * 3 + 1] = g_mid[i * 4 + 1]; rgb[i * 3 + 2] = g_mid[i * 4]; }
            snprintf(path, sizeof path, "%s/f%05u_mid.png", g_shot_dir, g_frames_run);
            png_write_rgb(path, rgb, w, 224);
            /* POPN_SNAP_TEST=1: the pictures the display thread would show at t = .25/.5/.75, drawn
             * through the same snapshot path (hw_snapshot_publish / hw_snapshot_render) */
            if (getenv("POPN_SNAP_TEST")) {
                const double F = 1000.0 / 60.0988, t0 = g_frames_run * F;
                hw_snapshot_publish(t0);
                for (int k = 1; k <= 3; k++) {
                    int ww = hw_snapshot_render(t0 + k * F / 4, g_mid);
                    if (!ww) continue;
                    for (int i = 0; i < ww * 224; i++) { rgb[i * 3] = g_mid[i * 4 + 2]; rgb[i * 3 + 1] = g_mid[i * 4 + 1]; rgb[i * 3 + 2] = g_mid[i * 4]; }
                    snprintf(path, sizeof path, "%s/f%05u_q%d.png", g_shot_dir, g_frames_run, k);
                    png_write_rgb(path, rgb, ww, 224);
                }
            }
        }
    }
    if (g_shot_dir && g_shot_every && getenv("POPN_INTERP_TEST") && (g_frames_run + 1) % g_shot_every == 0) {
        char path[1024]; snprintf(path, sizeof path, "%s/f%05u_prev.png", g_shot_dir, g_frames_run + 1);
        save_frame_png(path);                     /* the frame before the next shot */
    }
    {   /* POPN_WS_REPORT=dir: log gameplay frames left in 4:3 and capture a state there */
        static const char *rep; static int init; static uint32_t last = 0;
        if (!init) { rep = getenv("POPN_WS_REPORT"); init = 1; }
        const char *info;
        if (rep && hw_ws_failed(&info) && (last == 0 || g_frames_run - last > 900)) {
            last = g_frames_run;
            char pth[1024]; snprintf(pth, sizeof pth, "%s/report.txt", rep);
            FILE *f = fopen(pth, "a"); if (f) { fprintf(f, "frame %u %s\n", g_frames_run, info); fclose(f); }
            snprintf(g_test_state, sizeof g_test_state, "%s/fail_%06u.state", rep, g_frames_run);
            request_save(-1);
        }
    }
    { static int ipi; if (!ipi) { ipi = 1; if (getenv("POPN_INTERP_TEST") || getenv("POPN_IP_SELFTEST")) hw_interp_enable(1); } }
    {   /* POPN_IP_SELFTEST=dir: check motion smoothing on every frame. The in-between picture at
         * t = 1 must equal the real frame exactly; t = 0 is compared with the previous real frame
         * (differences there are pose changes, new graphics, or bugs) and the worst are saved. */
        static const char *dir; static int init; static uint8_t prevf[HW_WIDE_MAX_W * 224 * 4], curf[HW_WIDE_MAX_W * 224 * 4];
        static int prevw; static unsigned checked, bad1, worst_n; static FILE *rep;
        if (!init) { init = 1; dir = getenv("POPN_IP_SELFTEST"); if (dir) { char p[1024]; snprintf(p, sizeof p, "%s/selftest.txt", dir); rep = fopen(p, "w"); } }
        if (dir && rep) {
            int w = hw_get_pixels_wide(curf);
            if (hw_interp_ready() && prevw == w) {
                static uint8_t t1f[HW_WIDE_MAX_W * 224 * 4];
                int w1 = hw_render_interp(1.0f, t1f);
                unsigned d1 = 0;
                for (int i = 0; i < w * 224; i++) d1 += memcmp(&t1f[i * 4], &curf[i * 4], 3) != 0;
                int w0 = hw_render_interp(0.0f, g_mid);
                unsigned d0 = 0;
                for (int i = 0; i < w * 224; i++) d0 += memcmp(&g_mid[i * 4], &prevf[i * 4], 3) != 0;
                checked++;
                if (w1 != w || d1) {
                    bad1++;
                    extern void hw_interp_dump(int a, int b);
                    fprintf(stderr, "== t1 mismatch frame %u (%u px)\n", g_frames_run, d1);
                    hw_interp_dump(0, 60);
                }
                fprintf(rep, "%u %u %u\n", g_frames_run, d1, d0);
                static unsigned t1_n;
                static int keep = -1; if (keep < 0) { const char *e = getenv("POPN_IP_KEEP"); keep = e ? atoi(e) : 400; }   /* smallest t=0 difference worth keeping */
                if ((d1 && t1_n < 40) || (d0 > (unsigned)keep && d0 < (unsigned)(w * 224) / 2 && worst_n < 20)) {   /* keep evidence */
                    if (d1) t1_n++; else worst_n++;
                    static uint8_t rgb[HW_WIDE_MAX_W * 224 * 3];
                    char p[1024];
                    const uint8_t *imgs[4] = {prevf, g_mid, curf, t1f}; const char *nm[4] = {"a_prev", "b_t0", "c_cur", "d_t1"};
                    for (int k = 0; k < (d1 ? 4 : 3); k++) {
                        for (int i = 0; i < w * 224; i++) { rgb[i * 3] = imgs[k][i * 4 + 2]; rgb[i * 3 + 1] = imgs[k][i * 4 + 1]; rgb[i * 3 + 2] = imgs[k][i * 4]; }
                        snprintf(p, sizeof p, "%s/f%06u_%s.png", dir, g_frames_run, nm[k]); png_write_rgb(p, rgb, w, 224);
                    }
                    (void)w0;
                }
                if (g_frames_run % 600 == 0) fflush(rep);
            }
            memcpy(prevf, curf, (size_t)w * 224 * 4); prevw = w;
            if (g_frames_run + 1 >= g_max_frames) { fprintf(rep, "# checked %u, t=1 mismatches %u\n", checked, bad1); fflush(rep); }
        }
    }
    {   /* debug: POPN_SAVE_AT="FRAME,FILE" saves a state to FILE at that frame (works with --load-state) */
        static int at = -2; static char file[MAX_PATH];
        if (at == -2) { const char *e = getenv("POPN_SAVE_AT"); at = -1; if (e && sscanf(e, "%d,%259[^\n]", &at, file) != 2) at = -1; }
        if (at >= 0 && (int)g_frames_run == at) { snprintf(g_test_state, sizeof g_test_state, "%s", file); request_save(-1); }
    }
    if (g_state_test && g_frames_run == g_state_test) request_save(-1);
    if (g_state_test && g_frames_run == g_state_test + 180) {
        printf("state-test: loading state saved at frame %u\n", g_state_test);
        request_load(-1);
    }
    if (g_frames_run >= g_max_frames) {
        snprintf(rt.halt_reason, sizeof rt.halt_reason, "reached frame limit");
        longjmp(rt.exit_jmp, EXIT_QUIT);
    }
}

/* ---- pacing ---- */
static double g_emu_hist[32]; static int g_emu_i;
static double g_exit_t, g_next_t;
static int g_lock_k; static double g_hz = 60.0;
static int g_ff_count;

static double emu_estimate(void) {
    double m = 0; for (int i = 0; i < 32; i++) if (g_emu_hist[i] > m) m = g_emu_hist[i];
    return m;
}

static int smoothing_active(void) {
    plat_display_rate(g_set.motion_smooth);
    return g_set.motion_smooth > 60 && g_hz > 75.0;
}

static void choose_pacing(void) {
    g_hz = plat_refresh_hz();
    g_lock_k = 0;
    if (smoothing_active()) return;     /* the display thread presents (vsync); the game runs by timer */
    if (g_set.vsync) {
        int k = (int)(g_hz / 60.0988 + 0.5);
        if (k >= 1 && fabs(g_hz / k - 60.0) < 1.0) g_lock_k = k;      /* 60/120/240 Hz: lock to refresh */
        plat_set_swap_interval(1);      /* tear-free; the 60 Hz cadence comes from our own timing */
    } else plat_set_swap_interval(0);
}

/* start/stop the display thread when the setting or the refresh rate changes; returns 1 if on */
static int smoothing_update(void) {
    int want = smoothing_active();
    if (want != g_ip_state) {
        hw_interp_enable(want);
        plat_display_thread(want ? hw_snapshot_render : NULL);
        g_ip_state = want;
        choose_pacing();
    }
    return want;
}

static void quit_game(void) {
    settings_save(g_ini);
    if (g_set.resume && !g_quit_after_capture) {         /* remember where we are, then quit */
        g_quit_after_capture = 1; g_quit_wait = 0;
        request_save(0);
        return;
    }
    snprintf(rt.halt_reason, sizeof rt.halt_reason, "quit");
    longjmp(rt.exit_jmp, EXIT_QUIT);
}

static void handle_action(int a) {
    switch (a) {
    case MA_QUIT: quit_game(); break;
    case MA_RESET: settings_save(g_ini); longjmp(rt.exit_jmp, EXIT_RESET);
    case MA_SCREENSHOT: screenshot(); break;
    case MA_SAVE: request_save(g_set.slot); break;
    case MA_LOAD: request_load(g_set.slot); break;
    }
}

static void run_menu(void) {
    audio_clear();
    menu_open();
    for (;;) {
        if (!plat_pump()) { quit_game(); menu_close(); break; }
        int f = plat_take_flags();
        if (f & PF_MENU) menu_close();
        if (f & PF_SCREENSHOT) { menu_close(); screenshot(); }
        int vk;
        while (menu_is_open() && plat_pop_key(&vk)) handle_action(menu_handle_key(vk));
        if (!menu_is_open() || g_quit_after_capture) { menu_close(); break; }
        menu_tick();
        plat_redraw();
        if (!g_set.vsync || plat_display_thread_running()) Sleep(5);   /* nothing else waits here */
    }
    settings_save(g_ini);
    plat_clear_keys();
    hw_set_widescreen(settings_ws_extra(), 1); hw_set_sprite_limit(g_set.sprite_limit);
    choose_pacing();
    g_next_t = plat_now_ms(); g_exit_t = 0;
}

static void run_pause(void) {
    audio_clear();
    plat_toast(tr(T_PAUSED));
    for (;;) {
        if (!plat_pump()) { quit_game(); return; }
        int f = plat_take_flags();
        if (f & PF_PAUSE) break;
        if (f & PF_MENU) { run_menu(); break; }
        plat_redraw();
        Sleep(16);
    }
    g_next_t = plat_now_ms(); g_exit_t = 0;
}

/* A newer release was found: ask, then download the new builder (progress in the corner) and
 * hand over to it. It rebuilds the game from the embedded ROM into this folder and restarts it. */
static void offer_update(const char *version) {
    char text[1024];
    audio_clear();
    snprintf(text, sizeof text, tr(T_UPDATE_ASK), version);
    if (getenv("POPN_UPDATE_AUTO") /* test: no question */ || plat_ask(tr(T_ERROR_TITLE), text)) {
        update_begin();
        int p;
        for (int tick = 0; (p = update_progress()) >= 0 && p < 100; tick++) {
            if (!plat_pump()) { quit_game(); return; }
            (void)plat_take_flags();
            if (tick % 10 == 0) { char b[128]; snprintf(b, sizeof b, tr(T_UPDATE_PROGRESS), p); plat_toast(b); }
            plat_redraw();
            Sleep(30);
        }
        if (p == 100 && update_launch()) { quit_game(); return; }
        plat_message(tr(T_ERROR_TITLE), tr(T_UPDATE_FAILED));
    }
    plat_clear_keys();
    g_next_t = plat_now_ms(); g_exit_t = 0;
}

/* Runs at the start of every vblank: the frame is complete and the game's controller read
 * comes right after this returns. Present first, wait as late as possible, then read input. */
static void on_vblank_window(void) {
    double t_in = plat_now_ms();
    if (g_exit_t > 0) { g_emu_hist[g_emu_i++ & 31] = t_in - g_exit_t; }
    g_frames_run++;
    plat_note_emulated_frame();
    if (g_quit_after_capture && ++g_quit_wait > 30) {     /* couldn't capture in time: just quit */
        snprintf(rt.halt_reason, sizeof rt.halt_reason, "quit"); longjmp(rt.exit_jmp, EXIT_QUIT);
    }
    if (g_frames_run % 300 == 1) choose_pacing();
    /* autosave your place every 10 s, so "continue" survives crashes and forced closes too */
    if (g_set.resume && g_frames_run % 600 == 0 && !rt_capture_req && !g_quit_after_capture) request_save(0);
    if (g_frames_run % 600 == 0 && getenv("POPN_AUDIO_LOG")) {     /* diagnostics */
        static FILE *lf; if (!lf) { char pth[MAX_PATH + 32]; snprintf(pth, sizeof pth, "%s\\audio_log.txt", g_dir); lf = fopen(pth, "w"); }
        long u, mn, mx, cb; audio_stats(&u, &mn, &mx, &cb);
        if (lf) { fprintf(lf, "frame %u underruns %ld fill %ld..%ld callbacks %ld lock %d hz %.2f emu %.2fms ip %d %.2fms shown %.1f fps\n", g_frames_run, u, mn, mx, cb, g_lock_k, g_hz, emu_estimate(), g_ip_state, g_ip_cost, plat_shown_fps()); fflush(lf); }
    }

    int ff = plat_fast_forward();
    int focused = plat_focused();
    int n = hw_take_samples(g_native, 4096);
    int vol = (g_set.mute || (g_set.mute_unfocused && !focused)) ? 0 : g_set.volume;
    if (!ff) audio_push(g_native, n, HW_AUDIO_RATE, vol);

    if (ff) {
        int skip = g_set.ff_speed == FF_2X ? 2 : g_set.ff_speed == FF_4X ? 4 : 16;
        plat_set_swap_interval(0);
        if (++g_ff_count % skip == 0) {
            if (g_ip_state > 0) hw_snapshot_publish(plat_now_ms() - 2 * FRAME_MS);   /* shown as is */
            int w = hw_get_pixels_wide(g_wide); plat_present(g_wide, w);
            if (g_set.ff_speed != FF_MAX) { g_next_t += FRAME_MS; plat_sleep_until_ms(g_next_t); }
        }
        if (plat_now_ms() > g_next_t + 100) g_next_t = plat_now_ms();
    } else {
        if (g_ff_count) { g_ff_count = 0; choose_pacing(); g_next_t = plat_now_ms(); }
        int w = hw_get_pixels_wide(g_wide);
        /* Motion smoothing (refresh rates above 75 Hz): a display thread shows a new picture on
         * every refresh, drawn by the PPU at that refresh's own moment between the two newest
         * frames (see hw_snapshot_render). Here the game just runs at 60 fps by timer and hands
         * over each finished frame. */
        if (smoothing_update()) {
            double now = plat_now_ms();
            if (now > g_next_t + 3 * FRAME_MS || now < g_next_t - 3 * FRAME_MS) g_next_t = now;
            hw_snapshot_publish(g_next_t);                   /* this frame's moment */
            g_next_t += FRAME_MS;
            plat_sleep_until_ms(g_next_t - emu_estimate() - 1.0);
            goto presented;
        }
        plat_present(g_wide, w);                         /* blocks until vblank when vsync-locked */
        double now = plat_now_ms();
        if (g_lock_k && !plat_minimized()) {
            /* vsync-locked: the swap normally blocks until the refresh. If the driver returned
             * early anyway (ignored interval, compositor quirk), hold the 60 Hz schedule here. */
            /* The swap just returned at a refresh. The next frame must be on screen exactly one
             * game frame (= k refreshes) later: wake up, read input and emulate just before that
             * refresh; the swap then waits at most until it. */
            double period = g_lock_k * 1000.0 / g_hz;
            if (now - g_next_t > 2 * period || g_next_t - now > 2 * period) g_next_t = now;
            double base = now > g_next_t - 1.0 ? now : g_next_t;      /* swap returned early: keep schedule */
            double margin = g_set.low_latency ? 2.5 : 6.0;
            plat_sleep_until_ms(base + period - emu_estimate() - margin);
            g_next_t = base + period;
        } else {                                         /* timer pacing (vsync off, 144 Hz, minimized) */
            g_next_t += FRAME_MS;
            if (now > g_next_t + 3 * FRAME_MS || now < g_next_t - 3 * FRAME_MS) g_next_t = now + FRAME_MS;
            plat_sleep_until_ms(g_next_t - emu_estimate() - 1.0);
        }
    }
presented:

    if (!plat_pump()) quit_game();
    int flags = plat_take_flags();
    if (flags & PF_SCREENSHOT) screenshot();
    if (flags & PF_SAVE) request_save(g_set.slot);
    if (flags & PF_LOAD) request_load(g_set.slot);
    if (flags & (PF_SLOT_PREV | PF_SLOT_NEXT)) {
        g_set.slot = (g_set.slot - 1 + ((flags & PF_SLOT_NEXT) ? 1 : 8)) % 9 + 1;
        char info[64], b[160]; state_slot_info(g_set.slot, info, sizeof info);
        snprintf(b, sizeof b, tr(T_SLOT), g_set.slot);
        if (info[0]) { strncat(b, "    ", sizeof b - strlen(b) - 1); strncat(b, info, sizeof b - strlen(b) - 1); }
        plat_toast(b);
    }
    if (flags & PF_MENU) run_menu();
    if (flags & PF_PAUSE) run_pause();
    { char ver[32]; if (!g_quit_after_capture && update_available(ver, sizeof ver)) offer_update(ver); }
    static int bg_save_frame = -1;
    if (g_set.pause_unfocused && !plat_focused() && g_set.resume && bg_save_frame < 0 && !g_quit_after_capture) {
        request_save(0);                 /* save your place first; pause from the next frame on */
        bg_save_frame = (int)g_frames_run;
    } else if (g_set.pause_unfocused && !plat_focused() && !g_quit_after_capture) {
        audio_clear();
        while (!plat_focused()) {
            if (!plat_pump()) { quit_game(); break; }
            if (plat_take_flags() & PF_MENU) run_menu();
            Sleep(30);
        }
        g_next_t = plat_now_ms();
        bg_save_frame = -1;
    }
    if (plat_focused()) bg_save_frame = -1;
    hw_set_buttons(plat_buttons());                     /* latched by the game right after this */
    {   /* debug: POPN_LIVE_INPUT="FROM-TO:MASK,..." scripted buttons in the window too, and
         * POPN_LIVE_FRAMES=N quits after N frames */
        static int init, ni, quit_at; static unsigned from[64], to[64], mask[64];
        if (!init) {
            init = 1;
            const char *p = getenv("POPN_LIVE_INPUT");
            while (p && *p && ni < 64 && sscanf(p, "%u-%u:%x", &from[ni], &to[ni], &mask[ni]) == 3) { ni++; p = strchr(p, ','); if (p) p++; }
            p = getenv("POPN_LIVE_FRAMES"); quit_at = p ? atoi(p) : 0;
        }
        if (ni) {
            unsigned m = 0;
            for (int i = 0; i < ni; i++) if (g_frames_run >= from[i] && g_frames_run <= to[i]) m |= mask[i];
            hw_set_buttons((uint16_t)m);
        }
        if (quit_at && (int)g_frames_run >= quit_at) { snprintf(rt.halt_reason, sizeof rt.halt_reason, "quit"); longjmp(rt.exit_jmp, EXIT_QUIT); }
    }
    g_exit_t = plat_now_ms();
}

/* ================================================================ startup */
static const uint8_t *embedded_rom(size_t *size) {
    HRSRC r = FindResourceA(NULL, "ROMDATA", MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    if (!r) return NULL;
    HGLOBAL g = LoadResource(NULL, r);
    *size = SizeofResource(NULL, r);
    return g ? (const uint8_t *)LockResource(g) : NULL;
}

static uint8_t *load_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*size);
    if (d && fread(d, 1, *size, f) != *size) { free(d); d = NULL; }
    fclose(f); return d;
}

static void boot_cpu(void) {
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.e = 1; g_cpu.s = 0x01FF; g_cpu.p = P_M | P_X | P_I;
    { extern Cpu *rt_dbg_cpu; rt_dbg_cpu = &g_cpu; }
}

int main(int argc, char **argv) {
    const char *rom_override = NULL, *wav_path = NULL;
    { extern Cpu *rt_dbg_cpu; rt_dbg_cpu = &g_cpu; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless") && i + 1 < argc) { g_headless = 1; g_max_frames = (uint32_t)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) g_shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) g_shot_every = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_override = argv[++i];
        else if (!strcmp(argv[i], "--ws") && i + 1 < argc) g_ws_override = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--state-test") && i + 1 < argc) g_state_test = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) g_load_state_arg = argv[++i];
        else if (!strcmp(argv[i], "--bot") && i + 1 < argc) { g_bot = 1; g_rng = (uint32_t)atoi(argv[++i]) * 2654435761u + 99; }
    }
    plat_exe_dir(g_dir, sizeof g_dir);
    snprintf(g_ini, sizeof g_ini, "%s\\settings.ini", g_dir);
    snprintf(rt.missing_path, sizeof rt.missing_path, "%s\\discovered.txt", g_dir);
    snprintf(g_test_state, sizeof g_test_state, "%s\\state_test.state", g_dir);
    settings_load(g_ini);

    size_t rom_size = 0;
    const uint8_t *rom = rom_override ? load_file(rom_override, &rom_size) : embedded_rom(&rom_size);
    if (!rom || rom_size != GEN_ROM_SIZE) {
        if (!g_headless) MessageBoxA(NULL, "The game data is missing or damaged. Please rebuild with the Magical Pop'n Builder.",
                                     "Magical Pop'n", MB_OK | MB_ICONERROR);
        fprintf(stderr, "ROM missing or wrong size\n");
        return 1;
    }
    if (!hw_init(rom, rom_size, rt_frame_hook)) { fprintf(stderr, "hardware init failed\n"); return 1; }
    rt_init_hooks();
    rt.on_capture = on_capture;
    rt.nmi_vec = canon24(rom[0x7FEA] | (rom[0x7FEB] << 8));
    rt.irq_vec = canon24(rom[0x7FEE] | (rom[0x7FEF] << 8));

    if (g_headless) {
        rt.on_frame = on_vblank_headless;
        hw_set_widescreen(g_ws_override >= 0 ? g_ws_override : 0, 1);
        hw_set_sprite_limit(getenv("POPN_SPRITE_LIMIT") ? atoi(getenv("POPN_SPRITE_LIMIT")) : g_set.sprite_limit);
        if (wav_path && (g_wav = fopen(wav_path, "wb")) != NULL) {
            static const uint8_t hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                0x28,0x7D,0,0, 0xA0,0xF4,0x01,0, 4,0,16,0,'d','a','t','a',0,0,0,0};   /* 32040 Hz */
            fwrite(hdr, 1, 44, g_wav);
        }
    } else {
        if (!plat_init("Magical Pop'n")) {
            MessageBoxA(NULL, "Could not open the game window (OpenGL is required).", "Magical Pop'n", MB_OK | MB_ICONERROR);
            return 1;
        }
        audio_init();
        rt.on_frame = on_vblank_window;
        hw_set_widescreen(settings_ws_extra(), 1); hw_set_sprite_limit(g_set.sprite_limit);
        choose_pacing();
        g_next_t = plat_now_ms();
        if (g_set.resume && read_state(0)) plat_toast(tr(T_RESUMED));
        else plat_toast(tr(T_TOAST_WELCOME));
        update_init(GEN_VERSION, g_dir, rom, rom_size);
        if (g_set.check_updates) update_check();
    }

    if (g_load_state_arg) {
        snprintf(g_test_state, sizeof g_test_state, "%s", g_load_state_arg);
        if (!read_state(-1)) { fprintf(stderr, "could not load state %s\n", g_load_state_arg); return 1; }
        g_state_test = 0;
    }
    int rc = g_load_buf ? EXIT_LOAD : 0, started = 0;
    for (;;) {
        rt.cpu = &g_cpu; rt.in_irq = 0; rt.interp_depth = 0;
        rt_capture_req = 0; g_quit_after_capture = 0;
        uint32_t pc = 0;
        if (rc == EXIT_LOAD) {
            pc = apply_loaded_state();
            audio_clear();
            if (pc && g_load_slot > 0 && started) toastf(T_LOADED, g_load_slot);
            if (!pc) { hw_reset(); boot_cpu(); }
        } else boot_cpu();
        rc = setjmp(rt.exit_jmp);
        if (rc == 0) {
            started = 1;
            if (pc) rt_resume(&g_cpu, pc);          /* never returns */
            rt_run(&g_cpu, GEN_RESET);
        }
        if (rc == EXIT_RESET) { hw_reset(); audio_clear(); if (!g_headless) plat_toast(tr(T_RESET_DONE)); rc = 0; continue; }
        if (rc == EXIT_LOAD) continue;
        break;
    }

    if (g_headless) {
        printf("stopped: %s\n", rt.halt_reason);
        printf("frames %u functions %d dispatches %llu unwinds %llu interp_calls %llu interp_steps %llu\n",
               g_frames_run, GEN_FUNC_COUNT, rt.stats.dispatches, rt.stats.unwinds, rt.stats.interp_calls, rt.stats.interp_steps);
        { int ms, mt; unsigned ov = (unsigned)hw_sprite_overflow_lines(&ms, &mt);
          printf("sprite_overflow_lines %u busiest_line sprites %d slivers %d\n", ov, ms, mt); }
        if (g_wav) {
            uint32_t bytes = g_wav_frames * 4, riff = 36 + bytes;
            fseek(g_wav, 4, SEEK_SET); fwrite(&riff, 4, 1, g_wav);
            fseek(g_wav, 40, SEEK_SET); fwrite(&bytes, 4, 1, g_wav);
            fclose(g_wav);
        }
        if (g_state_test) DeleteFileA(g_test_state);
    } else {
        settings_save(g_ini);
        if (rc == 1) plat_message(tr(T_ERROR_TITLE), rt.halt_reason);
        audio_shutdown();
        plat_shutdown();
    }
    hw_free();
    return rc == 1 ? 3 : 0;
}

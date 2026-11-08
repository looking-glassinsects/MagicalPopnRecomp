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
static uint8_t *g_load_buf; static int g_load_size, g_load_slot;
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
    uint8_t *b = malloc(n > 0 ? n : 1);
    int ok = n > (long)sizeof(StateHeader) && fread(b, 1, n, f) == (size_t)n;
    fclose(f);
    StateHeader *h = (StateHeader *)b;
    if (!ok || memcmp(h->magic, "MPOPNST1", 8) || memcmp(h->rom_sha1, GEN_ROM_SHA1, 40) ||
        sizeof(StateHeader) + h->hw_size != (size_t)n) { free(b); return 0; }
    free(g_load_buf); g_load_buf = b; g_load_size = (int)n; g_load_slot = slot;
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

static uint32_t g_state_test = 0;
static const char *g_load_state_arg;   /* --load-state FILE: start from this state */   /* --state-test FRAME: save at FRAME, load at FRAME+180 */

static void on_vblank_headless(void) {
    g_frames_run++;
    {   /* debug: POPN_DUMP_WRAM=file -> per frame: BG1 hscroll (u16) + WRAM $0000-$1FFF */
        static FILE *df; static int init;
        if (!init) { const char *p = getenv("POPN_DUMP_WRAM"); if (p) df = fopen(p, "wb"); init = 1; }
        if (df) { uint16_t h = hw_bg_hscroll(0); fwrite(&h, 2, 1, df); fwrite(hw_wram(), 1, 0x2000, df); }
    }
    int n = hw_take_samples(g_native, 4096);
    if (g_wav) { fwrite(g_native, 4, n, g_wav); g_wav_frames += n; }
    if (g_bot) hw_set_buttons(bot_input(g_frames_run));
    {   /* debug: POPN_INPUT="FROM-TO:MASK,..." holds buttons (BTN_* bit mask, hex) on those frames */
        static int init, n; static unsigned from[64], to[64], mask[64];
        if (!init) {
            init = 1;
            const char *p = getenv("POPN_INPUT");
            while (p && *p && n < 64 && sscanf(p, "%u-%u:%x", &from[n], &to[n], &mask[n]) == 3) {
                n++; p = strchr(p, ','); if (p) p++;
            }
        }
        if (n) {
            unsigned m = 0;
            for (int i = 0; i < n; i++) if (g_frames_run >= from[i] && g_frames_run <= to[i]) m |= mask[i];
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
                if ((d1 && t1_n < 40) || (d0 > 400 && d0 < (unsigned)(w * 224) / 2 && worst_n < 20)) {   /* keep evidence */
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
}

int main(int argc, char **argv) {
    const char *rom_override = NULL, *wav_path = NULL;
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

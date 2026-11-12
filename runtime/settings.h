/* User settings, persisted to settings.ini next to the executable. */
#pragma once

enum { BTN_B, BTN_Y, BTN_SELECT, BTN_START, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_X, BTN_L, BTN_R, BTN_COUNT };
enum { ASPECT_PIXEL, ASPECT_TV, ASPECT_STRETCH, ASPECT_COUNT };
enum { LANG_EN, LANG_JA, LANG_COUNT };
enum { FF_2X, FF_4X, FF_MAX, FF_COUNT };
enum { WS_OFF, WS_16_9, WS_16_10, WS_21_9, WS_COUNT };

typedef struct Settings {
    int scale;            /* window size multiplier, 1..6 */
    int fullscreen;
    int aspect;           /* ASPECT_* (pixel shape) */
    int widescreen;       /* WS_*: draw extra level columns instead of stretching */
    int integer_scale;
    int smooth;
    int show_fps;
    int volume;           /* 0..100 */
    int mute;
    int language;         /* LANG_* */
    int pause_unfocused;
    int ff_speed;         /* FF_* */
    int keys[BTN_COUNT];  /* Windows virtual-key codes */
    int vsync;            /* lock to the display (60/120/240 Hz: every 1st/2nd/4th refresh) */
    int low_latency;      /* read input as late as possible before each frame */
    int motion_smooth;    /* pictures per second: 60 (off) or 120 (in-between frames) */
    int scanlines;
    int sprite_limit;     /* 1 = hardware limit of 32 sprites / 34 tiles per line (drops sprites) */
    int mute_unfocused;
    int resume;           /* continue where you left off when starting the game */
    int slot;             /* save state slot 1..9 */
    int check_updates;    /* look for a new release when the game starts */
    int no_slowdown;      /* give the game extra CPU time for frames it can't finish (removes the original's slowdown) */
    int win_x, win_y;     /* last window position (-32000 = let Windows choose) */
} Settings;

extern Settings g_set;

void settings_defaults(Settings *s);
void settings_reset_keys(Settings *s);
void settings_load(const char *path);
void settings_save(const char *path);
/* extra columns rendered on each side for the current widescreen + pixel-aspect settings */
int settings_ws_extra(void);
/* displayed pixel aspect (1.0 square, 8/7 TV) */
double settings_par(void);

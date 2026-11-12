#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "settings.h"

Settings g_set;

static const char *KEY_NAMES[BTN_COUNT] = {"B", "Y", "Select", "Start", "Up", "Down", "Left", "Right", "A", "X", "L", "R"};

void settings_reset_keys(Settings *s) {
    s->keys[BTN_B] = 'Z';      s->keys[BTN_A] = 'X';
    s->keys[BTN_Y] = 'A';      s->keys[BTN_X] = 'S';
    s->keys[BTN_L] = 'Q';      s->keys[BTN_R] = 'W';
    s->keys[BTN_START] = VK_RETURN;  s->keys[BTN_SELECT] = VK_RSHIFT;
    s->keys[BTN_UP] = VK_UP;   s->keys[BTN_DOWN] = VK_DOWN;
    s->keys[BTN_LEFT] = VK_LEFT; s->keys[BTN_RIGHT] = VK_RIGHT;
}

void settings_defaults(Settings *s) {
    s->scale = 3; s->fullscreen = 0; s->aspect = ASPECT_PIXEL; s->widescreen = WS_OFF; s->integer_scale = 0; s->smooth = 0;
    s->show_fps = 0; s->volume = 100; s->mute = 0; s->pause_unfocused = 1; s->ff_speed = FF_4X;
    s->language = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE) ? LANG_JA : LANG_EN;
    s->vsync = 1; s->low_latency = 0; s->motion_smooth = 60; s->scanlines = 0; s->sprite_limit = 0; s->mute_unfocused = 0; s->resume = 0; s->slot = 1; s->check_updates = 1; s->no_slowdown = 1;
    s->win_x = s->win_y = -32000;
    settings_reset_keys(s);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void settings_load(const char *path) {
    Settings *s = &g_set;
    settings_defaults(s);
#define GETI(sec, name, field) s->field = GetPrivateProfileIntA(sec, name, s->field, path)
    GETI("display", "scale", scale);       GETI("display", "fullscreen", fullscreen);
    GETI("display", "aspect", aspect);     GETI("display", "integer_scale", integer_scale);
    GETI("display", "widescreen", widescreen);
    GETI("display", "smooth", smooth);     GETI("display", "show_fps", show_fps);
    GETI("sound", "volume", volume);       GETI("sound", "mute", mute);
    GETI("general", "language", language); GETI("general", "pause_unfocused", pause_unfocused);
    GETI("general", "fast_forward", ff_speed);
    GETI("display", "vsync", vsync);       GETI("display", "low_latency", low_latency);
    /* older versions saved motion_smoothing = 0/1 (off / every refresh) */
    s->motion_smooth = GetPrivateProfileIntA("display", "motion_smoothing", s->motion_smooth > 60, path) ? 120 : 60;
    GETI("display", "motion_fps", motion_smooth);
    GETI("display", "scanlines", scanlines); GETI("display", "sprite_limit", sprite_limit); GETI("sound", "mute_unfocused", mute_unfocused);
    GETI("general", "resume", resume);     GETI("general", "slot", slot);
    GETI("general", "check_updates", check_updates); GETI("general", "no_slowdown", no_slowdown);
    GETI("window", "x", win_x);            GETI("window", "y", win_y);
    for (int i = 0; i < BTN_COUNT; i++) s->keys[i] = GetPrivateProfileIntA("keys", KEY_NAMES[i], s->keys[i], path);
#undef GETI
    s->scale = clampi(s->scale, 1, 6); s->widescreen = clampi(s->widescreen, 0, WS_COUNT - 1); s->aspect = clampi(s->aspect, 0, ASPECT_COUNT - 1);
    s->volume = clampi(s->volume, 0, 100); s->language = clampi(s->language, 0, LANG_COUNT - 1);
    s->ff_speed = clampi(s->ff_speed, 0, FF_COUNT - 1); s->slot = clampi(s->slot, 1, 9);
    /* on/off values index tables in the menu: keep them 0 or 1 whatever the file says */
    int *flags[] = {&s->fullscreen, &s->integer_scale, &s->smooth, &s->show_fps, &s->mute, &s->pause_unfocused,
                    &s->vsync, &s->low_latency, &s->scanlines, &s->sprite_limit, &s->mute_unfocused, &s->resume, &s->check_updates, &s->no_slowdown};
    for (int i = 0; i < (int)(sizeof flags / sizeof flags[0]); i++) *flags[i] = *flags[i] != 0;
    for (int i = 0; i < BTN_COUNT; i++) s->keys[i] = clampi(s->keys[i], 0, 255);
    s->motion_smooth = s->motion_smooth >= 90 ? 120 : 60;   /* 240 (older versions) becomes 120 */
}

void settings_save(const char *path) {
    Settings *s = &g_set;
    char b[32];
#define PUTI(sec, name, field) (snprintf(b, sizeof b, "%d", s->field), WritePrivateProfileStringA(sec, name, b, path))
    PUTI("display", "scale", scale);       PUTI("display", "fullscreen", fullscreen);
    PUTI("display", "aspect", aspect);     PUTI("display", "integer_scale", integer_scale);
    PUTI("display", "widescreen", widescreen);
    PUTI("display", "smooth", smooth);     PUTI("display", "show_fps", show_fps);
    PUTI("sound", "volume", volume);       PUTI("sound", "mute", mute);
    PUTI("general", "language", language); PUTI("general", "pause_unfocused", pause_unfocused);
    PUTI("general", "fast_forward", ff_speed);
    PUTI("display", "vsync", vsync);       PUTI("display", "low_latency", low_latency); PUTI("display", "motion_fps", motion_smooth);
    WritePrivateProfileStringA("display", "motion_smoothing", s->motion_smooth > 60 ? "1" : "0", path);
    PUTI("display", "scanlines", scanlines); PUTI("display", "sprite_limit", sprite_limit); PUTI("sound", "mute_unfocused", mute_unfocused);
    PUTI("general", "resume", resume);     PUTI("general", "slot", slot);
    PUTI("general", "check_updates", check_updates); PUTI("general", "no_slowdown", no_slowdown);
    PUTI("window", "x", win_x);            PUTI("window", "y", win_y);
    for (int i = 0; i < BTN_COUNT; i++) PUTI("keys", KEY_NAMES[i], keys[i]);
#undef PUTI
}

double settings_par(void) { return g_set.aspect == ASPECT_TV ? 8.0 / 7.0 : 1.0; }

int settings_ws_extra(void) {
    static const double ratio[WS_COUNT] = {0, 16.0 / 9.0, 16.0 / 10.0, 21.0 / 9.0};
    if (g_set.widescreen <= WS_OFF || g_set.widescreen >= WS_COUNT) return 0;
    double w = 224.0 * ratio[g_set.widescreen] / settings_par();
    int e = (int)((w - 256.0) / 2.0 + 0.5);
    return e < 0 ? 0 : e > 144 ? 144 : e;
}

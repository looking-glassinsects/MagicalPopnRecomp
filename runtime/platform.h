/* Win32 front end: OpenGL presentation (vsync), keyboard + XInput, overlays. */
#pragma once
#include <stdint.h>

/* pseudo virtual-keys produced by the controller for menu navigation */
#define PAD_VK_BASE 0x10000
#define PAD_VK_TAB_PREV (PAD_VK_BASE | 0x1001)
#define PAD_VK_TAB_NEXT (PAD_VK_BASE | 0x1002)
#define MENU_VK_CLICK_L (PAD_VK_BASE | 0x2001)   /* mouse clicks inside the menu */
#define MENU_VK_CLICK_R (PAD_VK_BASE | 0x2002)

enum { PF_MENU = 1, PF_SCREENSHOT = 2, PF_SAVE = 4, PF_LOAD = 8, PF_PAUSE = 16, PF_SLOT_PREV = 32, PF_SLOT_NEXT = 64 };

int      plat_init(const char *title);
void     plat_shutdown(void);
int      plat_pump(void);                       /* process messages; 0 = window closed */
int      plat_take_flags(void);                 /* PF_* hotkey requests since last call */
int      plat_pop_key(int *vk);                 /* menu input queue (keyboard VK or PAD_VK_BASE|VK) */
void     plat_clear_keys(void);

void     plat_present(const uint8_t *bgrx, int width);   /* new frame (224 lines) -> screen */
void     plat_redraw(void);
/* motion smoothing: a thread that shows render(when, out) on every refresh (NULL stops it) */
void     plat_display_thread(int (*render)(double when_ms, uint8_t *bgrx));
void     plat_display_rate(int fps);    /* pictures per second the display thread aims for */
int      plat_display_thread_running(void);                     /* re-present the last frame (menu, pause) */
void     plat_apply_display(void);              /* after window size / fullscreen / vsync changes */
void     plat_toggle_fullscreen(void);
void     plat_toast(const char *utf8);          /* short message in the corner */
void     plat_set_swap_interval(int n);         /* 0 = no vsync */
double   plat_shown_fps(void);                  /* frames shown per second (last second) */
double   plat_refresh_hz(void);                 /* refresh rate of the window's monitor */
int      plat_minimized(void);

uint16_t plat_buttons(void);                    /* LakeSnes button mask from settings mapping + pad */
int      plat_fast_forward(void);
int      plat_focused(void);
double   plat_now_ms(void);
void     plat_sleep_until_ms(double t);         /* precise wait (sleep, then spin the last ~1 ms) */
void     plat_note_emulated_frame(void);
void     plat_message(const char *title_utf8, const char *text_utf8);
int      plat_ask(const char *title_utf8, const char *text_utf8);      /* Yes / No box; 1 = yes */
void     plat_exe_dir(char *out, int size);

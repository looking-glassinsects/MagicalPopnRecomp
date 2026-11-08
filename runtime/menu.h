#pragma once

enum { MA_NONE, MA_RESUME, MA_RESET, MA_QUIT, MA_SCREENSHOT, MA_SAVE, MA_LOAD };

/* UI strings (English / Japanese) used outside the menu too */
enum { T_TOAST_SCREENSHOT, T_TOAST_WELCOME, T_TOAST_FF, T_PAUSED, T_ERROR_TITLE, T_RESET_DONE,
       T_SAVED, T_LOADED, T_NO_STATE, T_RESUMED, T_SLOT, T_EMPTY, T_SAVE_FAILED, T_PAD_CONNECTED, T_COUNT };
const char *tr(int id);

/* provided by main.c: "14:32" style label for a save slot, or "" if empty */
void state_slot_info(int slot, char *out, int size);

void menu_open(void);
void menu_close(void);
int  menu_is_open(void);
int  menu_handle_key(int vk);      /* returns MA_* */
void menu_draw(void *hdc, void *bits, int w, int h);   /* into a 32-bit top-down DIB, premultiplied alpha */
void menu_mouse_move(int x, int y);
void menu_tick(void);              /* once per menu loop: the controller setup reads the controller */
int  menu_pad_setup_active(void);  /* 1 while Controls > Set up controller waits for a button */
void menu_panel_rect(void *rect);   /* RECT* out: area the last menu_draw painted */

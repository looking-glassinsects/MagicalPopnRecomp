/* Game controllers: every XInput (Xbox-style) pad, plus every other game controller through
 * Raw Input HID, with built-in layouts for common pads and a per-controller setup in the menu. */
#pragma once
#include <stdint.h>

/* buttons by position (south = bottom face button), not by printed label */
enum { VP_SOUTH, VP_EAST, VP_WEST, VP_NORTH, VP_L, VP_R, VP_SELECT, VP_START,
       VP_UP, VP_DOWN, VP_LEFT, VP_RIGHT, VP_MENU, VP_FF, VP_COUNT };
#define VPB(x) (1u << (x))

void     pad_init(void *hwnd, const char *ini_path);
/* called for every window message; never consumes it (the caller still runs DefWindowProc) */
void     pad_message(unsigned msg, uintptr_t wp, intptr_t lp);
uint32_t pad_state(void);              /* VPB bits from all controllers together */
const char *pad_last_name(void);       /* UTF-8 name of the controller used last ("" = none yet) */

/* controller setup, step by step: map one VP_* at a time on the next controller input */
int  pad_setup_begin(void);            /* 0 = no controller that can be set up is connected */
int  pad_setup_poll(int vp);           /* 1 when a new input was pressed and is now assigned to vp */
void pad_setup_end(int save);          /* save = 1 keeps the new layout (written to settings.ini) */
const char *pad_setup_device(void);    /* name of the controller being set up ("" = not chosen yet) */
void pad_reset_layout(void);           /* forget custom layouts: every controller back to its default */

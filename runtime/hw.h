/* Thin wrapper around LakeSnes (third_party/LakeSnes, MIT) used as the SNES hardware:
 * PPU, SPC700 + S-DSP, DMA/HDMA, timers, auto-joypad, cartridge mapping.
 * LakeSnes' 65816 interpreter is only used for code the build didn't recompile and to
 * resume from save states. Kept in its own translation unit because LakeSnes also
 * defines a type named Cpu. */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* vblank_cb runs at the start of every vblank, after the frame is drawn and before the
 * game's controller read is latched: present + input happen there. */
int      hw_init(const uint8_t *rom, size_t size, void (*vblank_cb)(void));
void     hw_reset(void);
void     hw_free(void);
uint32_t hw_frame_count(void);

/* CPU bus accesses: advance the hardware by the real access time, then read/write. */
uint8_t  hw_read(uint32_t addr);
void     hw_write(uint32_t addr, uint8_t v);
/* opcode fetch + internal cycles for one recompiled instruction (no data transfer) */
void     hw_fetch(uint8_t bank, int nbytes, int idle);
void     hw_idle(int master_cycles);

/* polled by generated code at loop back-edges */
extern const volatile uint8_t  *hw_nmi_ptr, *hw_irq_ptr;
void     hw_ack_nmi(void);

typedef struct HwRegs { uint16_t a, x, y, s, d; uint8_t db, p, e; } HwRegs;
typedef struct HwInterpHooks {
    int      (*has_native)(uint32_t target, int m, int x);     /* recompiled routine exists? */
    uint32_t (*call_native)(HwRegs *r, uint32_t target);       /* run it; returns where it returned to */
    int      (*want_capture)(void);                            /* save state requested? */
    void     (*capture)(const HwRegs *r, uint32_t pc24);       /* take it at this exact boundary */
} HwInterpHooks;
void     hw_set_interp_hooks(const HwInterpHooks *h);
/* Interpret from pc24 (calling native code for known routines) until the code returns past
 * the stack level it was entered at, or forever if `forever`. Updates regs; returns the
 * address it returned to. */
uint32_t hw_interpret(HwRegs *r, uint32_t pc24, int forever, unsigned long long *steps);

/* save states of the whole hardware */
int      hw_state_size(void);
int      hw_save_state(uint8_t *buf);
int      hw_load_state(uint8_t *buf, int size);

void     hw_end_frame(void);                  /* catch up the APU */
void     hw_get_pixels(uint8_t *bgrx512x480); /* BGRX, 512x480 (lines 16..463 visible) */
/* Widescreen: render `extra` more columns on each side (0 = off). profile 1 = Magical Pop'n level-map source, 0 = generic. */
void     hw_set_widescreen(int extra, int profile);
void     hw_set_sprite_limit(int on);
uint32_t hw_sprite_overflow_lines(int *max_sprites, int *max_slivers);   /* 0: draw every sprite on a line */
#define  HW_WIDE_MAX_W 544                    /* 256 + 2 * PPU_WS_MAX */
int      hw_get_pixels_wide(uint8_t *bgrx);   /* 224 lines of (returned width) BGRX pixels */
void     hw_get_samples(int16_t *stereo, int count);
int      hw_take_samples(int16_t *stereo, int max_frames);   /* new ~32 kHz samples since last call */
#define  HW_AUDIO_RATE 32040.0
void     hw_set_buttons(uint16_t mask);       /* bit = LakeSnes button index */
const uint8_t *hw_wram(void);
uint16_t hw_bg_hscroll(int layer);
const uint32_t *hw_ws_stats(void);
const uint16_t *hw_line_hscroll(int layer);  /* test: BG1/BG2 horizontal scroll of each line (1..224) of the last frame */
int      hw_bg_vscroll(int layer);
const uint16_t *hw_line_vscroll(int layer);         /* test: sprite pixels by the 4:3 edges (turns counting on) */
int      hw_ws_failed(const char **info);   /* diagnostics: gameplay frame left in 4:3 */
/* Motion smoothing: record each frame's scanline registers, then draw in-between frames. */
void     hw_interp_enable(int on);
int      hw_interp_ready(void);                       /* previous and current frame recorded */
int      hw_render_interp(float t, uint8_t *bgrx);    /* frame at t in (0,1) between them; returns width */
int      hw_snapshot_publish(double t0_ms);           /* after a frame: hand it to the display thread */
void     hw_set_extra_time(int lines, double budget_ms);   /* extra CPU lines per frame for busy frames (0 = original slowdown) */
void     hw_lag_stats(uint32_t *lag_frames, uint32_t *lines);   /* frames the game didn't finish in time; lines inserted */
int      hw_ws_state_size(void);                       /* widescreen bookkeeping kept in save states */
void     hw_ws_state_save(uint8_t *buf);
int      hw_ws_state_load(const uint8_t *buf, int size);
int      hw_snapshot_render(double when_ms, uint8_t *bgrx);   /* display thread: picture for that moment */
#define  HW_FINE_MAX_W (2 * HW_WIDE_MAX_W)
extern volatile int hw_fine_on;        /* 0: draw whole pixels only (too slow for this screen) */
int      hw_snapshot_render_fine(double when_ms, uint8_t *bgrx); /* the same at half-pixel precision across (2 x width columns); returns the width */

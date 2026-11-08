/* Build-time trace profiler. Runs the ROM in LakeSnes' 65816 interpreter while an input bot
 * plays, and records every code entry point the game really uses, with the M/X register
 * widths it was entered with:
 *   call    JSR/JSL targets            jump    JMP/JML (indirect) targets
 *   int     interrupt handlers         resume  returns that don't match a call (stack tricks)
 * The tracer turns these into recompiled functions. Nothing about any particular dump is
 * hard-coded, so this works for every version of the game.
 *
 *   popn_trace.exe ROM OUT.txt MODE SEED FRAMES      MODE = attract | play | menus      */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes.h"
#include "cpu.h"
#include "png_write.h"

enum { B_B, B_Y, B_SELECT, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_X, B_L, B_R };
#define BIT(b) (1u << (b))

static uint32_t canon(uint32_t a) {
    uint8_t bank = (uint8_t)(a >> 16);
    if (bank == 0x7E || bank == 0x7F || (a & 0xFFFF) < 0x8000) return a;
    return a | 0x800000;
}

/* ---- entry set (open addressing) ---- */
#define SET_SIZE (1 << 18)
static uint32_t set_keys[SET_SIZE];
static char set_kind[SET_SIZE];
static int set_count;
static void add_entry(uint32_t addr, int m, int x, char kind) {
    if ((addr & 0xFFFF) < 0x8000 && (addr >> 16) != 0x7E && (addr >> 16) != 0x7F) return; /* I/O, not code */
    uint32_t key = (addr << 2) | (m << 1) | x | 0x80000000u;
    uint32_t h = (key * 2654435761u) & (SET_SIZE - 1);
    while (set_keys[h] && set_keys[h] != key) h = (h + 1) & (SET_SIZE - 1);
    if (!set_keys[h] && set_count < SET_SIZE / 2) { set_keys[h] = key; set_kind[h] = kind; set_count++; }
}

/* ---- shadow call stack ---- */
static uint32_t shadow[8192];
static int sdepth;
static void push_ret(uint32_t r) { if (sdepth < 8192) shadow[sdepth++] = r; else { memmove(shadow, shadow + 1024, sizeof(uint32_t) * 7168); sdepth = 7168; shadow[sdepth++] = r; } }

/* ---- input bot ---- */
static uint32_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint16_t hold_mask; static int hold_left;
static int g_stall_pulse;
static int g_stall_threshold = 64;
static uint8_t g_prev_wram[0x20000];

static uint16_t bot(const char *mode, uint32_t f) {
    if (!strcmp(mode, "attract")) return 0;
    if (!strcmp(mode, "menus")) {
        static const int pool[] = {B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_B, B_START, B_DOWN, B_A};
        if (f % 24 < 6) { if (f % 24 == 0) hold_mask = (uint16_t)BIT(pool[rnd() % 9]); return hold_mask; }
        return 0;
    }
    /* play: if the game looks stalled (paused, title, menu, game over), press Start once;
     * otherwise hold random directions/buttons. "Stalled" is measured from WRAM activity. */
    if (g_stall_pulse > 0) { g_stall_pulse--; return BIT(B_START); }
    if (f % 45 < 4 && f < 60 * 20) return BIT(B_A);
    if (hold_left-- <= 0) {
        uint32_t r = rnd() % 100;
        uint16_t m = r < 35 ? BIT(B_RIGHT) : r < 55 ? BIT(B_LEFT) : r < 65 ? BIT(B_UP) : r < 75 ? BIT(B_DOWN)
                   : r < 80 ? BIT(B_UP) | BIT(B_RIGHT) : r < 85 ? BIT(B_DOWN) | BIT(B_RIGHT) : 0;
        static const int btn[] = {B_B, B_Y, B_A, B_X};
        for (int i = 0; i < 4; i++) if (rnd() % 100 < 30) m |= BIT(btn[i]);
        if (rnd() % 100 < 5) m |= BIT(B_L);
        if (rnd() % 100 < 5) m |= BIT(B_R);
        hold_mask = m; hold_left = 6 + (int)(rnd() % 40);
    }
    return hold_mask;
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: popn_trace ROM OUT MODE SEED FRAMES\n"); return 2; }
    const char *mode = argv[3];
    rng = (uint32_t)strtoul(argv[4], 0, 10) * 2654435761u + 12345;
    uint32_t frames = (uint32_t)strtoul(argv[5], 0, 10);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open ROM\n"); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc((size_t)size);
    if (fread(rom, 1, (size_t)size, f) != (size_t)size) return 1;
    fclose(f);

    Snes *s = snes_init();
    if (!snes_loadRom(s, rom, (int)size)) return 1;
    Cpu *c = s->cpu;
    uint8_t *executed = calloc((size_t)size, 1);
    unsigned long long steps = 0;

    const char *shots = getenv("POPN_TRACE_SHOTS");   /* debug: dump a PNG every 600 frames */
    snes_setPixelFormat(s, pixelFormatXRGB);
    while (s->frames < frames) {
        if (s->frames % 60 == 59 && !strcmp(mode, "play")) {
            int diff = 0;
            for (int i = 0; i < 0x20000; i++) diff += s->ram[i] != g_prev_wram[i];
            memcpy(g_prev_wram, s->ram, sizeof g_prev_wram);
            if (getenv("POPN_TRACE_SHOTS")) fprintf(stderr, "frame %u wram diff %d\n", s->frames, diff);
            if (diff < g_stall_threshold) g_stall_pulse = 6;
        }
        s->input1->currentState = bot(mode, s->frames);
        if (shots && s->frames % 600 == 0) {
            static uint8_t px[512 * 480 * 4], rgb[256 * 224 * 3]; char pth[1024];
            snes_setPixels(s, px); png_from_snes(rgb, px);
            snprintf(pth, sizeof pth, "%s/%s_%05u.png", shots, mode, s->frames); png_write_rgb(pth, rgb, 256, 224);
        }
        uint32_t fr = s->frames;
        while (s->frames == fr) {
            uint32_t pre = ((uint32_t)c->k << 16) | c->pc;
            int irq = c->intWanted && !c->waiting && !c->stopped && !c->resetWanted;
            int reset = c->resetWanted;
            uint8_t op = (irq || reset) ? 0xEA : snes_read(s, pre);
            if (!irq && !reset && !c->waiting && (pre & 0xFFFF) >= 0x8000 && (pre >> 16) != 0x7E && (pre >> 16) != 0x7F) {
                uint32_t fo = ((pre >> 16) & 0x7F) * 0x8000u + ((pre & 0xFFFF) - 0x8000u);
                if (fo < (uint32_t)size) executed[fo] = 1;
            }
            cpu_runOpcode(c);
            steps++;
            uint32_t post = canon(((uint32_t)c->k << 16) | c->pc);
            int m = c->mf, x = c->xf;
            if (reset) { add_entry(post, m, x, 'i'); continue; }
            if (irq) { push_ret(canon(pre)); add_entry(post, m, x, 'i'); continue; }
            switch (op) {
            case 0x20: case 0xFC:
                push_ret(canon((pre & 0xFF0000) | ((pre + 3) & 0xFFFF))); add_entry(post, m, x, 'c'); break;
            case 0x22:
                push_ret(canon((pre & 0xFF0000) | ((pre + 4) & 0xFFFF))); add_entry(post, m, x, 'c'); break;
            case 0x6C: case 0x7C: case 0xDC:
                add_entry(post, m, x, 'j'); break;
            case 0x60: case 0x6B: case 0x40: {
                int i = sdepth - 1;
                while (i >= 0 && shadow[i] != post) i--;
                if (i >= 0) sdepth = i;               /* normal return, or unwinding several frames */
                else add_entry(post, m, x, 'r');      /* returned somewhere no call expects */
                break;
            }
            }
        }
        (void)snes_readBBus(s, 0x40);   /* keep the APU caught up (it drives game timing) */
    }

    FILE *o = fopen(argv[2], "w");
    if (!o) return 1;
    static const char *kinds[] = {"call", "jump", "int", "resume"};
    for (int i = 0; i < SET_SIZE; i++) {
        if (!set_keys[i]) continue;
        uint32_t k = set_keys[i] & 0x7FFFFFFF;
        const char *kd = set_kind[i] == 'c' ? kinds[0] : set_kind[i] == 'j' ? kinds[1] : set_kind[i] == 'i' ? kinds[2] : kinds[3];
        fprintf(o, "%06X %u %u %s\n", k >> 2, (k >> 1) & 1, k & 1, kd);
    }
    fclose(o);
    size_t ex = 0;
    for (long i = 0; i < size; i++) ex += executed[i];
    printf("mode=%s seed=%s frames=%u steps=%llu entries=%d executed_insn_starts=%zu\n",
           mode, argv[4], frames, steps, set_count, ex);
    /* executed instruction map, for coverage statistics */
    char path[1024]; snprintf(path, sizeof path, "%s.exec", argv[2]);
    FILE *e = fopen(path, "wb"); if (e) { fwrite(executed, 1, (size_t)size, e); fclose(e); }
    return 0;
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hw.h"
#include "snes.h"
#include "cpu.h"
#include "dma.h"
#include "ppu.h"
#include "apu.h"
#include "dsp.h"

static Snes *g;
int rt_ws_extra;   /* sprite-draw widening used by the patched draw helpers (see tools/emit_c.py) */
const volatile uint8_t *hw_nmi_ptr, *hw_irq_ptr;
static void (*g_vblank_cb)(void);
static HwInterpHooks g_hooks;
static int g_samples_primed;
static void ws_reset_anchors(void);

static void ip_end_frame(void);
static void ip_invalidate(void);
static void vblank_tramp(void *user) { (void)user; ip_end_frame(); if (g_vblank_cb) g_vblank_cb(); }

int hw_init(const uint8_t *rom, size_t size, void (*vblank_cb)(void)) {
    g = snes_init();
    if (!snes_loadRom(g, rom, (int)size)) return 0;
    snes_setPixelFormat(g, pixelFormatXRGB);  /* byte order B,G,R,X = Windows DIB */
    hw_nmi_ptr = (const volatile uint8_t *)&g->cpu->nmiWanted;
    hw_irq_ptr = (const volatile uint8_t *)&g->cpu->irqWanted;
    g_vblank_cb = vblank_cb;
    g->vblankHook = vblank_tramp;
    return 1;
}

void hw_set_interp_hooks(const HwInterpHooks *h) { g_hooks = *h; }
void hw_reset(void) { snes_reset(g, true); g_samples_primed = 0; ws_reset_anchors(); ip_invalidate(); }
void hw_free(void) { if (g) snes_free(g); g = 0; }
uint32_t hw_frame_count(void) { return g->frames; }

uint8_t hw_read(uint32_t addr) { return snes_cpuRead(g, addr); }
void hw_write(uint32_t addr, uint8_t v) { snes_cpuWrite(g, addr, v); }

void hw_idle(int cycles) {
    dma_handleDma(g->dma, cycles);
    snes_runCycles(g, cycles);
}

void hw_fetch(uint8_t bank, int nbytes, int idle) {
    int per = (g->fastMem && bank >= 0x80) ? 6 : 8;
    hw_idle(nbytes * per + idle * 6);
}

void hw_ack_nmi(void) { g->cpu->nmiWanted = false; }

static void regs_to_cpu(Cpu *c, const HwRegs *r, uint32_t pc24) {
    c->a = r->a; c->x = r->x; c->y = r->y; c->sp = r->s; c->dp = r->d; c->db = r->db;
    c->k = (uint8_t)(pc24 >> 16); c->pc = (uint16_t)pc24;
    uint8_t p = r->p;
    c->n = p & 0x80; c->v = p & 0x40; c->mf = p & 0x20; c->xf = p & 0x10;
    c->d = p & 0x08; c->i = p & 0x04; c->z = p & 0x02; c->c = p & 0x01;
    c->e = r->e;
    c->waiting = c->stopped = false;
    c->intWanted = false;
    c->resetWanted = false;
}

static void cpu_to_regs(const Cpu *c, HwRegs *r) {
    r->a = c->a; r->x = c->x; r->y = c->y; r->s = c->sp; r->d = c->dp; r->db = c->db;
    r->p = (uint8_t)((c->n << 7) | (c->v << 6) | (c->mf << 5) | (c->xf << 4) | (c->d << 3) | (c->i << 2) | (c->z << 1) | c->c);
    r->e = c->e;
}

static uint8_t peek(uint32_t a) { return snes_read(g, a & 0xFFFFFF); }

/* Interpret from pc24. Unless `forever`, stops once an RTS/RTL/RTI (or a native callee)
 * returns past the stack level we started at. JSR/JSL into recompiled routines run the
 * native code. State captures happen at instruction boundaries when requested. */
uint32_t hw_interpret(HwRegs *r, uint32_t pc24, int forever, unsigned long long *steps) {
    Cpu *c = g->cpu;
    regs_to_cpu(c, r, pc24);
    uint16_t entry_s = r->s;
    for (;;) {
        int boundary = !c->intWanted && !c->waiting && !c->stopped;
        uint32_t pc = ((uint32_t)c->k << 16) | c->pc;
        uint8_t op = boundary ? peek(pc) : 0xEA;
        if (boundary && g_hooks.want_capture && g_hooks.want_capture()) {
            cpu_to_regs(c, r);
            g_hooks.capture(r, pc);
        }
        if (boundary && (op == 0x20 || op == 0x22) && g_hooks.has_native) {
            uint32_t target = op == 0x20
                ? (pc & 0xFF0000) | peek(pc + 1) | (peek(pc + 2) << 8)
                : peek(pc + 1) | (peek(pc + 2) << 8) | ((uint32_t)peek(pc + 3) << 16);
            if (g_hooks.has_native(target, c->mf, c->xf)) {
                uint16_t ret = (uint16_t)(c->pc + (op == 0x20 ? 2 : 3));
                hw_fetch((uint8_t)(pc >> 16), op == 0x20 ? 3 : 4, 1);
                if (op == 0x22) { snes_cpuWrite(g, c->sp, c->k); c->sp--; }
                snes_cpuWrite(g, c->sp, (uint8_t)(ret >> 8)); c->sp--;
                snes_cpuWrite(g, c->sp, (uint8_t)ret); c->sp--;
                cpu_to_regs(c, r);
                uint32_t back = g_hooks.call_native(r, target);
                regs_to_cpu(c, r, back);
                if (steps) (*steps)++;
                if (!forever && (uint16_t)(c->sp - entry_s) < 0x8000 && c->sp != entry_s) break;
                continue;
            }
        }
        cpu_runOpcode(c);
        if (steps) (*steps)++;
        /* returned past the level we entered at: hand control back to recompiled code */
        if (!forever && (op == 0x60 || op == 0x6B || op == 0x40) && (uint16_t)(c->sp - entry_s) < 0x8000 && c->sp != entry_s) break;
    }
    c->intWanted = false;
    cpu_to_regs(c, r);
    return ((uint32_t)c->k << 16) | c->pc;
}

/* ---- save states (whole hardware: RAM, VRAM, PPU, APU/DSP, DMA, timers) ---- */
int hw_state_size(void) { return snes_saveState(g, NULL); }
int hw_save_state(uint8_t *buf) { return snes_saveState(g, buf); }
int hw_load_state(uint8_t *buf, int size) {
    if (!snes_loadState(g, buf, size)) return 0;
    g_samples_primed = 0;
    ws_reset_anchors();
    ip_invalidate();
    return 1;
}

void hw_end_frame(void) {
    /* LakeSnes only catches the APU up on port access or in snes_runFrame (which we
     * don't use); a side-effect-free port read forces the catch-up each frame. */
    (void)snes_readBBus(g, 0x40);
}

void hw_get_pixels(uint8_t *px) { snes_setPixels(g, px); }
/* ---------------------------------------------------------------- widescreen
 * Magical Pop'n keeps each gameplay layer's level map decoded in WRAM as a ring buffer
 * (BG1: $7E:C100, BG2: $7F:C100; row stride in $84 / $9A, ring width = stride / 2) and only
 * streams the column entering the screen into its 512 px VRAM tilemap. So the
 * widescreen columns are drawn straight from the ring. Each frame the ring column that the
 * visible VRAM columns correspond to is found by matching, so the scheme doesn't depend on
 * camera variables or code addresses, and it switches itself off (black side areas) on
 * screens that don't use this layout: title, menus, cutscenes. */
/* scrolled: the camera has moved since this scene's anchor was found. Cutscenes and menus
 * that happen to use the same map system never scroll, so they stay pillarboxed. */
typedef struct { int delta; int locked; int first_h; int stride; int scrolled; int rdelta; int miss; } WsAnchor;
static WsAnchor g_anchor[2] = {{-1, 0, -1, 0, 0, 0, 0}, {-1, 0, -1, 0, 0, 0, 0}};
static void ws_reset_anchors(void) { for (int i = 0; i < 2; i++) { g_anchor[i].delta = -1; g_anchor[i].locked = 0; g_anchor[i].first_h = -1; g_anchor[i].stride = 0; g_anchor[i].scrolled = 0; g_anchor[i].miss = 0; } }

static int g_dbg_best, g_dbg_best_d, g_dbg_best_rd, g_dbg_total, g_dbg_nseen, g_dbg_odd;
static int g_ws_failed; static char g_ws_fail_info[200];
int hw_ws_failed(const char **info) { if (info) *info = g_ws_fail_info; return g_ws_failed; }
/* Find where the visible part of the hardware tilemap sits in the level map. Columns: ring
 * offset `delta`. Rows: the tilemap is a 32-row ring too, so level row = screen row + 32*k for
 * some k (a->rdelta receives that multiple of 32 rows, the height of the tilemap). Only the rows that
 * are actually on screen are compared. */
/* Scenes drawn in bands that scroll separately (rooftops under a slow sky): each sample row's
 * visible tilemap columns follow its own line's scroll, taken from the last frame relative to its
 * first line. A row of another band may show tilemap columns that sit a whole tilemap width away
 * in the level map; `pg` gets the first page to try for it (0 = scrolls with the first line). */
static void ws_rows(const Ppu *ppu, int layer, int mc0, int *mcj, int *pg) {
    int h = ppu->bgLayer[layer].hScroll & 0x3ff;
    for (int j = 0; j < 14; j++) {
        int line = 1 + 16 * j;
        int off = ((ppu->wsLineH[layer][line] - ppu->wsLineH[layer][1] + 512) & 0x3ff) - 512;
        mcj[j] = ((h + off) & 0x3ff) >> 4;
        pg[j] = mcj[j] == mc0 ? 0 : mc0 + (((mcj[j] - mc0 + 16) & 31) - 16) - mcj[j];
    }
}
/* matches in one sample row at anchor (d, rd): rows of the first line's band only at page 0,
 * others at whichever nearby page fits best */
static int ws_row_score(const uint8_t *base, int stride, int w, int row, int mc, int pg, int banded, const uint16_t *vc) {
    int best = 0;
    for (int i = 0; i < (banded ? 3 : 1); i++) {
        int p = pg + (i == 0 ? 0 : i == 1 ? -32 : 32), s = 0;
        const uint8_t *r = base + row * stride;
        for (int k = 0; k < 17; k++) {
            const uint8_t *q = r + ((((mc + k + p) % w) + w) % w) * 2;
            s += (uint16_t)(q[0] | (q[1] << 8)) == vc[k];
        }
        if (s > best) best = s;
    }
    return best;
}

static int ws_match(Ppu *ppu, int layer, const uint8_t *base, int stride, int rows, WsAnchor *a) {
    int w = stride / 2;
    int h = ppu->bgLayer[layer].hScroll & 0x3ff, mc0 = h >> 4;
    int v = ppu->bgLayer[layer].vScroll & 0x3ff;
    uint16_t map = ppu->bgLayer[layer].tilemapAdr;
    enum { NR = 14 };
    int total = 17 * NR, best = -1, best_d = -1, best_rd = 0;
    static uint16_t vcol[17][NR], vrow[NR][17];
    int lrow[NR];                                    /* unwrapped screen-row index of each sample */
    int mcj[NR], pg[NR];
    ws_rows(ppu, layer, mc0, mcj, pg);
    for (int j = 0; j < NR; j++) lrow[j] = (1 + 16 * j + v) >> 4;
    for (int k = 0; k < 17; k++)
        for (int j = 0; j < NR; j++)
            vrow[j][k] = vcol[k][j] = ppu->vram[(map + (lrow[j] & 31) * 32 + ((mcj[j] + k) & 31)) & 0x7fff];
    /* an (almost) uniform window matches anything; only anchor on real structure */
    {
        uint16_t seen[8]; int nseen = 0, odd = 0;
        for (int k = 0; k < 17; k++)
            for (int j = 0; j < NR; j++) {
                uint16_t t = vcol[k][j]; int f = 0;
                for (int i = 0; i < nseen; i++) if (seen[i] == t) { f = 1; break; }
                if (!f && nseen < 8) seen[nseen++] = t;
                if (t != vcol[0][0]) odd++;
            }
        g_dbg_nseen = nseen; g_dbg_odd = odd; g_dbg_best = -1; g_dbg_total = total;
        if (nseen < 4 || odd < 24) { a->locked = 0; return -1; }
    }
    /* the tilemap repeats every 32 rows, so tall rooms can sit at any multiple of 32 */
    for (int kk = -2; kk * 32 < rows; kk++) {
        int rd = 32 * kk;
        int ok_rows = 1;
        for (int j = 0; j < NR; j++) { int r = lrow[j] + rd; if (r < 0 || r >= rows) { ok_rows = 0; break; } }
        if (!ok_rows) continue;
        for (int d = 0; d < w; d++) {
            int score = 0;
            for (int j = 0; j < NR; j++)
                score += ws_row_score(base, stride, w, lrow[j] + rd, mcj[j] + d, pg[j], mcj[j] != mc0, vrow[j]);
            if (d == a->delta && rd == a->rdelta) score += 1;   /* prefer last frame's anchor on ties */
            if (score > best) { best = score; best_d = d; best_rd = rd; }
        }
    }
    g_dbg_best = best; g_dbg_best_d = best_d; g_dbg_best_rd = best_rd;
    if (best * 100 < total * 92) { a->locked = 0; return -1; }
    a->delta = best_d; a->rdelta = best_rd; a->locked = 1;
    return best_d;
}

/* Fraction (per mille) of the visible tilemap window that matches the level map at a given
 * column/row anchor. Used when a layer is too featureless to anchor by itself. */
static int ws_verify(Ppu *ppu, int layer, const uint8_t *base, int stride, int rows, int d, int rd) {
    int w = stride / 2, h = ppu->bgLayer[layer].hScroll & 0x3ff, v = ppu->bgLayer[layer].vScroll & 0x3ff;
    uint16_t map = ppu->bgLayer[layer].tilemapAdr;
    int ok = 0, n = 0, mcj[14], pg[14];
    ws_rows(ppu, layer, h >> 4, mcj, pg);
    for (int j = 0; j < 14; j++) {
        int lr = (1 + 16 * j + v) >> 4, r = lr + rd;
        if (r < 0 || r >= rows) return 0;
        uint16_t vc[17];
        for (int k = 0; k < 17; k++) vc[k] = ppu->vram[(map + (lr & 31) * 32 + ((mcj[j] + k) & 31)) & 0x7fff];
        ok += ws_row_score(base, stride, w, r, mcj[j] + d, pg[j], mcj[j] != (h >> 4), vc);
        n += 17;
    }
    return ok * 1000 / n;
}

/* The back layer (BG2) extended straight from its hardware tilemap, with each line's own scroll.
 * The game refreshes that map piece by piece, briefly leaving holes in parts that are off-screen
 * in 4:3, so this uses a copy where a tile is taken over only after it has held the same value
 * for two frames. */
/* lvl (optional): the layer's level map (stride 64 = 32 columns, `rows` rows, anchor d/rd); a
 * tilemap word the game hasn't written since the last load (room set-up) is still the previous
 * room's, so the level map's tile for that spot is used instead */
static const uint8_t *ws_vram_copy_lvl(Ppu *ppu, int l, const uint8_t *lvl, int rows, int d, int rd) {
    static uint8_t vprev[2][0x800], vstable[2][0x800];
    static int vmap[2] = {-1, -1};
    uint16_t map = ppu->bgLayer[l].tilemapAdr;
    uint8_t cur[0x800];
    int top = ((ppu->bgLayer[l].vScroll & 0x3ff) >> 4) - 4;
    static int mdbg = -1; if (mdbg < 0) mdbg = getenv("POPN_MERGE_DEBUG") ? atoi(getenv("POPN_MERGE_DEBUG")) : 0;
    for (int i = 0; i < 0x400; i++) {
        uint16_t w = ppu->vram[(map + i) & 0x7fff];
        int trusted = (ppu->wsVramEpoch[(map + i) & 0x7fff] & 0x7fff) == ppu->wsEpoch;
        uint16_t lw = 0xFFFF;
        if (lvl) {
            int r = i >> 5, c = i & 31;
            int lr = top + ((r - top) & 31), R = lr + rd;
            if (R >= 0 && R < rows) { const uint8_t *p = lvl + R * 64 + (((c + d) & 31) * 2); lw = (uint16_t)(p[0] | (p[1] << 8)); }
        }
        if (lvl && !trusted && lw != 0xFFFF) w = lw;
        if (lvl && mdbg && (i >> 5) == ((top + mdbg) & 31))
            fprintf(stderr, "merge f%u L%d c%d trusted %d load %d vram %04x level %04x used %04x\n", hw_frame_count(), l, i & 31, trusted,
                    (ppu->wsVramEpoch[(map + i) & 0x7fff] >> 15) & 1, ppu->vram[(map + i) & 0x7fff], lw, w);
        cur[i * 2] = (uint8_t)w; cur[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    if (vmap[l] != map) { memcpy(vprev[l], cur, sizeof cur); memcpy(vstable[l], cur, sizeof cur); vmap[l] = map; }
    else for (int i = 0; i < 0x800; i += 2) {
        if (cur[i] == vprev[l][i] && cur[i + 1] == vprev[l][i + 1]) { vstable[l][i] = cur[i]; vstable[l][i + 1] = cur[i + 1]; }
        else { vprev[l][i] = cur[i]; vprev[l][i + 1] = cur[i + 1]; }
    }
    return vstable[l];
}
/* the room's set-up wrote (nearly) the layer's whole tilemap: a room drawn once and never
 * streamed (the throne room), so its tilemap is the picture. Rooms that stream set up only the
 * part on screen and keep filler elsewhere in the tilemap. */
static int ws_tilemap_complete(Ppu *ppu, int l) {
    uint16_t map = ppu->bgLayer[l].tilemapAdr; int n = 0;
    for (int i = 0; i < 0x400; i++) n += ppu->wsVramEpoch[(map + i) & 0x7fff] == (ppu->wsEpoch | 0x8000);
    return n >= 1000;
}
/* point a layer's extension at its hardware tilemap (keeps its clamp settings); for a narrow
 * room's BG1 the level map fills in what the game hasn't drawn since the room was set up */
static void ws_src_vram_lvl(Ppu *ppu, int l, const uint8_t *lvl, int rows, int d, int rd) {
    ppu->wsSrc[l].valid = true; ppu->wsSrc[l].base = ws_vram_copy_lvl(ppu, l, lvl, rows, d, rd); ppu->wsSrc[l].stride = 64;
    ppu->wsSrc[l].width = 32; ppu->wsSrc[l].rows = 32; ppu->wsSrc[l].delta = 0; ppu->wsSrc[l].rdelta = 0;
    ppu->wsSrc[l].vring = 1; ppu->wsSrc[l].refCol = -1;
}
static void ws_src_vram(Ppu *ppu, int l) {
    const uint8_t *lvl = NULL; int rows = 0, d = 0, rd = 0;
    if (l == 0 && ppu->wsSrc[0].valid && !ppu->wsSrc[0].vring && ppu->wsSrc[0].stride == 64) {
        lvl = ppu->wsSrc[0].base; rows = ppu->wsSrc[0].rows; d = ppu->wsSrc[0].delta; rd = ppu->wsSrc[0].rdelta;
    }
    ws_src_vram_lvl(ppu, l, lvl, rows, d, rd);
}
static void ws_back_from_vram(Ppu *ppu) {
    ws_src_vram(ppu, 1);
    ppu->wsSrc[1].clamp = false;
}

/* debug switches, read once: POPN_WS_DEBUG=level, POPN_NO_SPRITE_WS, POPN_WS_LAYERS, POPN_HIDE_BG=mask */
static struct { int init, level, no_sprite_ws, layers, hide; } g_wsdbg;
static void ws_debug_init(void) {
    const char *e = getenv("POPN_WS_DEBUG");
    g_wsdbg.level = e ? (atoi(e) ? atoi(e) : 1) : 0;
    g_wsdbg.no_sprite_ws = getenv("POPN_NO_SPRITE_WS") != NULL;
    g_wsdbg.layers = getenv("POPN_WS_LAYERS") != NULL;
    e = getenv("POPN_HIDE_BG"); g_wsdbg.hide = e ? atoi(e) : 0;
    g_wsdbg.init = 1;
}

static void popn_ws_hook(Ppu *ppu, void *user) {
    Snes *s = (Snes *)user;
    if (!g_wsdbg.init) ws_debug_init();
    const int dbg = g_wsdbg.level;
    static const struct { int bank_off, stride_var; } L[2] = {{0x0000, 0x84}, {0x10000, 0x9A}};
    ppu->wsBgMask = 0;
    ppu->wsCamLine = 1;
    for (int l = 0; l < 2; l++) {
        ppu->wsSrc[l].valid = false; ppu->wsSrc[l].refCol = -1;
        /* A back layer that scrolls independently (parallax, often with a different scroll per
         * band of lines) never streams its tiles: its hardware map already repeats correctly,
         * so it extends straight from VRAM with each line's own scroll. */
        /* Some parallax layers are wider than the hardware map, though, and the game streams them
         * just like the playfield (e.g. the misty forest): when the visible window matches the
         * back layer's WRAM level map, extend from that instead (handled by the code below). */
        int parallax_ring = 0;
        if (l == 1 && (ppu->bgLayer[1].hScroll & 0x3ff) != (ppu->bgLayer[0].hScroll & 0x3ff) &&
            ppu->mode == 1 && ppu->bgLayer[1].bigTiles && !ppu->bgLayer[1].tilemapWider) {
            int st = s->ram[0x9A] | (s->ram[0x9B] << 8);
            if (st > 64 && st <= 1024 && !(st & 1)) {
                WsAnchor tmp = g_anchor[1];
                parallax_ring = ws_match(ppu, 1, s->ram + 0x10000 + 0xC100, st, 0x3F00 / st, &tmp) >= 0;
            }
        }
        if (l == 1 && !parallax_ring && (ppu->bgLayer[1].hScroll & 0x3ff) != (ppu->bgLayer[0].hScroll & 0x3ff)) {
            if (!ppu->wsSrc[0].valid || !ppu->bgLayer[1].bigTiles || ppu->bgLayer[1].tilemapWider) continue;
            ws_back_from_vram(ppu);
            continue;
        }
        int stride = s->ram[L[l].stride_var] | (s->ram[L[l].stride_var + 1] << 8);
        if (dbg && (s->frames % 60 == 0 || dbg == 2))
            fprintf(stderr, "ws f%u BG%d main %d%d%d%d%d sub %d%d%d%d%d cam62 %04x bg3 h%d v%d map %04x m7c %02x big %d wider %d higher %d stride %d\n", s->frames, l + 1,
                    ppu->layer[0].mainScreenEnabled, ppu->layer[1].mainScreenEnabled, ppu->layer[2].mainScreenEnabled,
                    ppu->layer[3].mainScreenEnabled, ppu->layer[4].mainScreenEnabled, ppu->layer[0].subScreenEnabled,
                    ppu->layer[1].subScreenEnabled, ppu->layer[2].subScreenEnabled, ppu->layer[3].subScreenEnabled,
                    ppu->layer[4].subScreenEnabled, s->ram[0x62] | (s->ram[0x63] << 8), ppu->bgLayer[2].hScroll,
                    ppu->bgLayer[2].vScroll, ppu->bgLayer[2].tilemapAdr, 0,
                    ppu->bgLayer[l].bigTiles, ppu->bgLayer[l].tilemapWider, ppu->bgLayer[l].tilemapHigher, stride);
        if (ppu->mode != 1 || !ppu->bgLayer[l].bigTiles || ppu->bgLayer[l].tilemapWider) continue;
        if (stride <= 32 || stride > 1024 || (stride & 1)) continue;   /* <= 256 px: nothing beyond the screen */
        /* A map exactly as wide as the 32-column hardware tilemap is either a small room (play
         * area 512 px wide) or a story scene that page-flips between the two halves. Rooms are
         * accepted only when the game's camera agrees with the scroll register (checked below). */
        int narrow = stride <= 64;
        /* gameplay screens show the status bar on BG3 with a -4 horizontal offset; story
         * scenes don't, so a narrow map without it is a cutscene */
        if (narrow && (ppu->bgLayer[2].hScroll & 0x3ff) != 0x3FC) continue;
        /* Narrow rooms (no wider than the 32-column hardware tilemap) can differ from their level
         * map. One that the game set up whole and never streams (the throne room) is drawn from
         * its tilemap (BG1 below, BG2 here). In the others, BG2 is drawn from its tilemap when its
         * level map doesn't match it (castle rooms that draw the back wall into the tilemap only);
         * otherwise both use the level map like any other room, since a streaming room's
         * off-screen tilemap entries can be left over from rows or columns the view has passed. */
        WsAnchor probe = g_anchor[1];
        if (narrow && l == 1 && ppu->wsSrc[0].valid && ppu->wsSrc[0].clamp &&
            (ppu->bgLayer[1].hScroll & 0x3ff) == (ppu->bgLayer[0].hScroll & 0x3ff) &&
            (ppu->wsSrc[0].vring || stride != 64 || ws_match(ppu, 1, s->ram + 0x10000 + 0xC100, 64, 0x3F00 / 64, &probe) < 0)) {
            /* entries not drawn since the room was set up come from BG2's level map, placed like
             * BG1's (same layout when both scroll together) */
            const uint8_t *lvl2 = s->ram + 0x10000 + 0xC100;
            if (stride == 64 && (ppu->bgLayer[1].vScroll & 0x3ff) == (ppu->bgLayer[0].vScroll & 0x3ff))
                ws_src_vram_lvl(ppu, 1, lvl2, 0x3F00 / 64, g_anchor[0].delta, g_anchor[0].rdelta);
            else
                ws_src_vram(ppu, 1);
            ppu->wsSrc[1].clamp = true; ppu->wsSrc[1].lxMin = ppu->wsSrc[0].lxMin; ppu->wsSrc[1].lxMax = ppu->wsSrc[0].lxMax;
            continue;
        }
        int rows = 0x3F00 / stride;                 /* level height held in WRAM ($C100-$FFFF) */
        const uint8_t *base = s->ram + L[l].bank_off + 0xC100;
        WsAnchor *a = &g_anchor[l];
        int d = ws_match(ppu, l, base, stride, rows, a);
        if (dbg == 5 && l == 0) {   /* debug: whole map rows */
            for (int r = 4; r < 12; r++) {
                fprintf(stderr, "row %d vram:", r);
                for (int c = 0; c < 32; c++) fprintf(stderr, " %04x", ppu->vram[(ppu->bgLayer[0].tilemapAdr + r * 32 + c) & 0x7fff]);
                fprintf(stderr, "\nrow %d ring:", r);
                for (int c = 0; c < stride / 2; c++) { const uint8_t *p = base + r * stride + c * 2; fprintf(stderr, " %04x", p[0] | (p[1] << 8)); }
                fprintf(stderr, "\n");
            }
        }
        if (dbg == 4) {   /* debug: bands of line scroll (last frame) */
            fprintf(stderr, "BG%d bands:", l + 1);
            for (int ln = 1; ln < 225; ln++)
                if (ln == 1 || ppu->wsLineH[l][ln] != ppu->wsLineH[l][ln - 1]) fprintf(stderr, " %d:%d", ln, ppu->wsLineH[l][ln]);
            fprintf(stderr, "\n");
        }
        if (dbg == 3 && d >= 0) {   /* debug: tilemap vs level map, a few rows */
            int hh = ppu->bgLayer[l].hScroll & 0x3ff, vv = ppu->bgLayer[l].vScroll & 0x3ff;
            for (int y = 40; y < 200; y += 32) {
                int lr = (y + vv) >> 4, mc0 = hh >> 4;
                fprintf(stderr, "BG%d y%3d vram:", l + 1, y);
                for (int k = -6; k < 26; k++) fprintf(stderr, " %04x", ppu->vram[(ppu->bgLayer[l].tilemapAdr + (lr & 31) * 32 + ((mc0 + k) & 31)) & 0x7fff]);
                fprintf(stderr, "\nBG%d y%3d ring:", l + 1, y);
                for (int k = -6; k < 26; k++) { const uint8_t *p = base + (lr + a->rdelta) * stride + (((mc0 + k + d) % (stride / 2) + stride / 2) % (stride / 2)) * 2; fprintf(stderr, " %04x", p[0] | (p[1] << 8)); }
                fprintf(stderr, "\n");
            }
        }
        if (d < 0 && g_dbg_best == -1 && a->delta >= 0 && a->stride == stride &&
            ws_verify(ppu, l, base, stride, rows, a->delta, a->rdelta) >= 920) {
            d = a->delta; a->locked = 1;                          /* featureless view: keep last anchor */
        }
        /* The game draws some things into the tilemap only (not the level map), so a scene can
         * briefly fall under the bar for finding an anchor from scratch. The anchor already
         * known just has to go on fitting; a real change of scene or scroll wrap won't. */
        if (d < 0 && g_dbg_best >= 0 && a->delta >= 0 && a->stride == stride &&
            ws_verify(ppu, l, base, stride, rows, a->delta, a->rdelta) >= 650) {
            d = a->delta; a->locked = 1;
        }
        /* ...and when the anchor itself moves (the map position wraps) during such a moment,
         * a clear best fit in the same scene is taken rather than dropping to 4:3 */
        if (d < 0 && g_dbg_best * 100 >= g_dbg_total * 75 && a->delta >= 0 && a->stride == stride) {
            d = g_dbg_best_d; a->delta = d; a->rdelta = g_dbg_best_rd; a->locked = 1;
        }
        /* a frame or two that can't be matched (screen shake, tiles being rewritten) mustn't
         * flash the sides: carry on with the last good position for a moment */
        if (d >= 0) a->miss = 0;
        else if (a->delta >= 0 && a->stride == stride && a->miss < 10 && !ppu->forcedBlank) { a->miss++; d = a->delta; a->locked = 1; }
        if (d < 0 && l == 0 && g_dbg_best == -1 &&                /* playfield too featureless to anchor */
            (ppu->bgLayer[1].hScroll & 0x3ff) == (ppu->bgLayer[0].hScroll & 0x3ff) &&
            (ppu->bgLayer[1].vScroll & 0x3ff) == (ppu->bgLayer[0].vScroll & 0x3ff) &&
            (s->ram[0x9A] | (s->ram[0x9B] << 8)) == stride && ppu->bgLayer[1].bigTiles) {
            /* ...borrow the anchor of the back layer that scrolls with it (same map layout) */
            const uint8_t *base2 = s->ram + 0x10000 + 0xC100;
            WsAnchor tmp = g_anchor[1];
            int d2 = ws_match(ppu, 1, base2, stride, rows, &tmp);
            if (d2 >= 0 && ws_verify(ppu, 0, base, stride, rows, d2, tmp.rdelta) >= 920) {
                d = d2; a->delta = d2; a->rdelta = tmp.rdelta; a->locked = 1;
            }
        }
        if (dbg && (s->frames % 60 == 0 || dbg == 2))
            fprintf(stderr, "ws f%u BG%d stride %d rows %d h %d v %d -> d %d (best %d/%d, distinct %d, odd %d) map %04x\n",
                    s->frames, l + 1, stride, rows, ppu->bgLayer[l].hScroll, ppu->bgLayer[l].vScroll, d,
                    g_dbg_best, g_dbg_total, g_dbg_nseen, g_dbg_odd, ppu->bgLayer[l].tilemapAdr);
        if (d < 0 || stride != a->stride) { a->first_h = -1; a->scrolled = 0; a->stride = stride; }
        /* a back layer whose visible part is one or two tiles repeated (a plain wall) can't be
         * anchored in the level map, and needn't be: its hardware tilemap repeats it already */
        if (d < 0 && l == 1 && g_dbg_best == -1 && g_dbg_nseen <= 2 && ppu->wsSrc[0].valid) {
            /* (entries the game hasn't drawn since the room was set up are the previous room's:
             * those come from BG2's level map, placed like BG1's, when the layout allows) */
            if (stride == 64 && (ppu->bgLayer[1].vScroll & 0x3ff) == (ppu->bgLayer[0].vScroll & 0x3ff) &&
                (ppu->bgLayer[1].hScroll & 0x3ff) == (ppu->bgLayer[0].hScroll & 0x3ff) && g_anchor[0].delta >= 0) {
                ws_src_vram_lvl(ppu, 1, base, rows, g_anchor[0].delta, g_anchor[0].rdelta);
                ppu->wsSrc[1].clamp = ppu->wsSrc[0].clamp;
                ppu->wsSrc[1].lxMin = ppu->wsSrc[0].lxMin; ppu->wsSrc[1].lxMax = ppu->wsSrc[0].lxMax;
            } else
                ws_back_from_vram(ppu);
            continue;
        }
        if (d < 0) continue;
        int h = ppu->bgLayer[l].hScroll & 0x3ff;
        if (a->first_h < 0) a->first_h = h;
        else if (h != a->first_h) a->scrolled = 1;
        /* (no "must have scrolled" requirement: areas are wide from their very first frame) */
        /* The game sometimes uses off-screen parts of the map as scratch for a single frame
         * (never visible in 4:3). Draw the extension from a stable copy: a tile is taken over
         * only once it has had the same value on two consecutive frames. */
        {
            static uint8_t prev[2][0x3F00], stable[2][0x3F00];
            static int last_stride[2] = {-1, -1};
            int bytes = stride * rows; if (bytes > 0x3F00) bytes = 0x3F00;
            if (last_stride[l] != stride) {
                memcpy(prev[l], base, bytes); memcpy(stable[l], base, bytes); last_stride[l] = stride;
            } else {
                for (int i = 0; i < bytes; i += 2) {
                    if (base[i] == prev[l][i] && base[i + 1] == prev[l][i + 1]) { stable[l][i] = base[i]; stable[l][i + 1] = base[i + 1]; }
                    else { prev[l][i] = base[i]; prev[l][i + 1] = base[i + 1]; }
                }
            }
            rows = bytes / stride;
            /* Doors and similar objects are written into the tilemap only, never into the level
             * map the extension is drawn from. Remember what the tilemap shows wherever it
             * differs from the level map, for as long as that spot of the level map itself
             * stays the same, so such things stay drawn after they scroll into the wide area.
             * Only tiles well inside the 4:3 view count, and only once they have stayed the same
             * for a few frames: the columns at its edges are briefly stale while the game streams
             * them in (most of all when the screen shakes), and taking those would leave a stale
             * or blank column in the wide area. */
            {
                enum { OV_FRAMES = 4 };
                static uint8_t ov_on[2][0x1F80]; static uint16_t ov_tile[2][0x1F80], ov_base[2][0x1F80];
                static int ov_stride[2];
                int ww = stride / 2, cells = bytes / 2;
                if (ov_stride[l] != stride) { memset(ov_on[l], 0, sizeof ov_on[l]); ov_stride[l] = stride; }
                int hs = ppu->bgLayer[l].hScroll & 0x3ff, vs = ppu->bgLayer[l].vScroll & 0x3ff;
                uint16_t mp = ppu->bgLayer[l].tilemapAdr;
                int mcj[14], pgj[14];
                ws_rows(ppu, l, hs >> 4, mcj, pgj);
                for (int j = 0; j < 14; j++) {
                    int lr = (1 + 16 * j + vs) >> 4, r = lr + a->rdelta;
                    if (r < 0 || r >= rows) continue;
                    if (mcj[j] != (hs >> 4)) continue;   /* a band scrolled elsewhere: its tiles aren't at these columns */
                    for (int k = 2; k < 15; k++) {
                        int mc = (hs >> 4) + k, idx = r * ww + (mc + d) % ww;
                        uint16_t vt = ppu->vram[(mp + (lr & 31) * 32 + (mc & 31)) & 0x7fff];
                        uint16_t bt = (uint16_t)(base[idx * 2] | (base[idx * 2 + 1] << 8));
                        if (vt == bt) ov_on[l][idx] = 0;
                        else if (ov_on[l][idx] && ov_tile[l][idx] == vt && ov_base[l][idx] == bt) { if (ov_on[l][idx] < 255) ov_on[l][idx]++; }
                        else { ov_on[l][idx] = 1; ov_tile[l][idx] = vt; ov_base[l][idx] = bt; }
                    }
                }
                for (int i = 0; i < cells; i++) {
                    if (ov_on[l][i] < OV_FRAMES) continue;
                    if ((uint16_t)(base[i * 2] | (base[i * 2 + 1] << 8)) != ov_base[l][i]) { ov_on[l][i] = 0; continue; }
                    stable[l][i * 2] = (uint8_t)ov_tile[l][i]; stable[l][i * 2 + 1] = (uint8_t)(ov_tile[l][i] >> 8);
                }
            }
            ppu->wsSrc[l].valid = true; ppu->wsSrc[l].base = stable[l]; ppu->wsSrc[l].stride = stride;
        }
        ppu->wsSrc[l].width = stride / 2; ppu->wsSrc[l].rows = rows; ppu->wsSrc[l].delta = d; ppu->wsSrc[l].rdelta = a->rdelta;
        ppu->wsSrc[l].refCol = (ppu->bgLayer[l].hScroll & 0x3ff) >> 4;
        ppu->wsSrc[l].clamp = false; ppu->wsSrc[l].vring = false;
        if (l == 1 && narrow && !(ppu->wsSrc[0].valid && ppu->wsSrc[0].clamp)) { ppu->wsSrc[l].valid = false; continue; }
        /* A back layer that scrolls together with the playfield shares its bounds: beyond them
         * the game only keeps out-of-bounds filler (never visible in 4:3). Parallax layers that
         * scroll on their own repeat by design and stay unclamped. */
        if (l == 1 && ppu->wsSrc[0].valid && ppu->wsSrc[0].clamp &&
            (ppu->bgLayer[1].hScroll & 0x3ff) == (ppu->bgLayer[0].hScroll & 0x3ff)) {
            ppu->wsSrc[1].clamp = true;
            ppu->wsSrc[1].lxMin = ppu->wsSrc[0].lxMin;
            ppu->wsSrc[1].lxMax = ppu->wsSrc[0].lxMax;
        }
        if (l == 0) {
            /* The decoded map is the whole area, so nothing exists left of its first column or
             * right of its last. $62 holds the camera X (+0x180) in full; reconcile it with the
             * 10-bit scroll register actually used for this frame and clamp to the map. */
            int cam = (s->ram[0x62] | (s->ram[0x63] << 8)) - 0x180;
            /* In a scene drawn in bands that scroll separately, only the band that moves with the
             * camera says where the level is: find it (the line whose scroll last frame was
             * nearest the camera; line 1 when the whole screen scrolls together) and estimate its
             * scroll for this frame from line 1's. The edges are drawn from that line's scroll. */
            /* The line is taken from the middle of that band (its first line sits on the border
             * with the band above, which moves as the view scrolls up or down), and in a banded
             * scene the band's scroll is last frame's plus the camera's move since then: line 1
             * may have changed band since last frame, so this frame's line 1 says nothing. */
            int cam_line = 1, hc = h;
            {
                static int prev_cam = -100000;
                int cm = ((cam % 1024) + 1024) % 1024, bestd = 1 << 30, banded = 0;
                for (int ln = 1; ln <= 224; ln++) {
                    int dd = abs(((ppu->wsLineH[0][ln] - cm + 512) & 0x3ff) - 512);
                    if (dd < bestd) bestd = dd;
                    if (ppu->wsLineH[0][ln] != ppu->wsLineH[0][1]) banded = 1;
                }
                int start = -1, best_run = 0;              /* longest run of such lines, same scroll */
                for (int ln = 1; ln <= 225; ln++) {
                    int in = ln <= 224 && abs(((ppu->wsLineH[0][ln] - cm + 512) & 0x3ff) - 512) == bestd;
                    if (start >= 0 && (!in || ppu->wsLineH[0][ln] != ppu->wsLineH[0][start])) {
                        if (ln - start > best_run) { best_run = ln - start; cam_line = start + (ln - start) / 2; }
                        start = -1;
                    }
                    if (in && start < 0) start = ln;
                }
                if (banded && prev_cam > -100000 && abs(cam - prev_cam) < 64)
                    hc = (ppu->wsLineH[0][cam_line] + (cam - prev_cam)) & 0x3ff;
                else
                    hc = (h + ((ppu->wsLineH[0][cam_line] - ppu->wsLineH[0][1] + 512) & 0x3ff) - 512) & 0x3ff;
                prev_cam = cam;
            }
            int k = (cam - hc + 512) >> 10;
            if (cam - hc + 512 < 0) k = -((hc - cam + 511) >> 10);
            int cam_r = hc + 1024 * k;
            int w_px = (stride / 2) * 16;
            if (cam_r - cam < 64 && cam - cam_r < 64 && cam > -64 && cam < w_px) {
                ppu->wsSrc[l].clamp = true;
                ppu->wsSrc[l].lxMin = hc - cam_r;
                ppu->wsSrc[l].lxMax = hc - cam_r + w_px;
                ppu->wsRefH = hc;
                ppu->wsCamLine = cam_line;
            }
            if (dbg == 2)
                fprintf(stderr, "   cam %d cam_r %d h %d clamp %d lx [%d,%d) line %d load %d\n", cam, cam_r, hc, ppu->wsSrc[l].clamp, ppu->wsSrc[l].lxMin, ppu->wsSrc[l].lxMax, cam_line, ppu->wsEpoch);
            if (narrow && !ppu->wsSrc[l].clamp) { ppu->wsSrc[l].valid = false; continue; }
            if (narrow && ws_tilemap_complete(ppu, 0)) ws_src_vram(ppu, 0);
        }
    }
    /* The final boss's arena (stage 195) is a backdrop that repeats by design and has no level
     * map, so its side columns come straight from the hardware tilemaps. */
    if (!ppu->wsSrc[0].valid && (s->ram[0x480] | (s->ram[0x481] << 8)) == 195 && ppu->mode == 1 && !ppu->forcedBlank) {
        static uint8_t arena[2][0x800];
        for (int l = 0; l < 2; l++) {
            if (!ppu->bgLayer[l].bigTiles || ppu->bgLayer[l].tilemapWider) continue;
            for (int i = 0; i < 0x400; i++) {
                uint16_t w = ppu->vram[(ppu->bgLayer[l].tilemapAdr + i) & 0x7fff];
                arena[l][i * 2] = (uint8_t)w; arena[l][i * 2 + 1] = (uint8_t)(w >> 8);
            }
            ppu->wsSrc[l].valid = true; ppu->wsSrc[l].base = arena[l]; ppu->wsSrc[l].stride = 64;
            ppu->wsSrc[l].width = 32; ppu->wsSrc[l].rows = 32; ppu->wsSrc[l].delta = 0; ppu->wsSrc[l].rdelta = 0;
            ppu->wsSrc[l].vring = 1; ppu->wsSrc[l].clamp = false;
        }
    }
    /* without a verified playfield source there is nothing correct to show at the sides */
    ppu->wsBlank = !ppu->wsSrc[0].valid;
    /* diagnostics: a gameplay screen (status bar on BG3 at its -4 offset) that still fell back
     * to 4:3 means an area layout the widescreen source doesn't understand yet */
    g_ws_failed = ppu->wsExtra > 0 && ppu->wsBlank && ppu->mode == 1 &&
                  (ppu->bgLayer[2].hScroll & 0x3ff) == 0x3FC && ppu->layer[2].mainScreenEnabled && !ppu->forcedBlank;
    if (g_ws_failed) {
        snprintf(g_ws_fail_info, sizeof g_ws_fail_info, "stride %d/%d h %d v %d big %d wider %d higher %d best %d/%d distinct %d odd %d",
                 s->ram[0x84] | (s->ram[0x85] << 8), s->ram[0x9A] | (s->ram[0x9B] << 8), ppu->bgLayer[0].hScroll,
                 ppu->bgLayer[0].vScroll, ppu->bgLayer[0].bigTiles, ppu->bgLayer[0].tilemapWider,
                 ppu->bgLayer[0].tilemapHigher, g_dbg_best, g_dbg_total, g_dbg_nseen, g_dbg_odd);
    }
    rt_ws_extra = ppu->wsBlank ? 0 : ppu->wsExtra;     /* applies to the sprites the game draws next */
    if (g_wsdbg.no_sprite_ws) rt_ws_extra = 0;
    ppu->wsDebugLayers = g_wsdbg.layers;
    ppu->wsDebugHide = (uint8_t)g_wsdbg.hide;
}

void hw_set_sprite_limit(int on) { g->ppu->noSpriteLimit = !on; }
uint32_t hw_sprite_overflow_lines(int *max_sprites, int *max_slivers) {
    *max_sprites = g->ppu->spriteMaxOnLine; *max_slivers = g->ppu->sliverMaxOnLine;
    return g->ppu->spriteOverflowLines;
}

void hw_set_widescreen(int extra, int profile) {
    g->ppu->wsExtra = extra;
    if (!extra || profile != 1) rt_ws_extra = 0;
    if (profile == 1) {             /* Magical Pop'n level-map source */
        g->ppu->wsBgMask = 0x0;     /* layers extend only from a verified level-map source */
        g->ppu->wsFrameHook = popn_ws_hook;
        g->ppu->wsUser = g;
    } else {                        /* generic: every layer extends from VRAM */
        g->ppu->wsBgMask = 0xF;
        g->ppu->wsFrameHook = NULL;
        for (int l = 0; l < 4; l++) g->ppu->wsSrc[l].valid = false;
    }
}
int hw_get_pixels_wide(uint8_t *px) { return ppu_putPixelsWide(g->ppu, px); }
const uint16_t *hw_line_hscroll(int layer) { return g->ppu->wsLineH[layer & 1]; }
int hw_bg_vscroll(int layer) { return g->ppu->bgLayer[layer & 3].vScroll & 0x3ff; }
const uint16_t *hw_line_vscroll(int layer) { return g->ppu->wsLineV[layer & 1]; }
const uint32_t *hw_ws_stats(void) { g->ppu->wsStats = true; return g->ppu->wsStatObj; }
void hw_get_samples(int16_t *s, int n) { snes_setSamples(g, s, n); }

/* New native-rate (~32 kHz) stereo samples produced since the last call. */
int hw_take_samples(int16_t *out, int max_frames) {
    static uint16_t last;
    Dsp *d = g->apu->dsp;
    if (!g_samples_primed) { last = d->sampleOffset; g_samples_primed = 1; return 0; }
    int n = (uint16_t)(d->sampleOffset - last);
    if (n > 0x3F0) { last = (uint16_t)(d->sampleOffset - 0x3F0); n = 0x3F0; }   /* overrun: keep newest */
    if (n > max_frames) n = max_frames;
    for (int i = 0; i < n; i++) {
        int k = (last + i) & 0x3ff;
        out[i * 2] = d->sampleBuffer[k * 2];
        out[i * 2 + 1] = d->sampleBuffer[k * 2 + 1];
    }
    last = (uint16_t)(last + n);
    return n;
}
void hw_set_buttons(uint16_t mask) { g->input1->currentState = mask; }
const uint8_t *hw_wram(void) { return g->ram; }

uint16_t hw_bg_hscroll(int layer) { return g->ppu->bgLayer[layer & 3].hScroll; }

/* ---- motion smoothing: in-between frames for 120/240 Hz displays ----
 * The game runs at 60 fps. For a display refreshing 2 or 4 times per game frame, frames
 * between two real ones are drawn by the PPU itself: every scanline is re-rendered from that
 * line's recorded registers with the background scroll and each sprite moved part of the way
 * from the previous frame's position to the current one. Pixel art stays crisp (no blending).
 * Jumps (camera cuts, sprites reusing a slot elsewhere) aren't interpolated. */
#define RG_START offsetof(Ppu, objPriority)
#define RG_SIZE  (offsetof(Ppu, pixelBuffer) - offsetof(Ppu, objPriority))
typedef struct {
    int valid, lines;
    uint8_t regs[240][RG_SIZE];
    uint8_t has[240];
    uint16_t oam[0x100];
    uint8_t high[0x20];
    uint32_t gfx[128];            /* fingerprint of each sprite's pixel data in VRAM */
    /* the palette each line was drawn with: games change it during the frame (fades, colour
     * gradients), and the end-of-frame palette alone would recolour the earlier lines */
    int mixed_blank;              /* the screen was switched off for part of the frame */
    int cg_n;                     /* distinct palettes this frame */
    uint8_t cg_of_line[240];
    uint16_t cg[240][0x100];
} IpFrame;
static IpFrame *g_ip[2];          /* [0] previous frame, [1] current frame */
static int g_ip_on;               /* recording enabled */

static void ip_line_hook(Ppu *ppu, int line, void *user) {
    (void)user;
    if (line < 1 || line > 239) return;
    IpFrame *f = g_ip[1];
    if (line == 1) { f->lines = 0; f->valid = 0; f->cg_n = 0; memset(f->has, 0, sizeof f->has); }
    memcpy(f->regs[line], (uint8_t *)ppu + RG_START, RG_SIZE);
    if (f->cg_n == 0 || memcmp(f->cg[f->cg_n - 1], ppu->cgram, sizeof ppu->cgram) != 0) {
        if (f->cg_n < 240) memcpy(f->cg[f->cg_n++], ppu->cgram, sizeof ppu->cgram);
        else memcpy(f->cg[239], ppu->cgram, sizeof ppu->cgram);
    }
    f->cg_of_line[line] = (uint8_t)(f->cg_n - 1);
    f->has[line] = 1;
    if (line > f->lines) f->lines = line;
}

static void snap_alloc(void);
void hw_interp_enable(int on) {
    if (on && !g_ip[0]) {
        g_ip[0] = calloc(1, sizeof(IpFrame)); g_ip[1] = calloc(1, sizeof(IpFrame));
        if (!g_ip[0] || !g_ip[1]) { free(g_ip[0]); free(g_ip[1]); g_ip[0] = g_ip[1] = NULL; return; }
    }
    if (on) snap_alloc();
    g_ip_on = on && g_ip[0];
    g->ppu->lineHook = g_ip_on ? ip_line_hook : NULL;
    if (g_ip[0]) { g_ip[0]->valid = g_ip[1]->valid = 0; }
    /* recording goes into g_ip[1]; the hook swaps them at each vblank */
}

static void ip_invalidate(void) { if (g_ip[0]) g_ip[0]->valid = g_ip[1]->valid = 0; }

/* Fingerprint of the pixel data a sprite shows. Games often animate a character by copying
 * new artwork into the same tiles, so the tile number alone doesn't say whether it changed. */
static uint32_t ip_sprite_gfx(const Ppu *p, int i) {
    static const int sizes[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}};
    uint16_t a = p->oam[i * 2 + 1];
    int big = (p->highOam[i >> 2] >> (((i * 2) & 7) + 1)) & 1;
    int size = sizes[p->objSize & 7][big];
    uint16_t base = (a & 0x100) ? p->objTileAdr2 : p->objTileAdr1;
    uint8_t tile = a & 0xff;
    uint32_t h = 2166136261u;
    for (int r = 0; r < size / 8; r++)
        for (int c = 0; c < size / 8; c++) {
            uint8_t t = (uint8_t)((((tile >> 4) + r) << 4) | (((tile & 0xf) + c) & 0xf));
            for (int k = 0; k < 16; k++) { h ^= p->vram[(base + t * 16 + k) & 0x7fff]; h *= 16777619u; }
        }
    return h;
}

/* called at vblank: the frame just drawn becomes "current", the one before it "previous" */
static void ip_end_frame(void) {
    if (!g_ip_on) return;
    IpFrame *f = g_ip[1];
    memcpy(f->oam, g->ppu->oam, sizeof f->oam);
    memcpy(f->high, g->ppu->highOam, sizeof f->high);
    for (int i = 0; i < 128; i++) {
        int y = g->ppu->oam[i * 2] >> 8;
        f->gfx[i] = (y >= 224 && y < 240) ? 0 : ip_sprite_gfx(g->ppu, i);
    }
    /* A game switches the screen off partway down to upload graphics (scene changes): the
     * lines drawn before that used graphics that are gone by the end of the frame, so such a
     * frame can't be redrawn; the real frame is shown for it. */
    {
        int on = 0, off = 0;
        for (int l = 1; l <= f->lines; l++) {
            if (!f->has[l]) continue;
            if (((const Ppu *)((const uint8_t *)f->regs[l] - RG_START))->forcedBlank) off = 1; else on = 1;
        }
        f->mixed_blank = on && off;
    }
    f->valid = f->lines >= 200;
    IpFrame *t = g_ip[0]; g_ip[0] = g_ip[1]; g_ip[1] = t;   /* [0] = just completed, [1] = the one before */
}

static void ip_frame_hook(Ppu *ppu, void *user) { ppu->wsBlank = *(const bool *)user; }

int hw_interp_ready(void) { return g_ip_on && g_ip[0] && g_ip[0]->valid && g_ip[1] && g_ip[1]->valid; }

#define IP_SPR_R 16           /* a sprite moves at most this far per frame on screen */

static int ip_lerp(int prev, int cur, int mod, int maxd, float t) {
    int d = ((cur - prev + mod / 2) & (mod - 1)) - mod / 2;
    if (d > maxd || d < -maxd) return cur;
    return (cur - (int)lroundf(d * (1.0f - t))) & (mod - 1);
}

/* How far each sprite of the current frame moved since the previous one.
 *
 * The game reshuffles OAM slots from frame to frame, so a piece is paired with sprites of the
 * previous frame that show the same tile with the same attributes and size, nearby. That alone
 * is fooled when an object changes pose: the new pose is built from mostly different tiles, and
 * the few tiles both poses share sit at other offsets, so they "move" 8 or 16 px and drag the
 * whole object out of place for a picture. So the motion is decided per object (sprites that
 * touch): the motion most of its pieces agree on, which every piece of it then follows. An
 * object whose pieces don't agree is changing shape and is drawn as it is now. */
static int ip_debug(void) {   /* POPN_IP_DEBUG: print each object's matching to stderr */
    static int v = -1;
    if (v < 0) v = getenv("POPN_IP_DEBUG") != NULL;
    return v;
}
enum { IP_MAXC = 6 };
typedef struct { int nc[128], cx[128][IP_MAXC], cy[128][IP_MAXC], cg[128][IP_MAXC]; } IpPairs;
/* The motion of one group of sprites (mem[0..n)): the motion supported by the most pieces (a piece
 * supports it if one of its pairings is within 1 px; ties go to the smaller motion), provided
 * enough of the group agrees (all of it if `all`). Returns 1 and the motion, or 0: no motion. */
static int ip_group_motion(const IpPairs *q, const int *mem, int n, int all, int *obx, int *oby) {
    int best = 0, bx = 0, by = 0;
    for (int a = 0; a < n; a++)
        for (int c = 0; c < q->nc[mem[a]]; c++) {
            int mx = q->cx[mem[a]][c], my = q->cy[mem[a]][c], sup = 0;
            for (int b = 0; b < n; b++)
                for (int e = 0; e < q->nc[mem[b]]; e++)
                    if (abs(q->cx[mem[b]][e] - mx) <= 1 && abs(q->cy[mem[b]][e] - my) <= 1) { sup++; break; }
            if (sup > best || (sup == best && abs(mx) + abs(my) < abs(bx) + abs(by))) { best = sup; bx = mx; by = my; }
        }
    *obx = bx; *oby = by;
    if (!best) return 0;
    /* most of the group must agree; for a big step nearly all of it, since tiles sit on an 8 px
     * grid and a pose change can line a few of them up at exactly such a step */
    int big = abs(bx) >= 6 || abs(by) >= 6;
    if (all ? best < n : big ? best * 5 < n * 4 : best * 5 < n * 3) return 0;
    /* New artwork in the same tiles together with a jump of more than a few pixels is a pose
     * change, not motion: the game moves the sprite to where the new pose's artwork sits
     * (crouch-attacking drops it 8 px; real motion with an animation step, like a fall, stays
     * under 6 px a frame). Sliding the new artwork in from the old spot would draw the
     * character out of place, so it is shown where it is now. */
    if (big) {
        int same = 0, changed = 0;
        for (int a = 0; a < n; a++)
            for (int e = 0; e < q->nc[mem[a]]; e++)
                if (abs(q->cx[mem[a]][e] - bx) <= 1 && abs(q->cy[mem[a]][e] - by) <= 1) { if (q->cg[mem[a]][e]) same++; else changed++; break; }
        if (changed > same) return 0;
    }
    return 1;
}

static void ip_sprite_motion(int xs[2][128], int ys[2][128], int sz[2][128], uint16_t key[2][128],
                             const uint32_t *gfx_prev, const uint32_t *gfx_cur, int obj_size,
                             const int *wdx, const int *wdy, int *mdx, int *mdy, int *have) {
    static const int SIZES[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}};
    IpPairs pairs, *q = &pairs;
    int on[128];
    for (int i = 0; i < 128; i++) {
        mdx[i] = mdy[i] = have[i] = 0; q->nc[i] = 0;
        on[i] = !(ys[1][i] >= 224 && ys[1][i] < 240);               /* not parked off screen */
        if (!on[i]) continue;
        for (int j = 0; j < 128 && q->nc[i] < IP_MAXC; j++) {
            if (key[0][j] != key[1][i] || sz[0][j] != sz[1][i] || (ys[0][j] >= 224 && ys[0][j] < 240)) continue;
            int ddx = ((xs[1][i] - xs[0][j] + 256) & 511) - 256, ddy = ((ys[1][i] - ys[0][j] + 128) & 255) - 128;
            if (ddx < -IP_SPR_R || ddx > IP_SPR_R || ddy < -IP_SPR_R || ddy > IP_SPR_R) continue;
            int with_bg = abs(ddx - wdx[i]) <= 1 && abs(ddy - wdy[i]) <= 1;
            q->cx[i][q->nc[i]] = ddx; q->cy[i][q->nc[i]] = ddy; q->cg[i][q->nc[i]] = with_bg || gfx_prev[j] == gfx_cur[i]; q->nc[i]++;
        }
        /* A sprite that animates by showing another tile every frame (a spinning star) has no
         * pairing by tile. If the background scrolled and a sprite of the same size and palette
         * was exactly where this one would have been had it stayed put in the level, it is that
         * sprite: it moves with the background, whatever it shows. */
        if ((wdx[i] || wdy[i]) && q->nc[i] < IP_MAXC) {
            int found = 0;
            for (int c = 0; c < q->nc[i]; c++) if (abs(q->cx[i][c] - wdx[i]) <= 1 && abs(q->cy[i][c] - wdy[i]) <= 1) found = 1;
            for (int j = 0; j < 128 && !found; j++) {
                if (sz[0][j] != sz[1][i] || ((key[0][j] ^ key[1][i]) & 0x0E00) || (ys[0][j] >= 224 && ys[0][j] < 240)) continue;
                int ddx = ((xs[1][i] - xs[0][j] + 256) & 511) - 256, ddy = ((ys[1][i] - ys[0][j] + 128) & 255) - 128;
                if (abs(ddx - wdx[i]) > 1 || abs(ddy - wdy[i]) > 1) continue;
                q->cx[i][q->nc[i]] = ddx; q->cy[i][q->nc[i]] = ddy; q->cg[i][q->nc[i]] = 1; q->nc[i]++;
                found = 1;
            }
        }
    }
    /* objects: connected groups of nearby on-screen sprites (union-find) */
    int par[128];
    for (int i = 0; i < 128; i++) par[i] = i;
    for (int i = 0; i < 128; i++) {
        if (!on[i]) continue;
        for (int j = i + 1; j < 128; j++) {
            if (!on[j]) continue;
            int ox = ((xs[1][i] - xs[1][j] + 256) & 511) - 256, oy = ((ys[1][i] - ys[1][j] + 128) & 255) - 128;
            /* pieces whose boxes touch belong together, whatever their size: big sprites stacked
             * into one tall object (a flame pillar, a boss) must move as one or a seam opens */
            int si = SIZES[obj_size & 7][sz[1][i]], sj = SIZES[obj_size & 7][sz[1][j]];
            if (ox <= -(si + 2) || ox >= sj + 2 || oy <= -(si + 2) || oy >= sj + 2) continue;
            int a = i, b = j;
            while (par[a] != a) a = par[a] = par[par[a]];
            while (par[b] != b) b = par[b] = par[par[b]];
            if (a != b) par[a] = b;
        }
    }
    for (int i = 0; i < 128; i++) { int a = i; while (par[a] != a) a = par[a]; par[i] = a; }
    for (int root = 0; root < 128; root++) {
        int mem[128], n = 0;
        for (int i = 0; i < 128; i++) if (on[i] && par[i] == root) mem[n++] = i;
        if (!n) continue;
        int bx, by, moves = ip_group_motion(q, mem, n, 0, &bx, &by);
        if (ip_debug()) {
            fprintf(stderr, "obj n=%d motion %d,%d %s:", n, bx, by, moves ? "moves" : "stays");
            for (int a = 0; a < n; a++) {
                int i = mem[a];
                fprintf(stderr, " [%d @%d,%d k%04x s%d", i, xs[1][i], ys[1][i], key[1][i], sz[1][i]);
                for (int c = 0; c < q->nc[i]; c++) fprintf(stderr, " %+d%+d", q->cx[i][c], q->cy[i][c]);
                fprintf(stderr, "]");
            }
            fprintf(stderr, "\n");
        }
        if (moves) for (int a = 0; a < n; a++) { mdx[mem[a]] = bx; mdy[mem[a]] = by; have[mem[a]] = 1; }
        /* Two different things that touch (the heroine walking past a door) land in one group, and
         * the group's motion is then wrong for one of them, or there is none: the door would stand
         * still for a picture while the background behind it moves. So the pieces of each palette
         * are looked at on their own as well: two or more that all agree on a motion follow that. */
        for (int pal = 0; pal < 8 && n > 1; pal++) {
            int sub[128], m = 0, sx, sy;
            for (int a = 0; a < n; a++) if (((key[1][mem[a]] >> 9) & 7) == pal) sub[m++] = mem[a];
            if (m < 2 || m == n || !ip_group_motion(q, sub, m, 1, &sx, &sy)) continue;
            if (ip_debug()) fprintf(stderr, "  palette %d: %d pieces move %d,%d\n", pal, m, sx, sy);
            for (int a = 0; a < m; a++) { mdx[sub[a]] = sx; mdy[sub[a]] = sy; have[sub[a]] = 1; }
        }
    }
}

/* Draw the picture at fraction t (0..1) between `prev` and `cur` with PPU `p` (whose VRAM,
 * palette and widescreen sources are those of `cur`) into `out` (BGRX); returns its width.
 * p's registers, sprites and hooks are restored afterwards. */
static int ip_render(Ppu *p, const IpFrame *cur, const IpFrame *prev, float t, uint8_t *out) {
    uint8_t regs[RG_SIZE]; uint16_t oam[0x100]; uint8_t high[0x20]; uint16_t cgram[0x100];
    memcpy(regs, (uint8_t *)p + RG_START, RG_SIZE);
    memcpy(cgram, p->cgram, sizeof cgram);
    memcpy(oam, p->oam, sizeof oam); memcpy(high, p->highOam, sizeof high);
    void (*line_hook)(Ppu *, int, void *) = p->lineHook;
    void (*frame_hook)(Ppu *, void *) = p->wsFrameHook;
    void *user = p->wsUser;
    bool blank = p->wsBlank;
    p->lineHook = NULL; p->wsNoRecord = true;
    p->wsFrameHook = ip_frame_hook;   /* keeps this frame's widescreen sources and blanking */
    p->wsUser = &blank;
    /* Sprites: each moves part of the way along its object's motion (ip_sprite_motion);
     * anything without one stays where it is now. */
    {
        int xs[2][128], ys[2][128], sz[2][128]; uint16_t key[2][128];
        const IpFrame *fr[2] = {prev, cur};
        for (int f = 0; f < 2; f++)
            for (int i = 0; i < 128; i++) {
                int hb = (i * 2) & 7, byte = i >> 2;
                uint16_t pos = fr[f]->oam[i * 2];
                xs[f][i] = (pos & 0xff) | (((fr[f]->high[byte] >> hb) & 1) << 8);
                ys[f][i] = pos >> 8;
                sz[f][i] = (fr[f]->high[byte] >> (hb + 1)) & 1;
                key[f][i] = fr[f]->oam[i * 2 + 1];
            }
        /* how far a sprite standing still in the level moved on screen: the opposite of the
         * playfield's scroll since the previous frame, on the sprite's own lines */
        int wdx[128], wdy[128];
        for (int i = 0; i < 128; i++) {
            int line = ys[1][i] + 8; if (line < 1) line = 1; if (line > cur->lines) line = cur->lines;
            wdx[i] = wdy[i] = 0;
            if (line < 1 || line > 239 || !cur->has[line] || !prev->has[line]) continue;
            const Ppu *c1 = (const Ppu *)((const uint8_t *)cur->regs[line] - RG_START);
            const Ppu *c0 = (const Ppu *)((const uint8_t *)prev->regs[line] - RG_START);
            int dh = ((c1->bgLayer[0].hScroll - c0->bgLayer[0].hScroll + 512) & 0x3ff) - 512;
            int dv = ((c1->bgLayer[0].vScroll - c0->bgLayer[0].vScroll + 512) & 0x3ff) - 512;
            if (abs(dh) <= IP_SPR_R && abs(dv) <= IP_SPR_R) { wdx[i] = -dh; wdy[i] = -dv; }
        }
        int mdx_of[128], mdy_of[128], have_of[128];
        ip_sprite_motion(xs, ys, sz, key, prev->gfx, cur->gfx, p->objSize, wdx, wdy, mdx_of, mdy_of, have_of);
        for (int i = 0; i < 128; i++) {
            int mdx = mdx_of[i], mdy = mdy_of[i], have = have_of[i];
            int x = xs[1][i], y = ys[1][i];
            if (have) {
                x = (x - (int)lroundf(mdx * (1.0f - t))) & 511;
                y = (y - (int)lroundf(mdy * (1.0f - t))) & 255;
            }
            int hb = (i * 2) & 7, byte = i >> 2;
            p->oam[i * 2] = (uint16_t)((x & 0xff) | (y << 8));
            p->highOam[byte] = (uint8_t)((p->highOam[byte] & ~(1 << hb)) | (((x >> 8) & 1) << hb));
        }
    }
    int last = cur->lines;
    for (int line = 1; line <= last; line++) {
        if (!cur->has[line]) continue;
        memcpy((uint8_t *)p + RG_START, cur->regs[line], RG_SIZE);
        if (cur->cg_n > 0) memcpy(p->cgram, cur->cg[cur->cg_of_line[line]], sizeof p->cgram);
        if (prev->has[line]) {
            const Ppu *pr = (const Ppu *)((const uint8_t *)prev->regs[line] - RG_START);
            if (pr->mode == p->mode && !p->forcedBlank && !pr->forcedBlank)
                for (int l = 0; l < 4; l++) {
                    if (p->mode == 7 && l == 0) continue;
                    p->bgLayer[l].hScroll = (uint16_t)ip_lerp(pr->bgLayer[l].hScroll & 0x3ff, p->bgLayer[l].hScroll & 0x3ff, 1024, 64, t);
                    p->bgLayer[l].vScroll = (uint16_t)ip_lerp(pr->bgLayer[l].vScroll & 0x3ff, p->bgLayer[l].vScroll & 0x3ff, 1024, 64, t);
                }
        }
        ppu_runLine(p, line);
    }
    int w = ppu_putPixelsWide(p, out);
    memcpy((uint8_t *)p + RG_START, regs, RG_SIZE);
    memcpy(p->cgram, cgram, sizeof cgram);
    memcpy(p->oam, oam, sizeof oam); memcpy(p->highOam, high, sizeof high);
    p->lineHook = line_hook; p->wsFrameHook = frame_hook; p->wsUser = user; p->wsBlank = blank; p->wsNoRecord = false;
    return w;
}

/* debug: what was recorded for the current frame's lines a..b */
void hw_interp_dump(int a, int b) {
    const IpFrame *f = g_ip[0];
    fprintf(stderr, "lines %d valid %d cg_n %d | live: evenFrame %d frameOverscan %d forcedBlank %d bright %d\n",
            f->lines, f->valid, f->cg_n, g->ppu->evenFrame, g->ppu->frameOverscan, g->ppu->forcedBlank, g->ppu->brightness);
    for (int l = a; l <= b && l < 240; l++) {
        const Ppu *r = (const Ppu *)((const uint8_t *)f->regs[l] - RG_START);
        fprintf(stderr, "  line %3d has %d fb %d br %2d mode %d even %d ovs %d cg %d bd %04x\n", l, f->has[l], r->forcedBlank,
                r->brightness, r->mode, r->evenFrame, r->frameOverscan, f->cg_of_line[l], f->cg[f->cg_of_line[l]][0]);
    }
}

/* On the live PPU, right after a frame (used by the headless test). */
int hw_render_interp(float t, uint8_t *out) {
    if (g_ip[0]->mixed_blank) return ppu_putPixelsWide(g->ppu, out);   /* as hw_snapshot_render */
    return ip_render(g->ppu, g_ip[0], g_ip[1], t, out);
}

/* ---- snapshots for the display thread: triple-buffered so neither side ever waits ---- */
typedef struct {
    Ppu ppu;                          /* private copy: VRAM, palette, line buffers, registers */
    IpFrame cur, prev;
    uint8_t src[4][0x3F00];           /* widescreen tile sources as they were */
    uint8_t real[HW_WIDE_MAX_W * 224 * 4];
    int real_w;
    double t0;                        /* when this frame was emulated (ms) */
    int valid;
} IpSnap;
static IpSnap *g_snap[4];
static int g_snap_back = 0, g_snap_latest = 1, g_snap_front = 2, g_snap_older = 3;
static volatile int g_snap_fresh;
static CRITICAL_SECTION g_snap_cs;

/* created on the main thread before the display thread starts (hw_interp_enable) */
static volatile int g_snap_ready;
static void snap_alloc(void) {
    if (g_snap_ready) return;
    for (int i = 0; i < 4; i++) if (!g_snap[i] && !(g_snap[i] = calloc(1, sizeof(IpSnap)))) return;
    InitializeCriticalSection(&g_snap_cs);
    g_snap_ready = 1;
}

int hw_snapshot_publish(double t0_ms) {
    if (!g_snap_ready) return 0;
    IpSnap *s = g_snap[g_snap_back];
    s->valid = hw_interp_ready();
    if (s->valid) {
        memcpy(&s->ppu, g->ppu, sizeof(Ppu));
        s->ppu.lineHook = NULL; s->ppu.wsFrameHook = NULL; s->ppu.snes = NULL;
        for (int l = 0; l < 4; l++)
            if (s->ppu.wsSrc[l].valid && s->ppu.wsSrc[l].base) {
                int bytes = s->ppu.wsSrc[l].stride * s->ppu.wsSrc[l].rows;
                if (bytes > (int)sizeof s->src[l]) bytes = sizeof s->src[l];
                memcpy(s->src[l], s->ppu.wsSrc[l].base, bytes);
                s->ppu.wsSrc[l].base = s->src[l];
            }
        memcpy(&s->cur, g_ip[0], sizeof(IpFrame));
        memcpy(&s->prev, g_ip[1], sizeof(IpFrame));
        s->real_w = ppu_putPixelsWide(g->ppu, s->real);
        s->t0 = t0_ms;
    }
    EnterCriticalSection(&g_snap_cs);
    int t = g_snap_back; g_snap_back = g_snap_latest; g_snap_latest = t; g_snap_fresh = 1;
    LeaveCriticalSection(&g_snap_cs);
    return s->valid;
}

/* Display thread: the picture for the moment `when_ms`. Snapshot s covers [s->t0, s->t0 + frame]
 * (its previous frame at t0, its own one frame later), so pictures run one game frame behind
 * the emulation. The snapshot before the newest is kept too: the caller asks for moments a
 * little behind the newest frame, and when one falls before it, the older snapshot has it,
 * so motion never stalls on a frame that arrived late and then jumps. Returns width, 0 = nothing. */
volatile long hw_snap_clamps;
int hw_snapshot_render(double when_ms, uint8_t *out) {
    if (!g_snap_ready) return 0;
    EnterCriticalSection(&g_snap_cs);
    if (g_snap_fresh) {
        int t = g_snap_older; g_snap_older = g_snap_front; g_snap_front = g_snap_latest; g_snap_latest = t;
        g_snap_fresh = 0;
    }
    LeaveCriticalSection(&g_snap_cs);
    const double F = 1000.0 / 60.0988;
    IpSnap *s = g_snap[g_snap_front], *o = g_snap[g_snap_older];
    if (!s->valid) return 0;
    if (when_ms < s->t0 && o->valid && fabs(o->t0 - (s->t0 - F)) < 1.0) s = o;   /* the step before */
    double t = (when_ms - s->t0) / F;
    if (t < 0 || t > 1.0) hw_snap_clamps++;                  /* diagnostics: a frame wasn't there */
    if (t >= 0.999 || s->cur.mixed_blank) { memcpy(out, s->real, (size_t)s->real_w * 224 * 4); return s->real_w; }
    return ip_render(&s->ppu, &s->cur, &s->prev, (float)(t < 0 ? 0 : t), out);
}

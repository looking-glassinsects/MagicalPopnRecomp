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
static LARGE_INTEGER g_frame_t0, g_qpf;      /* when emulating the current frame began */
static double g_extra_budget_ms;             /* 0 = no time limit on extra lines */
static void vblank_tramp(void *user) {
    (void)user; ip_end_frame();
    if (g_vblank_cb) g_vblank_cb();
    QueryPerformanceCounter(&g_frame_t0);    /* after the host's pacing and input: the frame starts here */
}
/* another extra line only while this frame's emulation has taken less than the budget: a slow
 * PC removes less of the slowdown instead of missing its own frames */
static bool extra_time_allowed(void *user) {
    (void)user;
    if (g_extra_budget_ms <= 0) return true;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_frame_t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart < g_extra_budget_ms;
}

int hw_init(const uint8_t *rom, size_t size, void (*vblank_cb)(void)) {
    g = snes_init();
    if (!snes_loadRom(g, rom, (int)size)) return 0;
    snes_setPixelFormat(g, pixelFormatXRGB);  /* byte order B,G,R,X = Windows DIB */
    g->ppu->noPixelBuffer = true;             /* only the wide output (wsBuffer) is used */
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
/* [widescreen] Which VRAM words the current room has drawn since it was set up (see
 * wsVramEpoch) is not part of the console's state, so a save state carries it separately. Without
 * it, narrow rooms show tiles left over from earlier rooms at the sides after a load. */
#define WS_STATE_MAGIC 0x31455357u     /* "WSE1" */
int hw_ws_state_size(void) { return 4 + 2 + 1 + (int)sizeof g->ppu->wsVramEpoch; }
void hw_ws_state_save(uint8_t *buf) {
    uint32_t m = WS_STATE_MAGIC; memcpy(buf, &m, 4);
    memcpy(buf + 4, &g->ppu->wsEpoch, 2); buf[6] = g->ppu->wsInLoad;
    memcpy(buf + 7, g->ppu->wsVramEpoch, sizeof g->ppu->wsVramEpoch);
}
int hw_ws_state_load(const uint8_t *buf, int size) {
    uint32_t m;
    if (size != hw_ws_state_size()) return 0;
    memcpy(&m, buf, 4); if (m != WS_STATE_MAGIC) return 0;
    memcpy(&g->ppu->wsEpoch, buf + 4, 2); g->ppu->wsInLoad = buf[6] != 0;
    memcpy(g->ppu->wsVramEpoch, buf + 7, sizeof g->ppu->wsVramEpoch);
    return 1;
}
int hw_save_state(uint8_t *buf) { return snes_saveState(g, buf); }
int hw_load_state(uint8_t *buf, int size) {
    if (!snes_loadState(g, buf, size)) return 0;
    g_samples_primed = 0;
    ws_reset_anchors();
    ip_invalidate();
    return 1;
}

static void ws_audit_frame(void);
void hw_end_frame(void) {
    /* LakeSnes only catches the APU up on port access or in snes_runFrame (which we
     * don't use); a side-effect-free port read forces the catch-up each frame. */
    (void)snes_readBBus(g, 0x40);
    ws_audit_frame();
}

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
typedef struct { int delta; int locked; int first_h; int stride; int scrolled; int rdelta; int miss; int map; } WsAnchor;
static WsAnchor g_anchor[2] = {{-1, 0, -1, 0, 0, 0, 0, -1}, {-1, 0, -1, 0, 0, 0, 0, -1}};
static void ws_reset_anchors(void) { for (int i = 0; i < 2; i++) { g_anchor[i].delta = -1; g_anchor[i].locked = 0; g_anchor[i].first_h = -1; g_anchor[i].stride = 0; g_anchor[i].scrolled = 0; g_anchor[i].miss = 0; g_anchor[i].map = -1; } }

static int g_dbg_best, g_dbg_best_d, g_dbg_best_rd, g_dbg_total, g_dbg_nseen, g_dbg_odd;
static int g_match_ties;   /* last full ws_match search: anchors sharing the best score (1 = unique) */
static int g_ties_agree;   /* ...and whether all of them give the same tiles beside the visible window */
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
    int total = 17 * NR, best = -1, best_d = -1, best_rd = 0, raw_best = -1, ties = 0;
    int tie_d[16], tie_rd[16];
    g_match_ties = 0; g_ties_agree = 0;
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
    /* Steady scrolling: last frame's anchor nearly always still fits exactly. Score it first,
     * and when every sampled tile matches, keep it without searching the whole map (the search
     * is the single most expensive thing in a frame). This is the search's own answer: nothing
     * can score higher than a perfect fit, and the search gives ties to the last anchor. */
    if (a->delta >= 0 && a->stride == stride && a->map == map) {
        int ok_rows = 1, score = 0;
        for (int j = 0; j < NR && ok_rows; j++) { int r = lrow[j] + a->rdelta; if (r < 0 || r >= rows) ok_rows = 0; }
        if (ok_rows) {
            for (int j = 0; j < NR; j++)
                score += ws_row_score(base, stride, w, lrow[j] + a->rdelta, mcj[j] + a->delta, pg[j], mcj[j] != mc0, vrow[j]);
            if (score == total) {
                g_dbg_best = score; g_dbg_best_d = a->delta; g_dbg_best_rd = a->rdelta;
                a->locked = 1;
                return a->delta;
            }
        }
    }
    a->map = map;
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
            if (score > raw_best) { raw_best = score; ties = 0; }
            if (score == raw_best) { if (ties < 16) { tie_d[ties] = d; tie_rd[ties] = rd; } ties++; }
            if (d == a->delta && rd == a->rdelta) score += 1;   /* prefer last frame's anchor on ties */
            if (score > best) { best = score; best_d = d; best_rd = rd; }
        }
    }
    g_dbg_best = best; g_dbg_best_d = best_d; g_dbg_best_rd = best_rd; g_match_ties = ties;
    /* do the equally good places agree on the 6 tile columns either side of the window? */
    if (ties >= 1 && ties <= 16) {
        g_ties_agree = 1;
        for (int t = 1; t < ties && g_ties_agree; t++)
            for (int j = 0; j < NR && g_ties_agree; j++)
                for (int k = -6; k < 23; k++) {
                    if (k == 0) k = 17;
                    int r0 = lrow[j] + tie_rd[0], r1 = lrow[j] + tie_rd[t];
                    const uint8_t *p0 = base + r0 * stride + ((((mcj[j] + k + tie_d[0]) % w) + w) % w) * 2;
                    const uint8_t *p1 = base + r1 * stride + ((((mcj[j] + k + tie_d[t]) % w) + w) % w) * 2;
                    if (p0[0] != p1[0] || p0[1] != p1[1]) { g_ties_agree = 0; break; }
                }
    }
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
            /* A map exactly as wide as the hardware tilemap counts too (the dark forest streams its
             * 512 px back layer column by column, leaving filler in the tilemap elsewhere), but only
             * on a perfect match whose equally good places (a repeating pattern fits in several) all
             * give the same tiles beside the window: in halls hung with curtains they don't, and the
             * wrong one patches the sides with pieces of curtain. */
            if (st >= 64 && st <= 1024 && !(st & 1)) {
                WsAnchor tmp = g_anchor[1];
                if (st == 64) tmp.delta = -1;          /* a full search, so ties are counted */
                parallax_ring = ws_match(ppu, 1, s->ram + 0x10000 + 0xC100, st, 0x3F00 / st, &tmp) >= 0 &&
                                (st > 64 || (g_dbg_best == g_dbg_total && g_ties_agree));
                /* ...and, for those, only once it has fitted 3 frames running: for a single frame
                 * the game's copy can hold another area that happens to fit */
                static int narrow_run;
                if (st == 64) { narrow_run = parallax_ring ? narrow_run + 1 : 0; if (narrow_run < 3) parallax_ring = 0; }
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
            /* A back layer whose level map is narrower than the room (a 512 px ring behind a wider
             * playfield): rows the game hasn't decoded for this spot yet still hold another part of
             * the area (a patch of a different wall), while the screen already shows the right
             * tiles. Such a row is recognised by its on-screen part not matching the screen; its
             * on-screen columns are then taken from the screen, and the rest of it from the
             * screen's own repeating pattern, when the wall has one. Rows that match are left
             * alone: their off-screen tiles are the real ones. */
            if (l == 1 && stride == 64 && (s->ram[0x84] | (s->ram[0x85] << 8)) > 64) {
                int ww = 32;
                int hs = ppu->bgLayer[1].hScroll & 0x3ff, vs = ppu->bgLayer[1].vScroll & 0x3ff;
                int mc = hs >> 4, r0 = vs >> 4;
                uint16_t mp = ppu->bgLayer[1].tilemapAdr;
#define VT(j, k) ppu->vram[(mp + ((r0 + (j)) & 31) * 32 + ((mc + (k)) & 31)) & 0x7fff]
#define LV(r, k) (uint16_t)(base[((r) * ww + ((mc + (k) + d) % ww)) * 2] | (base[((r) * ww + ((mc + (k) + d) % ww)) * 2 + 1] << 8))
                int per = -1;                                /* the wall's period on screen, in tiles (found when needed) */
                for (int j = 0; j <= 14; j++) {
                    int r = r0 + j + a->rdelta; if (r < 0 || r >= rows) continue;
                    int match = 0;                           /* (edge columns may not be drawn yet: 1..15 only) */
                    for (int k = 1; k <= 15; k++) match += LV(r, k) == VT(j, k);
                    if (match >= 12) continue;
                    if (per < 0) {
                        per = 0;
                        for (int P = 1; P <= 16 && !per; P++) {
                            int n = 0, bad = 0;
                            for (int jj = 1; jj <= 13; jj++)
                                for (int k = 0; k + P <= 16; k++) { if (VT(jj, k) != VT(jj, k + P)) bad++; else n++; }
                            if (n >= 12 && bad * 10 <= n) per = P;   /* (16 tiles: one pair per row on screen) */
                        }
                    }
                    for (int k = -8; k <= 24; k++) {
                        int kk = k;
                        if (k < 0 || k > 16) {
                            if (!per) continue;
                            kk = ((k % per) + per) % per; if (kk == 0) kk = per < 16 ? per : 0;
                        }
                        int i = r * ww + ((mc + k + d) % ww + ww) % ww; uint16_t v = VT(j, kk);
                        stable[l][i * 2] = (uint8_t)v; stable[l][i * 2 + 1] = (uint8_t)(v >> 8);
                    }
                }
#undef LV
#undef VT
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
    /* A gameplay room no wider than the 512 px hardware tilemap whose picture can't be found in
     * the level map (the playfield layer is empty there, or the game drew the room into the
     * tilemap only): the tilemap holds the whole room, so the sides come straight from it, up to
     * the room's own ends (from the camera $62 and the room width $86). */
    if (!ppu->wsSrc[0].valid && ppu->mode == 1 && !ppu->forcedBlank && (ppu->bgLayer[2].hScroll & 0x3ff) == 0x3FC &&
        ppu->layer[2].mainScreenEnabled && ppu->bgLayer[0].bigTiles && !ppu->bgLayer[0].tilemapWider) {
        int w_px = (s->ram[0x86] | (s->ram[0x87] << 8)) * 16 + 256;
        int cam = (s->ram[0x62] | (s->ram[0x63] << 8)) - 0x180;
        int h = ppu->bgLayer[0].hScroll & 0x3ff;
        int k = (cam - h + 512) >> 10;
        if (cam - h + 512 < 0) k = -((h - cam + 511) >> 10);
        int cam_r = h + 1024 * k;
        if (w_px > 256 && w_px <= 512 && cam_r - cam < 64 && cam - cam_r < 64 && cam > -64 && cam < w_px) {
            ws_src_vram(ppu, 0);
            ppu->wsSrc[0].clamp = true; ppu->wsSrc[0].lxMin = h - cam_r; ppu->wsSrc[0].lxMax = h - cam_r + w_px;
            ppu->wsRefH = h; ppu->wsCamLine = 1;
            if (!ppu->wsSrc[1].valid && ppu->bgLayer[1].bigTiles && !ppu->bgLayer[1].tilemapWider) {
                ws_back_from_vram(ppu);
                if ((ppu->bgLayer[1].hScroll & 0x3ff) == h) {
                    ppu->wsSrc[1].clamp = true; ppu->wsSrc[1].lxMin = ppu->wsSrc[0].lxMin; ppu->wsSrc[1].lxMax = ppu->wsSrc[0].lxMax;
                }
            }
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
    /* the vertical scroll each source's anchor belongs to (see Ppu.wsSrc refV) */
    for (int l = 0; l < 4; l++) ppu->wsSrc[l].refV = ppu->bgLayer[l].vScroll & 0x3ff;
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
/* Extra CPU time for frames the game can't finish (see ocMax in snes.h): up to `lines` lines per
 * frame, 0 = the original's timing, slowdown included. */
void hw_set_extra_time(int lines, double budget_ms) {
    g->ocMax = lines < 0 ? 0 : lines > 16768 ? 16768 : lines;
    g_extra_budget_ms = budget_ms;
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_frame_t0);
    g->ocAllow = extra_time_allowed; g->ocUser = NULL;
}
void hw_lag_stats(uint32_t *lag_frames, uint32_t *lines) { *lag_frames = g->ocLagFrames; *lines = g->ocLinesTotal; }
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
/* Magical Pop'n's objects: 48-byte records from $1000; +0 handler, +4 X (+7 its fraction), +8 Y
 * (+11 its fraction). The camera is at $62/$64. */
#define IP_OBJ_BASE 0x1000
#define IP_OBJ_BYTES 0x0C00
#define IP_OBJ_SIZE 0x30
#define RG_SIZE  (offsetof(Ppu, pixelBuffer) - offsetof(Ppu, objPriority))
typedef struct {
    int valid, lines;
    uint8_t regs[240][RG_SIZE];
    uint8_t has[240];
    uint16_t oam[0x100];
    uint8_t high[0x20];
    uint32_t gfx[128];            /* fingerprint of each sprite's pixel data in VRAM */
    /* the object each sprite was drawn for, and the game's object table and camera as they were
     * when this frame's sprite list was built: exact (sub-pixel) object motion between frames */
    uint16_t tag[128];
    uint8_t objs[IP_OBJ_BYTES];
    int camx, camy, has_objs;
    /* steady motion (ip_steady): where each picture is really drawn, relative to what the game
     * drew, in pixels across: each layer's scroll on each line, and each sprite */
    float ex[4][240];
    float sox[128];
    uint8_t hud[128];             /* the sprite stays put on the screen (status bar) */
    uint8_t nooff[128];           /* the sprite gets no offset (hud, or off the playfield's scrolling lines) */
    int steady;                   /* some offset isn't 0: the real frame is drawn, not shown as is */
    int st_ok;                    /* the offsets were worked out (else all are 0) */
    /* the palette each line was drawn with: games change it during the frame (fades, colour
     * gradients), and the end-of-frame palette alone would recolour the earlier lines */
    int mixed_blank;              /* the screen was switched off for part of the frame */
    int cg_n;                     /* distinct palettes this frame */
    uint8_t cg_of_line[240];
    uint16_t cg[240][0x100];
} IpFrame;
static IpFrame *g_ip[2];          /* [0] previous frame, [1] current frame */
static int g_ip_on;               /* recording enabled */
/* sprite -> object, as the game writes its sprite list; the list built during one frame is shown
 * in the next, so its tags and the objects' positions wait one frame (pending) */
static uint16_t g_tag_build[128], g_tag_pending[128];
static uint8_t g_objs_pending[IP_OBJ_BYTES], g_list_pending[0x200];
static int g_cam_pending[2], g_pending_ok;
void hw_oam_tag(int sprite, uint16_t object) { if (sprite >= 0 && sprite < 128) g_tag_build[sprite] = object; }

/* test switch POPN_WS_AUDIT: what the 16:9 side columns leave out, counted per frame and printed
 * as one WSAUDIT line at exit. Frames with a picture; of those, sides blacked out (on a gameplay
 * screen / on another screen); BG3 or BG4 showing at the 4:3 edge below the status bar (cut off
 * at the sides: main screen / sub screen); frames with holes filled from the previous frame;
 * mode 7 or hi-res frames; objects appearing or vanishing while wholly inside a side column. */
extern int ppuAuditOn; extern unsigned ppuAuditEdge, ppuAuditHoles;
static struct { unsigned pic, blank_game, blank_other, bg3m, bg3s, bg4m, bg4s, holes, m7, hires, appear, vanish, appear_edge, vanish_edge, appear_any, vanish_any; } g_aud;
static struct { uint16_t tag, handler; int inside, edge; } g_aud_prev[128];
static int g_aud_prev_n, g_aud_prev_ok;   /* last frame's objects, and whether it was looked at */
static void ws_audit_print(void) {
    fprintf(stderr, "WSAUDIT pic %u blank_game %u blank_other %u bg3_main %u bg3_sub %u bg4_main %u bg4_sub %u holes %u mode7 %u hires %u appear %u vanish %u appear_edge %u vanish_edge %u appear_any %u vanish_any %u\n",
            g_aud.pic, g_aud.blank_game, g_aud.blank_other, g_aud.bg3m, g_aud.bg3s, g_aud.bg4m, g_aud.bg4s, g_aud.holes, g_aud.m7, g_aud.hires, g_aud.appear, g_aud.vanish, g_aud.appear_edge, g_aud.vanish_edge, g_aud.appear_any, g_aud.vanish_any);
}
static void ws_audit_frame(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("POPN_WS_AUDIT"); on = e ? (atoi(e) > 1 ? 2 : 1) : 0; ppuAuditOn = on > 0; if (on) atexit(ws_audit_print); }
    if (!on) return;
    Ppu *p = g->ppu;
    unsigned edge = ppuAuditEdge, holes = ppuAuditHoles; ppuAuditEdge = 0; ppuAuditHoles = 0;
    if (p->forcedBlank || p->brightness == 0 || p->wsExtra <= 0) { g_aud_prev_ok = 0; return; }
    g_aud.pic++;
    int game = p->mode == 1 && (p->bgLayer[2].hScroll & 0x3ff) == 0x3FC && p->layer[2].mainScreenEnabled;
    if (p->mode == 7) g_aud.m7++;
    if (p->mode == 5 || p->mode == 6) g_aud.hires++;
    if (p->wsBlank) { if (game) g_aud.blank_game++; else g_aud.blank_other++; g_aud_prev_ok = 0; return; }
    if (edge & 0x004) g_aud.bg3m++;
    if (edge & 0x400) g_aud.bg3s++;
    if (edge & 0x008) g_aud.bg4m++;
    if (edge & 0x800) g_aud.bg4s++;
    if (holes) g_aud.holes++;
    /* objects (by the sprites drawn for them) appearing or vanishing inside a side column */
    static const int sizes[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}};
    int ws = p->wsExtra;
    struct { uint16_t tag, handler; int any, inside, minx, maxx; } cur[128]; int n = 0;
    for (int i = 0; i < 128; i++) {
        uint16_t t = g_tag_build[i];
        if (t < IP_OBJ_BASE || t >= IP_OBJ_BASE + IP_OBJ_BYTES || (t - IP_OBJ_BASE) % IP_OBJ_SIZE) continue;
        const uint8_t *e = g->ram + 0x800 + i * 4;
        int hb = g->ram[0xA00 + (i >> 2)] >> ((i & 3) * 2);
        int x = e[0] | ((hb & 1) << 8), y = e[1], sz = sizes[p->objSize & 7][(hb >> 1) & 1];
        if (x >= 384) x -= 512;
        if (y >= 224 && y < 240) continue;
        int in = (x >= -ws + 16 && x + sz <= 0) || (x >= 256 && x + sz <= 256 + ws - 16);
        int k = 0; while (k < n && cur[k].tag != t) k++;
        if (k == n) { cur[n].tag = t; cur[n].handler = (uint16_t)(g->ram[t] | (g->ram[t + 1] << 8)); cur[n].any = 0; cur[n].inside = 1; cur[n].minx = 9999; cur[n].maxx = -9999; n++; }
        cur[k].any = 1; if (!in) cur[k].inside = 0;
        if (x < cur[k].minx) cur[k].minx = x; if (x + sz > cur[k].maxx) cur[k].maxx = x + sz;
    }
    int edgef[128];
    for (int k = 0; k < n; k++) edgef[k] = (cur[k].minx >= -2 && cur[k].minx <= 8) || (cur[k].maxx >= 248 && cur[k].maxx <= 258);
    for (int k = 0; k < n; k++) {
        int was0 = 0; for (int j = 0; j < g_aud_prev_n; j++) if (g_aud_prev[j].tag == cur[k].tag && g_aud_prev[j].handler == cur[k].handler) was0 = 1;
        if (!was0 && g_aud_prev_ok) { g_aud.appear_any++; if (edgef[k]) { g_aud.appear_edge++; if (on > 1) fprintf(stderr, "wsaudit appear_edge f%u obj %04X handler %04X x %d..%d\n", g->frames, cur[k].tag, cur[k].handler, cur[k].minx, cur[k].maxx); } }
        if (!cur[k].inside) continue;
        int was = 0; for (int j = 0; j < g_aud_prev_n; j++) if (g_aud_prev[j].tag == cur[k].tag && g_aud_prev[j].handler == cur[k].handler) was = 1;
        if (!was && g_aud_prev_ok) {
            g_aud.appear++;
            if (on > 1) fprintf(stderr, "wsaudit appear f%u obj %04X handler %04X\n", g->frames, cur[k].tag, cur[k].handler);
        }
    }
    for (int j = 0; j < (g_aud_prev_ok ? g_aud_prev_n : 0); j++) {
        int is0 = 0; for (int k = 0; k < n; k++) if (cur[k].tag == g_aud_prev[j].tag && cur[k].handler == g_aud_prev[j].handler) is0 = 1;
        if (!is0) { g_aud.vanish_any++; if (g_aud_prev[j].edge) { g_aud.vanish_edge++; if (on > 1) fprintf(stderr, "wsaudit vanish_edge f%u obj %04X handler %04X\n", g->frames, g_aud_prev[j].tag, g_aud_prev[j].handler); } }
        if (!g_aud_prev[j].inside) continue;
        int is = 0; for (int k = 0; k < n; k++) if (cur[k].tag == g_aud_prev[j].tag && cur[k].handler == g_aud_prev[j].handler) is = 1;
        if (!is) {
            g_aud.vanish++;
            if (on > 1) fprintf(stderr, "wsaudit vanish f%u obj %04X handler %04X\n", g->frames, g_aud_prev[j].tag, g_aud_prev[j].handler);
        }
    }
    for (int k = 0; k < n; k++) { g_aud_prev[k].tag = cur[k].tag; g_aud_prev[k].handler = cur[k].handler; g_aud_prev[k].inside = cur[k].inside; g_aud_prev[k].edge = edgef[k]; }
    g_aud_prev_n = n; g_aud_prev_ok = 1;
}


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

static void ip_invalidate(void) { if (g_ip[0]) g_ip[0]->valid = g_ip[1]->valid = 0; g_pending_ok = 0; }

/* A sprite's exact on-screen motion from the previous frame to the current one, from its object
 * (in pixels, fractional): 1 if known. Unknown when the sprite wasn't drawn for an object, the
 * object slot now holds a different object, or the move is too big to be one frame's motion. */
static int ip_object_motion(const IpFrame *prev, const IpFrame *cur, int i, float *dx, float *dy) {
    if (!prev->has_objs || !cur->has_objs) return 0;
    unsigned t = cur->tag[i];
    if (t < IP_OBJ_BASE || t >= IP_OBJ_BASE + IP_OBJ_BYTES - IP_OBJ_SIZE || (t - IP_OBJ_BASE) % IP_OBJ_SIZE) return 0;
    const uint8_t *a = prev->objs + (t - IP_OBJ_BASE), *b = cur->objs + (t - IP_OBJ_BASE);
    if ((a[0] | (a[1] << 8)) != (b[0] | (b[1] << 8)) || !(b[0] | b[1])) return 0;   /* another object now */
    int ax = (a[4] | (a[5] << 8)) * 256 + a[7], bx = (b[4] | (b[5] << 8)) * 256 + b[7];
    int ay = (a[8] | (a[9] << 8)) * 256 + a[11], by = (b[8] | (b[9] << 8)) * 256 + b[11];
    int ddx = bx - ax, ddy = by - ay;
    if (ddx > 0x7FFFFF) ddx -= 0x1000000; if (ddx < -0x7FFFFF) ddx += 0x1000000;   /* 16-bit wrap */
    if (ddy > 0x7FFFFF) ddy -= 0x1000000; if (ddy < -0x7FFFFF) ddy += 0x1000000;
    int cdx = (int16_t)(cur->camx - prev->camx), cdy = (int16_t)(cur->camy - prev->camy);
    *dx = ddx / 256.0f - cdx; *dy = ddy / 256.0f - cdy;
    return *dx >= -24 && *dx <= 24 && *dy >= -24 && *dy <= 24;
}

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

/* The fraction of object t's position in frame f, 0..1 (at = 7: X, 11: Y), or -1: no object */
static float ip_object_frac(const IpFrame *f, uint16_t t, int at) {
    if (!f->has_objs || t < IP_OBJ_BASE || t >= IP_OBJ_BASE + IP_OBJ_BYTES - IP_OBJ_SIZE || (t - IP_OBJ_BASE) % IP_OBJ_SIZE) return -1;
    const uint8_t *b = f->objs + (t - IP_OBJ_BASE);
    if (!(b[0] | b[1])) return -1;
    return b[at] / 256.0f;
}

/* Steady motion. The game keeps its camera in whole pixels, so following the heroine at 2.75 px
 * a frame it moves 2, 3, 3, 3: smoothing spreads each frame's step evenly, but the steps still
 * differ from frame to frame. Here each layer's scroll on each line follows a steady path
 * instead: its average motion over the last 8 frames, kept within 3/4 of a pixel of where the game
 * put it (a sudden change of speed is caught up within a frame or two). It returns exactly to
 * the game's position when the layer stops, reverses, or jumps. Sprites in the playfield move
 * with it, at their objects' exact positions (the fraction the game keeps for each); sprites
 * that stay put on the screen while the playfield scrolls (the status bar) are left alone.
 * Every picture, the real frame's moment included, is then drawn at half-pixel precision. */
typedef struct { int on, cnt; float pos, d[8]; } IpTrack;
static IpTrack g_track[4][240];
static struct { uint16_t key; int16_t x, y; } g_hud[64];   /* sprites seen staying put on the screen */
static int g_hud_n, g_hud_next;
static uint16_t g_field_age[240];  /* frames since the playfield layer last scrolled on each line (0: never) */
static int g_ip_steady = -1;
#define IP_STEADY_MAXD 16
static int ip_hud_has(uint16_t key, int x, int y) {
    for (int k = 0; k < g_hud_n; k++) if (g_hud[k].key == key && g_hud[k].x == x && g_hud[k].y == y) return 1;
    return 0;
}
static void ip_steady(IpFrame *f, const IpFrame *pv) {
    memset(f->ex, 0, sizeof f->ex); memset(f->sox, 0, sizeof f->sox); memset(f->hud, 0, sizeof f->hud); memset(f->nooff, 0, sizeof f->nooff);
    f->steady = 0;
    if (g_ip_steady < 0) g_ip_steady = getenv("POPN_IP_NOSTEADY") == NULL;
    int ok = g_ip_steady && f->valid && pv->valid && !f->mixed_blank && !pv->mixed_blank;
    f->st_ok = ok;
    if (!ok) { memset(g_track, 0, sizeof g_track); memset(g_field_age, 0, sizeof g_field_age); g_hud_n = g_hud_next = 0; return; }
    for (int line = 1; line < 240; line++) {
        if (!f->has[line] || !pv->has[line]) { for (int l = 0; l < 4; l++) g_track[l][line].on = 0; continue; }
        const Ppu *c1 = (const Ppu *)((const uint8_t *)f->regs[line] - RG_START);
        const Ppu *c0 = (const Ppu *)((const uint8_t *)pv->regs[line] - RG_START);
        for (int l = 0; l < 4; l++) {
            IpTrack *s = &g_track[l][line];
            if (c1->mode != c0->mode || c1->forcedBlank || c0->forcedBlank || (c1->mode == 7 && l == 0)) { s->on = 0; continue; }
            int c = c1->bgLayer[l].hScroll & 0x3ff;
            int d = ((c - (c0->bgLayer[l].hScroll & 0x3ff) + 512) & 0x3ff) - 512;
            if (l == 0) g_field_age[line] = d ? 1 : g_field_age[line] && g_field_age[line] < 1000 ? g_field_age[line] + 1 : g_field_age[line];
            if (!s->on || d > IP_STEADY_MAXD || d < -IP_STEADY_MAXD) { s->on = 1; s->cnt = 0; s->pos = (float)c; continue; }
            s->d[s->cnt & 7] = (float)d; s->cnt++;
            int n = s->cnt < 8 ? s->cnt : 8, pos = 0, neg = 0; float v = 0;
            for (int k = 0; k < n; k++) { v += s->d[k]; pos |= s->d[k] > 0; neg |= s->d[k] < 0; }
            v /= n;
            /* stopping, or going back and forth (a ripple, a turn): half way back to where the game
             * put it each frame (no jump on a still screen) */
            float e;
            int stopped = d == 0 && s->cnt >= 2 && s->d[(s->cnt - 2) & 7] == 0;   /* two still frames */
            if (stopped || (pos && neg)) {
                if (pos && neg) s->cnt = 0;
                e = s->pos - c; e -= 1024.0f * floorf(e / 1024.0f + 0.5f);
                e = fabsf(e) < 0.05f ? 0 : 0.5f * e;
            } else {
                e = s->pos + v - c;
                e -= 1024.0f * floorf(e / 1024.0f + 0.5f);
                e *= 0.9f;        /* drawn back toward the game's position, so it stays centred on it */
            }
            if (e > 0.75f) e = 0.75f; if (e < -0.75f) e = -0.75f;
            e = roundf(e * 64.0f) / 64.0f;               /* exact in the sums that use it */
            s->pos = c + e;
            f->ex[l][line] = e;
            if (e != 0) f->steady = 1;
        }
    }
    {   /* test switch POPN_IP_STEADYLOG: each layer's scroll and steady offset on line 120 */
        static int lg = -1; if (lg < 0) lg = getenv("POPN_IP_STEADYLOG") != NULL;
        if (lg) {
            const Ppu *c1 = (const Ppu *)((const uint8_t *)f->regs[120] - RG_START);
            fprintf(stderr, "steady f%u", hw_frame_count());
            for (int l = 0; l < 4; l++) fprintf(stderr, " %d %.4f", c1->bgLayer[l].hScroll & 0x3ff, f->ex[l][120]);
            fprintf(stderr, "\n");
        }
    }
    /* sprites */
    for (int i = 0; i < 128; i++) {
        int hb = (i * 2) & 7, byte = i >> 2;
        int x = (f->oam[i * 2] & 0xff) | (((f->high[byte] >> hb) & 1) << 8), y = f->oam[i * 2] >> 8;
        int big = (f->high[byte] >> (hb + 1)) & 1;
        uint16_t key = f->oam[i * 2 + 1];
        if (y >= 224 && y < 240) continue;
        int line = y + 8; if (line < 1) line = 1; if (line > 239) line = 239;
        if (!ip_hud_has(key, x, y)) {
            /* The same sprite at the same place although what it should move with moved: its
             * object's whole-pixel place on the screen, or (no object) the playfield behind it.
             * It stays put on the screen (the status bar, some of it drawn for the heroine). */
            int moved;
            float odx, ody;
            if (ip_object_motion(pv, f, i, &odx, &ody)) {
                const uint8_t *a = pv->objs + (f->tag[i] - IP_OBJ_BASE), *b = f->objs + (f->tag[i] - IP_OBJ_BASE);
                int ix = (int16_t)((b[4] | (b[5] << 8)) - (a[4] | (a[5] << 8))) - (int16_t)(f->camx - pv->camx);
                int iy = (int16_t)((b[8] | (b[9] << 8)) - (a[8] | (a[9] << 8))) - (int16_t)(f->camy - pv->camy);
                moved = ix || iy;
            } else {
                const Ppu *c1 = (const Ppu *)((const uint8_t *)f->regs[line] - RG_START);
                const Ppu *c0 = (const Ppu *)((const uint8_t *)pv->regs[line] - RG_START);
                moved = f->has[line] && pv->has[line] && (((c1->bgLayer[0].hScroll - c0->bgLayer[0].hScroll) & 0x3ff) || ((c1->bgLayer[0].vScroll - c0->bgLayer[0].vScroll) & 0x3ff));
            }
            if (moved) {
                    for (int j = 0; j < 128; j++) {
                        int pb = (pv->high[j >> 2] >> ((j * 2) & 7)) & 3;
                        if (pv->oam[j * 2 + 1] == key && pv->oam[j * 2] == f->oam[i * 2] &&
                            (pb & 1) == ((x >> 8) & 1) && ((pb >> 1) & 1) == big) {
                            g_hud[g_hud_next].key = key; g_hud[g_hud_next].x = (int16_t)x; g_hud[g_hud_next].y = (int16_t)y;
                            g_hud_next = (g_hud_next + 1) & 63; if (g_hud_n < 64) g_hud_n++;
                            break;
                        }
                    }
            }
        }
        if (ip_hud_has(key, x, y)) { f->hud[i] = f->nooff[i] = 1; continue; }
        /* only on the playfield: lines where it has scrolled lately (the status bar's never do) */
        if (!g_field_age[line] || g_field_age[line] > 120) { f->nooff[i] = 1; continue; }
        float fx = ip_object_frac(f, f->tag[i], 7);
        f->sox[i] = (fx > 0 ? fx : 0) - f->ex[0][line];
        if (f->sox[i] != 0) f->steady = 1;
    }
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
    /* this frame showed the sprite list built during the frame before: its objects are the pending
     * ones; the list just built (and the objects as they are now) belong to the next frame */
    memcpy(f->tag, g_tag_pending, sizeof f->tag);
    /* a frame the game didn't finish shows a half-rebuilt list: a sprite keeps its tag only if it
     * is exactly the entry the tag was noted for */
    for (int i = 0; i < 128; i++)
        if (memcmp(&g->ppu->oam[i * 2], g_list_pending + i * 4, 4) != 0) f->tag[i] = 0;
    memcpy(f->objs, g_objs_pending, sizeof f->objs);
    f->camx = g_cam_pending[0]; f->camy = g_cam_pending[1]; f->has_objs = g_pending_ok;
    memcpy(g_tag_pending, g_tag_build, sizeof g_tag_pending);
    memcpy(g_objs_pending, g->ram + IP_OBJ_BASE, sizeof g_objs_pending);
    memcpy(g_list_pending, g->ram + 0x800, sizeof g_list_pending);
    g_cam_pending[0] = g->ram[0x62] | (g->ram[0x63] << 8); g_cam_pending[1] = g->ram[0x64] | (g->ram[0x65] << 8);
    g_pending_ok = 1;
    ip_steady(f, g_ip[0]);
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

/* Half-pixel pictures. A layer or sprite that moves 1 px per game frame can't be drawn half way
 * in whole pixels, so the in-between picture is made twice as wide: its even columns come from
 * a picture drawn with every half-way position rounded one way (phase 0) and its odd columns
 * from one rounded the other way (phase 1). Something on a whole pixel is identical in both, so
 * it keeps its exact shape; something half way across is drawn half a pixel over. The same goes
 * down the screen: then the picture is also twice as tall, its even rows drawn with vertical
 * half-way positions rounded one way (vertical phase 0), its odd rows the other way. */
#ifdef _MSC_VER
#define HW_THREAD_LOCAL __declspec(thread)
#else
#define HW_THREAD_LOCAL __thread
#endif
static HW_THREAD_LOCAL int ip_half_seen;     /* this picture put something on a half pixel: 1 across, 2 down */
static HW_THREAD_LOCAL int ip_line_half;     /* the same, for the layers on the line being drawn */
/* what the PPU's buffers hold from the picture drawn just before, of this same moment: its
 * phases across and down (ip_buf_ok 0: nothing usable) */
static HW_THREAD_LOCAL int ip_buf_ok, ip_buf_px, ip_buf_py;
static HW_THREAD_LOCAL int ip_vlines;        /* lines with something half way down (counted while drawing) */
/* a scroll register (content moves the other way): phase 0 rounds the scroll down. e0/e1: the
 * steady path's offsets from the game's scroll at the previous and current frame (ip_steady).
 * axis: 1 across, 2 down. */
static int ip_scroll_ph(int prev, int cur, int mod, int maxd, float t, int phase, float e0, float e1, int axis) {
    int d = ((cur - prev + mod / 2) & (mod - 1)) - mod / 2;
    if (d > maxd || d < -maxd) return cur;
    /* how far ahead of `cur` (rounded on the position itself, so the same position rounds the
     * same way whichever frame it is reached from) */
    float ahead = (e1 - (d + e1 - e0) * (1.0f - t));
    if (phase < 0) return (cur + (int)floorf(ahead + 0.5f)) & (mod - 1);
    int h2 = (int)floorf(2.0f * ahead + 0.5f);
    if (h2 & 1) { ip_half_seen |= axis; ip_line_half |= axis; }
    return (cur + (phase ? (h2 + 1) >> 1 : h2 >> 1)) & (mod - 1);
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
 * p's registers, sprites and hooks are restored afterwards. phase / vphase: which way half-way
 * positions across / down are rounded (0 or 1), or -1: to whole pixels. */
static int g_ip_noobj = -1;       /* test switch POPN_IP_NOOBJ: the old sprite-only motion */
static int ip_render(Ppu *p, const IpFrame *cur, const IpFrame *prev, float t, int phase, int vphase, uint8_t *out) {
    if (g_ip_noobj < 0) g_ip_noobj = getenv("POPN_IP_NOOBJ") != NULL;
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
    uint8_t half_line[240];          /* lines a sprite on a half pixel covers: 1 across, 2 down */
    memset(half_line, 0, sizeof half_line);
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
            float mdx = (float)mdx_of[i], mdy = (float)mdy_of[i]; int have = have_of[i];
            /* A sprite whose motion the sprites themselves don't show (the list was reshuffled,
             * or its artwork changed) moves with its object, when the previous frame had a sprite
             * of the same object and size where that motion says it came from. The check matters:
             * the game also draws screen-fixed sprites (the status bar) while an object is
             * current. Known sprite motion is kept: it is whole pixels and lands exactly on the
             * next real frame. */
            /* An object changing shape (a branch bending under the heroine) has its pieces move
             * about within it, so for a sprite not known to stay put on the screen (the status
             * bar: Ip.hud, known while steady motion runs) any of its object's sprites nearby,
             * of any size, will do. Otherwise it would stay where it is now while the background
             * it grows from moves on, and come apart from it. */
            float odx, ody;
            if (!have && !g_ip_noobj && ys[1][i] < 240 && ip_object_motion(prev, cur, i, &odx, &ody)) {
                float px = xs[1][i] - odx, py = ys[1][i] - ody;
                int loose = cur->st_ok && !cur->hud[i];
                float tol = loose ? 24.0f : 1.0f;
                for (int j = 0; j < 128; j++) {
                    if (prev->tag[j] != cur->tag[i] || (!loose && sz[0][j] != sz[1][i])) continue;
                    if (ys[0][j] >= 224 && ys[0][j] < 240) continue;
                    float ex = fabsf(fmodf(xs[0][j] - px + 768.0f, 512.0f) - 256.0f);
                    float ey = fabsf(fmodf(ys[0][j] - py + 384.0f, 256.0f) - 128.0f);
                    if (ex <= tol && ey <= tol) {
                        /* in whole pixels as the game drew it: the change of the objects' fractions
                         * isn't part of the move from one drawn position to the other */
                        float a0 = ip_object_frac(prev, cur->tag[i], 7), a1 = ip_object_frac(cur, cur->tag[i], 7);
                        float b0 = ip_object_frac(prev, cur->tag[i], 11), b1 = ip_object_frac(cur, cur->tag[i], 11);
                        mdx = lroundf(odx - (a0 >= 0 && a1 >= 0 ? a1 - a0 : 0));
                        mdy = lroundf(ody - (b0 >= 0 && b1 >= 0 ? b1 - b0 : 0));
                        have = 1; break;
                    }
                }
            }
            /* Still unknown (new artwork every frame, and no object to follow: the tip of a branch
             * bending under the heroine, whose base is background): it moves with the background
             * behind it rather than standing still on the screen, so it stays joined to what it
             * grows from while the view scrolls. Sprites that do stay put on the screen (the
             * status bar) are known (Ip.hud, while steady motion runs) and keep still. */
            if (!have && cur->st_ok && !cur->hud[i] && ys[1][i] < 224 && (wdx[i] || wdy[i])) {
                mdx = (float)wdx[i]; mdy = (float)wdy[i]; have = 1;
            }
            /* steady motion: where this sprite really is drawn now (so1) and was drawn in the
             * previous frame (so0), relative to the game's whole-pixel positions */
            float so1 = cur->sox[i], so0 = 0;
            if (have && !cur->nooff[i] && prev->st_ok) {
                int l0 = ys[1][i] - (int)mdy + 8; if (l0 < 1) l0 = 1; if (l0 > 239) l0 = 239;
                float f0 = ip_object_frac(prev, cur->tag[i], 7);
                if (f0 < 0) f0 = so1 + cur->ex[0][ys[1][i] + 8 < 1 ? 1 : ys[1][i] + 8 > 239 ? 239 : ys[1][i] + 8];
                so0 = f0 - prev->ex[0][l0];
            }
            int x = xs[1][i], y = ys[1][i];
            float ahead = have ? so1 - (mdx + so1 - so0) * (1.0f - t) : so1;   /* how far right of x */
            int halfs = 0;                               /* 1: half way across, 2: down */
            if (phase < 0) x = (x + (int)floorf(ahead + 0.5f)) & 511;
            else {                                       /* screen position: phase 0 rounds up */
                int h2 = (int)floorf(2.0f * ahead + 0.5f);
                if (h2 & 1) halfs |= 1;
                x = (x + (phase ? h2 >> 1 : (h2 + 1) >> 1)) & 511;
            }
            if (have) {
                if (vphase < 0) y = (y - (int)lroundf(mdy * (1.0f - t))) & 255;
                else {                                   /* the same down the screen */
                    int h2 = (int)floorf(-2.0f * mdy * (1.0f - t) + 0.5f);
                    if (h2 & 1) halfs |= 2;
                    y = (y + (vphase ? h2 >> 1 : (h2 + 1) >> 1)) & 255;
                }
            }
            if (y >= 224 && y < 240) halfs = 0;          /* parked off the screen */
            ip_half_seen |= halfs;
            if (halfs) {                                 /* the lines it covers (in either phase) */
                static const int sizes[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}};
                int h = sizes[p->objSize & 7][sz[1][i]];
                for (int k = -1; k <= h + 2; k++) { int l = (y + k) & 255; if (l < 240) half_line[l] |= (uint8_t)halfs; }
            }
            int hb = (i * 2) & 7, byte = i >> 2;
            p->oam[i * 2] = (uint16_t)((x & 0xff) | (y << 8));
            p->highOam[byte] = (uint8_t)((p->highOam[byte] & ~(1 << hb)) | (((x >> 8) & 1) << hb));
        }
    }
    int last = cur->lines;
    for (int line = 1; line <= last; line++) {
        if (!cur->has[line]) continue;
        ip_line_half = 0;
        memcpy((uint8_t *)p + RG_START, cur->regs[line], RG_SIZE);
        if (cur->cg_n > 0) memcpy(p->cgram, cur->cg[cur->cg_of_line[line]], sizeof p->cgram);
        p->wsBrightFine = -1;
        if (prev->has[line]) {
            const Ppu *pr = (const Ppu *)((const uint8_t *)prev->regs[line] - RG_START);
            /* a fade: the brightness part of the way from the previous frame's to this one's */
            if (pr->brightness != p->brightness && !p->forcedBlank && !pr->forcedBlank)
                p->wsBrightFine = (int16_t)lroundf((pr->brightness & 15) * 17.0f * (1.0f - t) + (p->brightness & 15) * 17.0f * t);
            if (pr->mode == p->mode && !p->forcedBlank && !pr->forcedBlank)
                for (int l = 0; l < 4; l++) {
                    if (p->mode == 7 && l == 0) continue;
                    p->bgLayer[l].hScroll = (uint16_t)ip_scroll_ph(pr->bgLayer[l].hScroll & 0x3ff, p->bgLayer[l].hScroll & 0x3ff, 1024, 64, t, phase,
                                                                       prev->ex[l][line], cur->ex[l][line], 1);
                    p->bgLayer[l].vScroll = (uint16_t)(vphase < 0
                        ? ip_lerp(pr->bgLayer[l].vScroll & 0x3ff, p->bgLayer[l].vScroll & 0x3ff, 1024, 64, t)
                        : ip_scroll_ph(pr->bgLayer[l].vScroll & 0x3ff, p->bgLayer[l].vScroll & 0x3ff, 1024, 64, t, vphase, 0, 0, 2));
                }
        }
        /* Another half-pixel picture of the same moment differs from the one still in the PPU's
         * buffers only on lines where something sits half way along an axis whose rounding
         * changed; the others are kept as they are. */
        if ((ip_line_half | half_line[line]) & 2) ip_vlines++;
        if (ip_buf_ok && line > 1) {
            int h = ip_line_half | half_line[line];
            if (!((phase != ip_buf_px && (h & 1)) || (vphase != ip_buf_py && (h & 2)))) {
                if (line == p->wsCamLine) p->wsEdgeScroll = p->bgLayer[0].hScroll;
                continue;
            }
        }
        ppu_runLine(p, line);
    }
    int w = ppu_putPixelsWide(p, out);
    memcpy((uint8_t *)p + RG_START, regs, RG_SIZE);
    memcpy(p->cgram, cgram, sizeof cgram);
    memcpy(p->oam, oam, sizeof oam); memcpy(p->highOam, high, sizeof high);
    p->lineHook = line_hook; p->wsFrameHook = frame_hook; p->wsUser = user; p->wsBlank = blank; p->wsNoRecord = false;
    p->wsBrightFine = -1;
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
    return ip_render(g->ppu, g_ip[0], g_ip[1], t, -1, -1, out);
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
        /* (not the 4:3 pixel buffer: the game doesn't use it, so it isn't even drawn) */
        memcpy(&s->ppu, g->ppu, offsetof(Ppu, pixelBuffer));
        memcpy((uint8_t *)&s->ppu + offsetof(Ppu, pixelBuffer) + sizeof g->ppu->pixelBuffer,
               (const uint8_t *)g->ppu + offsetof(Ppu, pixelBuffer) + sizeof g->ppu->pixelBuffer,
               sizeof(Ppu) - offsetof(Ppu, pixelBuffer) - sizeof g->ppu->pixelBuffer);
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
/* the snapshot that covers `when_ms` and how far into its frame that moment is (NULL: none yet) */
static IpSnap *snap_pick(double when_ms, double *tp) {
    if (!g_snap_ready) return NULL;
    EnterCriticalSection(&g_snap_cs);
    if (g_snap_fresh) {
        int t = g_snap_older; g_snap_older = g_snap_front; g_snap_front = g_snap_latest; g_snap_latest = t;
        g_snap_fresh = 0;
    }
    LeaveCriticalSection(&g_snap_cs);
    const double F = 1000.0 / 60.0988;
    IpSnap *s = g_snap[g_snap_front], *o = g_snap[g_snap_older];
    if (!s->valid) return NULL;
    if (when_ms < s->t0 && o->valid && fabs(o->t0 - (s->t0 - F)) < 1.0) s = o;   /* the step before */
    *tp = (when_ms - s->t0) / F;
    if (*tp < 0 || *tp > 1.0) hw_snap_clamps++;              /* diagnostics: a frame wasn't there */
    return s;
}

int hw_snapshot_render(double when_ms, uint8_t *out) {
    double t;
    IpSnap *s = snap_pick(when_ms, &t);
    if (!s) return 0;
    if (t >= 0.999 || s->cur.mixed_blank) { memcpy(out, s->real, (size_t)s->real_w * 224 * 4); return s->real_w; }
    return ip_render(&s->ppu, &s->cur, &s->prev, (float)(t < 0 ? 0 : t), -1, -1, out);
}

/* The same picture at half-pixel precision: `out` (HW_FINE_MAX_W x 448) gets 2 x width columns
 * and 224 rows, or 448 rows when something sits half way down (then HW_FINE_TALL is added to the
 * returned width). Returns the width in game pixels, or 0. Display thread only.
 * Up to four pictures of the moment are drawn: phases across x down (0,0), (1,0), (0,1), (1,1);
 * each after the first redraws only the lines that differ from the one before it. */
volatile int hw_fine_on = 2;   /* 2: half pixels across and down, 1: across only, 0: none (the display
                                * thread steps it down when the pictures don't fit in time) */
volatile double hw_fine_budget_ms = 1e9;   /* the time the display thread has for drawing a picture */
int hw_snapshot_render_fine(double when_ms, uint8_t *out) {
    static uint8_t pic[4][HW_WIDE_MAX_W * 224 * 4];
    double t;
    IpSnap *s = snap_pick(when_ms, &t);
    if (!s) return 0;
    int w, tall = 0;
    const uint8_t *pp[4] = {pic[0], pic[1], pic[2], pic[3]};
    /* the real frame's moment is shown as the game drew it, unless steady motion moved something */
    if ((t >= 0.999 && !s->cur.steady) || s->cur.mixed_blank) { w = s->real_w; pp[0] = pp[1] = s->real; }
    else {
        float ft = (float)(t < 0 ? 0 : t > 1 ? 1 : t);
        static int lvl = -2;     /* test switch POPN_FINE=0/1/2: that level instead of hw_fine_on */
        if (lvl == -2) {
            const char *e = getenv("POPN_FINE"); lvl = e ? atoi(e) : -1;
            e = getenv("POPN_FINE_BUDGET"); if (e) hw_fine_budget_ms = atof(e);   /* test switch: ms */
        }
        int fine = lvl >= 0 ? lvl : hw_fine_on;
        ip_half_seen = 0; ip_buf_ok = 0; ip_vlines = 0;
        LARGE_INTEGER c0, c1, cf; QueryPerformanceCounter(&c0);
        w = ip_render(&s->ppu, &s->cur, &s->prev, ft, 0, fine >= 2 ? 0 : -1, pic[0]);
        QueryPerformanceCounter(&c1); QueryPerformanceFrequency(&cf);
        int half = ip_half_seen;
        int across = (half & 1) && fine >= 1, down = (half & 2) && fine >= 2;
        /* Half pixels down redraw every line with something half way down, once or twice more
         * (twice when there are half pixels across too). When that wouldn't fit in the time
         * the display has for a picture, this one is drawn with whole pixels down. */
        if (down) {
            double one = (c1.QuadPart - c0.QuadPart) * 1000.0 / cf.QuadPart;   /* ms for a whole picture */
            double est = one * (across ? 2.0 : 1.0) + one * ip_vlines / 224.0 * (across ? 2.0 : 1.0);
            if (est > hw_fine_budget_ms) down = 0;
        }
        int done[4] = {1, 0, 0, 0};
        ip_buf_ok = 1; ip_buf_px = 0; ip_buf_py = fine >= 2 ? 0 : -1;
        /* the order keeps each picture one rounding change away from the one before it */
        static const int order[3] = {1, 3, 2};
        for (int k = 0; k < 3; k++) {
            int q = order[k], px = q & 1, py = q >> 1;
            if ((px && !across) || (py && !down)) continue;
            if (ip_render(&s->ppu, &s->cur, &s->prev, ft, px, py ? 1 : (fine >= 2 ? 0 : -1), pic[q]) != w) break;
            done[q] = 1; ip_buf_px = px; ip_buf_py = py ? 1 : (fine >= 2 ? 0 : -1);
        }
        ip_buf_ok = 0;
        if (!done[1]) pp[1] = pp[0];
        tall = down && done[2];
        if (tall && !done[3]) pp[3] = pp[2];
        {   /* test switch POPN_IP_REUSECHK: each picture drawn in full must be the same */
            static int chk = -1; static unsigned n, bad;
            if (chk < 0) chk = getenv("POPN_IP_REUSECHK") != NULL;
            if (chk)
                for (int q = 1; q < 4; q++) {
                    if (!done[q]) continue;
                    static uint8_t c[HW_WIDE_MAX_W * 224 * 4];
                    (void)ip_render(&s->ppu, &s->cur, &s->prev, ft, q & 1, (q >> 1) ? 1 : (fine >= 2 ? 0 : -1), c);
                    n++; if (memcmp(c, pic[q], (size_t)w * 224 * 4)) bad++;
                    if (n % 100 == 0) fprintf(stderr, "reusechk %u pictures, %u differ\n", n, bad);
                }
        }
    }
    uint32_t *uo = (uint32_t *)out;
    for (int r = 0; r < (tall ? 2 : 1); r++) {
        const uint32_t *ua = (const uint32_t *)pp[r * 2], *ub = (const uint32_t *)pp[r * 2 + 1];
        for (int y = 0; y < 224; y++) {
            uint32_t *row = uo + ((size_t)(tall ? y * 2 + r : y) * w * 2);
            const uint32_t *ra = ua + (size_t)y * w, *rb = ub + (size_t)y * w;
            for (int x = 0; x < w; x++) { row[x * 2] = ra[x]; row[x * 2 + 1] = rb[x]; }
        }
    }
    return w | (tall ? HW_FINE_TALL : 0);
}

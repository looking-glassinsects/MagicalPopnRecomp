/* Helpers used by generated code (out/gen/*.c). Semantics follow bsnes' 65816 core. */
#pragma once
#include "cpu_state.h"

#ifdef _MSC_VER
#pragma warning(disable: 4102) /* unreferenced label */
#define RT_INLINE static __forceinline
#else
#define RT_INLINE static inline
#endif

void rt_dispatch(Cpu *c, uint32_t addr);
void rt_bad_return(Cpu *c, uint32_t expected);
void rt_wai(Cpu *c);
void rt_stop(Cpu *c, uint32_t pc);
void rt_soft_irq(Cpu *c, uint32_t pc, int cop);
uint32_t canon24(uint32_t addr);
GenFn gen_lookup(uint32_t addr, int m, int x);
GenFn gen_lookup_any(uint32_t addr, int *m, int *x);

#include "hw.h"
extern int rt_ws_extra;     /* widescreen columns per side the game should draw sprites into (0 = off) */
extern uint32_t rt_loop_pc;
extern volatile int rt_capture_req;
void rt_poll(Cpu *c);
void rt_capture_native(Cpu *c, uint32_t pc);
/* Loop back-edge, about to jump to `target`. The CPU state here is exactly the 65816 state at
 * `target`, so this is where save states are taken; pending interrupts are serviced too. */
#define RT_TICK(c, target) do { rt_loop_pc = (target); \
    if (rt_capture_req) rt_capture_native((c), (target)); \
    if (*hw_nmi_ptr || (*hw_irq_ptr && !((c)->p & P_I))) rt_poll(c); } while (0)
/* opcode fetch + internal-operation time of one instruction */
#define RT_FETCH(bank, nbytes, idle) hw_fetch((bank), (nbytes), (idle))
/* After a call: if the callee returned somewhere else (it popped or pushed return addresses,
 * as 65816 code often does), unwind this C frame too. The first frame that expects
 * c->ret resumes; if none does, the top level dispatches to c->ret. */
#define RET_CHECK(c, want) do { if ((c)->ret != (want)) { rt_bad_return((c), (want)); return; } } while (0)

/* ---- memory ---- */
RT_INLINE uint8_t  rd8(uint32_t a) { return bus_read8(a & 0xFFFFFF); }
RT_INLINE void     wr8(uint32_t a, uint8_t v) { bus_write8(a & 0xFFFFFF, v); }
RT_INLINE uint16_t rd16l(uint32_t a) { return (uint16_t)(rd8(a) | (rd8(a + 1) << 8)); }
RT_INLINE void     wr16l(uint32_t a, uint16_t v) { wr8(a, (uint8_t)v); wr8(a + 1, (uint8_t)(v >> 8)); }
RT_INLINE uint16_t rd16z(uint16_t a) { return (uint16_t)(rd8(a) | (rd8((uint16_t)(a + 1)) << 8)); }
RT_INLINE void     wr16z(uint16_t a, uint16_t v) { wr8(a, (uint8_t)v); wr8((uint16_t)(a + 1), (uint8_t)(v >> 8)); }
RT_INLINE uint32_t rd24z(uint16_t a) { return rd16z(a) | ((uint32_t)rd8((uint16_t)(a + 2)) << 16); }
RT_INLINE uint32_t rd24l(uint32_t a) { return rd16l(a) | ((uint32_t)rd8(a + 2) << 16); }
/* [widescreen] how far the wide picture reaches past the 4:3 one on each side, never past the
 * room's own ends (camera X is $62 - 0x180; the room is $86 * 16 + 256 px wide) */
RT_INLINE int rt_ws_reach_left(void) {
    int cam = (int)rd16z(0x62) - 0x180, e = rt_ws_extra;
    return cam < e ? (cam > 0 ? cam : 0) : e;
}
RT_INLINE int rt_ws_reach_right(void) {
    int room = (int)rd16z(0x86) * 16 - ((int)rd16z(0x62) - 0x180), e = rt_ws_extra;
    return room < e ? (room > 0 ? room : 0) : e;
}

/* ---- stack ---- */
RT_INLINE void push8(Cpu *c, uint8_t v) {
    wr8(c->s, v);
    c->s = c->e ? (uint16_t)(0x0100 | (uint8_t)(c->s - 1)) : (uint16_t)(c->s - 1);
}
RT_INLINE uint8_t pull8(Cpu *c) {
    c->s = c->e ? (uint16_t)(0x0100 | (uint8_t)(c->s + 1)) : (uint16_t)(c->s + 1);
    return rd8(c->s);
}
RT_INLINE void push16(Cpu *c, uint16_t v) { push8(c, (uint8_t)(v >> 8)); push8(c, (uint8_t)v); }
RT_INLINE uint16_t pull16(Cpu *c) { uint16_t lo = pull8(c); return (uint16_t)(lo | (pull8(c) << 8)); }

/* ---- flags ---- */
RT_INLINE void setz(Cpu *c, int z) { c->p = (uint8_t)((c->p & ~P_Z) | (z ? P_Z : 0)); }
RT_INLINE void setc(Cpu *c, int v) { c->p = (uint8_t)((c->p & ~P_C) | (v ? P_C : 0)); }
RT_INLINE void setv(Cpu *c, int v) { c->p = (uint8_t)((c->p & ~P_V) | (v ? P_V : 0)); }
RT_INLINE void nz8(Cpu *c, uint8_t v) { c->p = (uint8_t)((c->p & ~(P_N | P_Z)) | (v & 0x80) | (v ? 0 : P_Z)); }
RT_INLINE void nz16(Cpu *c, uint16_t v) { c->p = (uint8_t)((c->p & ~(P_N | P_Z)) | ((v >> 8) & 0x80) | (v ? 0 : P_Z)); }
RT_INLINE void set_p(Cpu *c, uint8_t p) {
    if (c->e) p |= P_M | P_X;
    c->p = p;
    if (p & P_X) { c->x &= 0xFF; c->y &= 0xFF; }
}
RT_INLINE void xce(Cpu *c) {
    uint8_t carry = c->p & P_C;
    c->p = (uint8_t)((c->p & ~P_C) | c->e);
    c->e = carry;
    if (c->e) { c->p |= P_M | P_X; c->x &= 0xFF; c->y &= 0xFF; c->s = (uint16_t)(0x0100 | (uint8_t)c->s); }
}

/* ---- ALU ---- */
RT_INLINE void adc8(Cpu *c, uint8_t v) {
    uint8_t a = (uint8_t)c->a; int r, cy = c->p & P_C;
    if (c->p & P_D) {
        r = (a & 0x0F) + (v & 0x0F) + cy; if (r > 0x09) r += 0x06; cy = r > 0x0F;
        r = (a & 0xF0) + (v & 0xF0) + (cy << 4) + (r & 0x0F);
    } else r = a + v + cy;
    setv(c, ~(a ^ v) & (a ^ r) & 0x80);
    if ((c->p & P_D) && r > 0x9F) r += 0x60;
    setc(c, r > 0xFF);
    c->a = (c->a & 0xFF00) | (uint8_t)r; nz8(c, (uint8_t)r);
}
RT_INLINE void sbc8(Cpu *c, uint8_t v) {
    uint8_t a = (uint8_t)c->a; int r, cy = c->p & P_C;
    v = (uint8_t)~v;
    if (c->p & P_D) {
        r = (a & 0x0F) + (v & 0x0F) + cy; if (r <= 0x0F) r -= 0x06; cy = r > 0x0F;
        r = (a & 0xF0) + (v & 0xF0) + (cy << 4) + (r & 0x0F);
    } else r = a + v + cy;
    setv(c, ~(a ^ v) & (a ^ r) & 0x80);
    if ((c->p & P_D) && r <= 0xFF) r -= 0x60;
    setc(c, r > 0xFF);
    c->a = (c->a & 0xFF00) | (uint8_t)r; nz8(c, (uint8_t)r);
}
RT_INLINE void adc16(Cpu *c, uint16_t v) {
    uint16_t a = c->a; int r, cy = c->p & P_C;
    if (c->p & P_D) {
        r = (a & 0x000F) + (v & 0x000F) + cy;          if (r > 0x0009) r += 0x0006; cy = r > 0x000F;
        r = (a & 0x00F0) + (v & 0x00F0) + (cy << 4) + (r & 0x000F); if (r > 0x009F) r += 0x0060; cy = r > 0x00FF;
        r = (a & 0x0F00) + (v & 0x0F00) + (cy << 8) + (r & 0x00FF); if (r > 0x09FF) r += 0x0600; cy = r > 0x0FFF;
        r = (a & 0xF000) + (v & 0xF000) + (cy << 12) + (r & 0x0FFF);
    } else r = a + v + cy;
    setv(c, ~(a ^ v) & (a ^ r) & 0x8000);
    if ((c->p & P_D) && r > 0x9FFF) r += 0x6000;
    setc(c, r > 0xFFFF);
    c->a = (uint16_t)r; nz16(c, c->a);
}
RT_INLINE void sbc16(Cpu *c, uint16_t v) {
    uint16_t a = c->a; int r, cy = c->p & P_C;
    v = (uint16_t)~v;
    if (c->p & P_D) {
        r = (a & 0x000F) + (v & 0x000F) + cy;          if (r <= 0x000F) r -= 0x0006; cy = r > 0x000F;
        r = (a & 0x00F0) + (v & 0x00F0) + (cy << 4) + (r & 0x000F); if (r <= 0x00FF) r -= 0x0060; cy = r > 0x00FF;
        r = (a & 0x0F00) + (v & 0x0F00) + (cy << 8) + (r & 0x00FF); if (r <= 0x0FFF) r -= 0x0600; cy = r > 0x0FFF;
        r = (a & 0xF000) + (v & 0xF000) + (cy << 12) + (r & 0x0FFF);
    } else r = a + v + cy;
    setv(c, ~(a ^ v) & (a ^ r) & 0x8000);
    if ((c->p & P_D) && r <= 0xFFFF) r -= 0x6000;
    setc(c, r > 0xFFFF);
    c->a = (uint16_t)r; nz16(c, c->a);
}
RT_INLINE void cmp8(Cpu *c, uint8_t r, uint8_t v)   { setc(c, r >= v); nz8(c, (uint8_t)(r - v)); }
RT_INLINE void cmp16(Cpu *c, uint16_t r, uint16_t v) { setc(c, r >= v); nz16(c, (uint16_t)(r - v)); }
RT_INLINE void bit8(Cpu *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(P_N | P_V)) | (v & 0xC0)); setz(c, ((uint8_t)c->a & v) == 0);
}
RT_INLINE void bit16(Cpu *c, uint16_t v) {
    c->p = (uint8_t)((c->p & ~(P_N | P_V)) | ((v >> 8) & 0xC0)); setz(c, (c->a & v) == 0);
}
RT_INLINE uint8_t  inc8(Cpu *c, uint8_t v)   { v++; nz8(c, v); return v; }
RT_INLINE uint16_t inc16(Cpu *c, uint16_t v) { v++; nz16(c, v); return v; }
RT_INLINE uint8_t  dec8(Cpu *c, uint8_t v)   { v--; nz8(c, v); return v; }
RT_INLINE uint16_t dec16(Cpu *c, uint16_t v) { v--; nz16(c, v); return v; }
RT_INLINE uint8_t  asl8(Cpu *c, uint8_t v)   { setc(c, v & 0x80); v <<= 1; nz8(c, v); return v; }
RT_INLINE uint16_t asl16(Cpu *c, uint16_t v) { setc(c, v & 0x8000); v <<= 1; nz16(c, v); return v; }
RT_INLINE uint8_t  lsr8(Cpu *c, uint8_t v)   { setc(c, v & 1); v >>= 1; nz8(c, v); return v; }
RT_INLINE uint16_t lsr16(Cpu *c, uint16_t v) { setc(c, v & 1); v >>= 1; nz16(c, v); return v; }
RT_INLINE uint8_t  rol8(Cpu *c, uint8_t v)   { int cy = c->p & P_C; setc(c, v & 0x80); v = (uint8_t)((v << 1) | cy); nz8(c, v); return v; }
RT_INLINE uint16_t rol16(Cpu *c, uint16_t v) { int cy = c->p & P_C; setc(c, v & 0x8000); v = (uint16_t)((v << 1) | cy); nz16(c, v); return v; }
RT_INLINE uint8_t  ror8(Cpu *c, uint8_t v)   { int cy = c->p & P_C; setc(c, v & 1); v = (uint8_t)((v >> 1) | (cy << 7)); nz8(c, v); return v; }
RT_INLINE uint16_t ror16(Cpu *c, uint16_t v) { int cy = c->p & P_C; setc(c, v & 1); v = (uint16_t)((v >> 1) | (cy << 15)); nz16(c, v); return v; }

/* ---- block move ---- */
RT_INLINE void mvn(Cpu *c, uint8_t src, uint8_t dst) {
    uint16_t mask = (c->p & P_X) ? 0xFF : 0xFFFF;
    do {
        wr8(((uint32_t)dst << 16) | c->y, rd8(((uint32_t)src << 16) | c->x));
        c->x = (c->x + 1) & mask; c->y = (c->y + 1) & mask;
    } while (c->a-- != 0);
    c->db = dst;
}
RT_INLINE void mvp(Cpu *c, uint8_t src, uint8_t dst) {
    uint16_t mask = (c->p & P_X) ? 0xFF : 0xFFFF;
    do {
        wr8(((uint32_t)dst << 16) | c->y, rd8(((uint32_t)src << 16) | c->x));
        c->x = (c->x - 1) & mask; c->y = (c->y - 1) & mask;
    } while (c->a-- != 0);
    c->db = dst;
}

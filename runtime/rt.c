/* Runtime glue between recompiled code and the hardware (hw.c / LakeSnes):
 * bus, indirect-jump dispatch, interrupts, frame hook. */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "rt.h"
#include "rt_state.h"

RtState rt;
uint32_t rt_loop_pc;
volatile int rt_capture_req;

uint32_t canon24(uint32_t a) {
    uint8_t bank = (uint8_t)(a >> 16);
    if (bank == 0x7E || bank == 0x7F || (a & 0xFFFF) < 0x8000) return a;
    return a | 0x800000;
}

void rt_halt(const char *why) {
    fprintf(stderr, "[rt] halt at frame %u: %s\n", hw_frame_count(), why);
    snprintf(rt.halt_reason, sizeof rt.halt_reason, "%s", why);
    longjmp(rt.exit_jmp, 1);
}

uint8_t bus_read8(uint32_t addr) { return hw_read(addr); }
void bus_write8(uint32_t addr, uint8_t v) { hw_write(addr, v); }

/* ---------------------------------------------------------------- control flow */
/* Code the build didn't recompile is logged once (so the builder can include it next time). */
static void note_missing(uint32_t addr, int m, int x, const char *kind) {
    static uint32_t seen[4096]; static int n;
    uint32_t key = (addr << 2) | (m << 1) | x;
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 4096) seen[n++] = key;
    if (!rt.missing_path[0]) return;
    FILE *f = fopen(rt.missing_path, "a");
    if (f) { fprintf(f, "%06X %d %d %s\n", addr, m, x, kind); fclose(f); }
}

static void regs_from_cpu(const Cpu *c, HwRegs *r) {
    r->a = c->a; r->x = c->x; r->y = c->y; r->s = c->s; r->d = c->d; r->db = c->db; r->p = c->p; r->e = c->e;
}
static void cpu_from_regs(Cpu *c, const HwRegs *r) {
    c->a = r->a; c->x = r->x; c->y = r->y; c->s = r->s; c->d = r->d; c->db = r->db; c->p = r->p; c->e = r->e;
}

/* ---- interpreter <-> native glue ---- */
static int ih_has_native(uint32_t t, int m, int x) { return gen_lookup(canon24(t), m, x) != 0; }
static uint32_t ih_call_native(HwRegs *r, uint32_t t) {
    Cpu *c = rt.cpu;
    cpu_from_regs(c, r);
    GenFn f = gen_lookup(canon24(t), !!(c->p & P_M), !!(c->p & P_X));
    rt.stats.dispatches++;
    f(c);
    regs_from_cpu(c, r);
    return c->ret;
}
static int ih_want_capture(void) { return rt_capture_req; }
static void ih_capture(const HwRegs *r, uint32_t pc) {
    rt_capture_req = 0;
    if (rt.on_capture) rt.on_capture(r, canon24(pc));
}

void rt_init_hooks(void) {
    HwInterpHooks h = {ih_has_native, ih_call_native, ih_want_capture, ih_capture};
    hw_set_interp_hooks(&h);
}

/* Save state at a loop back-edge of recompiled code: the state is exactly the CPU state at `pc`. */
void rt_capture_native(Cpu *c, uint32_t pc) {
    if (rt.in_irq) return;                      /* wait until we're out of the interrupt handler */
    HwRegs r; regs_from_cpu(c, &r);
    rt_capture_req = 0;
    if (rt.on_capture) rt.on_capture(&r, canon24(pc));
}

/* Run unrecompiled code in LakeSnes' interpreter until it returns, then continue natively. */
static void interpret(Cpu *c, uint32_t addr) {
    HwRegs r; regs_from_cpu(c, &r);
    rt.stats.interp_calls++;
    rt.interp_depth++;
    uint32_t ret = hw_interpret(&r, addr, 0, &rt.stats.interp_steps);
    rt.interp_depth--;
    cpu_from_regs(c, &r);
    c->ret = canon24(ret);
}

/* Continue from a save state: the 65816 stack in WRAM holds the real return addresses, so
 * the interpreter can carry on from `pc` (running recompiled routines natively as they are
 * called) without the C call stack that existed when the state was taken. */
void rt_resume(Cpu *c, uint32_t pc) {
    HwRegs r; regs_from_cpu(c, &r);
    hw_interpret(&r, pc, 1, &rt.stats.interp_steps);
}

void rt_dispatch(Cpu *c, uint32_t addr) {
    addr = canon24(addr);
    int m = !!(c->p & P_M), x = !!(c->p & P_X);
    GenFn f = gen_lookup(addr, m, x);
    if (!f) {
        note_missing(addr, m, x, "dispatch");
        interpret(c, addr);
        return;
    }
    rt.stats.dispatches++;
    f(c);
}

void rt_bad_return(Cpu *c, uint32_t expected) {
    (void)c; (void)expected;
    rt.stats.unwinds++;
}

/* Top level: run from RESET, then keep resuming wherever control ends up after unwinding. */
void rt_run(Cpu *c, GenFn reset) {
    reset(c);
    for (;;) rt_dispatch(c, c->ret);
}

static void raise_interrupt(Cpu *c, uint32_t entry, const char *name) {
    uint32_t saved_ret = c->ret;
    const uint16_t fake_pc = 0xFFF0;
    if (!c->e) push8(c, 0x00);
    push16(c, fake_pc);
    push8(c, c->p);
    c->p = (uint8_t)((c->p | P_I) & ~P_D);
    hw_idle(7 * 6);
    int m = !!(c->p & P_M), x = !!(c->p & P_X);
    GenFn f = gen_lookup(entry, m, x);
    rt.in_irq++;
    if (f) f(c);
    else { note_missing(entry, m, x, name); interpret(c, entry); }
    rt.in_irq--;
    if ((c->ret & 0xFFFF) != fake_pc) {
        char b[96]; snprintf(b, sizeof b, "interrupt handler returned to $%06X instead of the interrupted code", c->ret);
        rt_halt(b);
    }
    c->ret = saved_ret;
}

/* called by the hardware at the start of every vblank */
void rt_frame_hook(void) {
    hw_end_frame();
    if (rt.on_frame) rt.on_frame();
}

void rt_poll(Cpu *c) {
    if (rt.in_irq) return;
    if (*hw_nmi_ptr) { hw_ack_nmi(); rt.stats.nmis++; raise_interrupt(c, rt.nmi_vec, "nmi"); }
    if (*hw_irq_ptr && !(c->p & P_I)) { rt.stats.irqs++; raise_interrupt(c, rt.irq_vec, "irq"); }
}

void rt_wai(Cpu *c) {
    while (!*hw_nmi_ptr && !*hw_irq_ptr) hw_idle(6);   /* vblank hook still runs inside */
    rt_poll(c);
}

void rt_stop(Cpu *c, uint32_t pc) { (void)c; char b[64]; snprintf(b, sizeof b, "STP at $%06X", pc); rt_halt(b); }

void rt_soft_irq(Cpu *c, uint32_t pc, int cop) {
    /* BRK/COP: take the software interrupt like the CPU would, then continue after it */
    uint16_t vec = c->e ? (cop ? 0xFFF4 : 0xFFFE) : (cop ? 0xFFE4 : 0xFFE6);
    uint32_t entry = canon24(rd16l(vec));
    uint32_t saved_ret = c->ret;
    if (!c->e) push8(c, (uint8_t)(pc >> 16));
    push16(c, (uint16_t)(pc + 2));
    push8(c, c->p);
    c->p = (uint8_t)((c->p | P_I) & ~P_D);
    rt_dispatch(c, entry);
    c->ret = saved_ret;
}

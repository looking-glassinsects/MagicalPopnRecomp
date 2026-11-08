#pragma once
#include <setjmp.h>
#include <stddef.h>
#include "cpu_state.h"
#include "hw.h"

typedef struct RtState {
    Cpu *cpu;
    uint32_t nmi_vec, irq_vec;
    int in_irq, interp_depth;
    void (*on_frame)(void);             /* called at every vblank (present, input, audio) */
    void (*on_capture)(const HwRegs *r, uint32_t pc);  /* save-state capture point reached */
    jmp_buf exit_jmp;
    char halt_reason[160];
    char missing_path[1024];            /* where unrecompiled code addresses get logged */
    struct { unsigned long long dispatches, nmis, irqs, unwinds, interp_calls, interp_steps; } stats;
} RtState;

extern RtState rt;
void rt_halt(const char *why);
void rt_run(Cpu *c, GenFn reset);
void rt_frame_hook(void);
void rt_init_hooks(void);
void rt_resume(Cpu *c, uint32_t pc);

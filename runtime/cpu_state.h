/* 65816 register file that recompiled functions operate on. */
#pragma once
#include <stdint.h>

typedef struct Cpu {
    uint16_t a, x, y, s, d;   /* accumulator, index, stack, direct page */
    uint8_t  db;              /* data bank */
    uint8_t  p;               /* NVMXDIZC */
    uint8_t  e;               /* emulation flag */
    uint32_t ret;             /* 24-bit address the last RTS/RTL/RTI returned to */
    uint32_t ticks;           /* work counter driving the fake frame clock */
} Cpu;

enum { P_C = 0x01, P_Z = 0x02, P_I = 0x04, P_D = 0x08, P_X = 0x10, P_M = 0x20, P_V = 0x40, P_N = 0x80 };

typedef void (*GenFn)(Cpu *c);

/* Memory bus: implemented by the runtime (WRAM, ROM, PPU/APU/DMA registers). */
uint8_t  bus_read8(uint32_t addr);
void     bus_write8(uint32_t addr, uint8_t v);

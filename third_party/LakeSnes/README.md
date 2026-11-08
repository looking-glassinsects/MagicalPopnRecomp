# LakeSnes (vendored)

The `snes/` core of [LakeSnes](https://github.com/angelo-wf/LakeSnes) by angelo_wf, from the commit
listed in `UPSTREAM_COMMIT`. MIT licence, see `LICENSE.txt`.

It provides the SNES hardware: the PPU, the SPC700 + S-DSP, DMA/HDMA, timers, joypads and
cartridge mapping. Its 65816 interpreter is used by the build-time trace profiler, and at
runtime only as a fallback for code the build didn't recompile.

## Local changes

`ppu.c` / `ppu.h` can render extra columns on each side of the 256-pixel picture (widescreen).
Every change is marked `[widescreen]`:
- a wider per-dot output buffer
- sprites with the wider X range
- per-layer extension control
- a per-frame hook through which the host can supply tile data for the extra columns

With `wsExtra = 0` the PPU behaves as upstream.

`snes.c` / `snes.h` add a vblank-start hook (marked `[recomp]`). The host uses it to present
the finished frame and set the controller state right before the auto-joypad read.

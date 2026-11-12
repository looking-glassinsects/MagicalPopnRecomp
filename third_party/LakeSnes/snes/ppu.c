
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ppu.h"
#include "snes.h"
#include "statehandler.h"

// array for layer definitions per mode:
//   0-7: mode 0-7; 8: mode 1 + l3prio; 9: mode 7 + extbg

//   0-3; layers 1-4; 4: sprites; 5: nonexistent
static const int layersPerMode[10][12] = {
  {4, 0, 1, 4, 0, 1, 4, 2, 3, 4, 2, 3},
  {4, 0, 1, 4, 0, 1, 4, 2, 4, 2, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5},
  {4, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5, 5},
  {2, 4, 0, 1, 4, 0, 1, 4, 4, 2, 5, 5},
  {4, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5, 5}
};

static const int prioritysPerMode[10][12] = {
  {3, 1, 1, 2, 0, 0, 1, 1, 1, 0, 0, 0},
  {3, 1, 1, 2, 0, 0, 1, 1, 0, 0, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 0, 0, 5, 5, 5, 5, 5, 5},
  {3, 2, 1, 0, 0, 5, 5, 5, 5, 5, 5, 5},
  {1, 3, 1, 1, 2, 0, 0, 1, 0, 0, 5, 5},
  {3, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5, 5}
};

static const int layerCountPerMode[10] = {
  12, 10, 8, 8, 8, 8, 6, 5, 10, 7
};

static const int bitDepthsPerMode[10][4] = {
  {2, 2, 2, 2},
  {4, 4, 2, 5},
  {4, 4, 5, 5},
  {8, 4, 5, 5},
  {8, 2, 5, 5},
  {4, 2, 5, 5},
  {4, 5, 5, 5},
  {8, 5, 5, 5},
  {4, 4, 2, 5},
  {8, 7, 5, 5}
};

static const int spriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static void ppu_handlePixel(Ppu* ppu, int x, int y);
static int ppu_getPixel(Ppu* ppu, int x, int y, bool sub, int* r, int* g, int* b);
static uint16_t ppu_getOffsetValue(Ppu* ppu, int col, int row);
static int ppu_getPixelForBgLayer(Ppu* ppu, int x, int y, int layer, int priority);
static int ppu_getPixelForBgSource(Ppu* ppu, int x, int y, int layer, int priority);
static void ppu_makeChunky(void);
static void ppu_composeLine(Ppu* ppu, int y, int ws);
static int ppu_decodeBgTile(Ppu* ppu, uint16_t tile, int x, int y, int layer, int priority);
static void ppu_handleOPT(Ppu* ppu, int layer, int* lx, int* ly);
static void ppu_calculateMode7Starts(Ppu* ppu, int y);
static int ppu_getPixelForMode7(Ppu* ppu, int x, int layer, bool priority);
static bool ppu_getWindowState(Ppu* ppu, int layer, int x);
static void ppu_evaluateSprites(Ppu* ppu, int line);
static uint16_t ppu_getVramRemap(Ppu* ppu);

// [widescreen] which load each VRAM word was last written in: writes during forced blank that
// follow a displayed picture start a new load (a new room or screen being set up)
static inline void ppu_wsStampVram(Ppu* ppu, uint16_t adr) {
  if((ppu->forcedBlank || ppu->brightness == 0) && !ppu->wsInLoad) {
    ppu->wsEpoch = (ppu->wsEpoch + 1) & 0x7fff; ppu->wsInLoad = true;
    static int dbg = -1; if(dbg < 0) dbg = getenv("POPN_LOAD_DEBUG") != NULL;
    if(dbg) fprintf(stderr, "vram load %d (forced blank %d, brightness %d) at %04x\n", ppu->wsEpoch, ppu->forcedBlank, ppu->brightness, adr);
  }
  ppu->wsVramEpoch[adr & 0x7fff] = ppu->wsEpoch | (ppu->wsInLoad ? 0x8000 : 0);   // bit 15: written while setting up
  if(((adr - ppu->bgLayer[0].tilemapAdr) & 0x7fff) < 0x400) { ppu->wsDbgMapWrites++; ppu->wsDbgBright = ppu->brightness | (ppu->forcedBlank << 4); }
}

Ppu* ppu_init(Snes* snes) {
  Ppu* ppu = malloc(sizeof(Ppu));
  ppu->snes = snes;
  ppu_setPixelOutputFormat(ppu, ppu_pixelOutputFormatBGRX);
  ppu->wsExtra = 0;      // [widescreen] off until the host enables it
  ppu->wsBrightFine = -1; // [recomp]
  ppu->wsBgMask = 0xf;
  ppu_makeChunky(); // [recomp]
  ppu->noPixelBuffer = false; // [recomp]
  ppu->wsFrameHook = NULL;
  ppu->lineHook = NULL; // [recomp]
  ppu->lineUser = NULL;
  ppu->noSpriteLimit = false;
  ppu->spriteOverflowLines = 0;
  ppu->spriteMaxOnLine = ppu->sliverMaxOnLine = 0;
  ppu->wsUser = NULL;
  ppu->wsBlank = false;
  memset(ppu->wsSrc, 0, sizeof(ppu->wsSrc));
  memset(ppu->wsBuffer, 0, sizeof(ppu->wsBuffer));
  return ppu;
}

void ppu_free(Ppu* ppu) {
  free(ppu);
}

void ppu_reset(Ppu* ppu) {
  memset(ppu->vram, 0, sizeof(ppu->vram));
  ppu->vramPointer = 0;
  ppu->vramIncrementOnHigh = false;
  ppu->vramIncrement = 1;
  ppu->vramRemapMode = 0;
  ppu->vramReadBuffer = 0;
  memset(ppu->cgram, 0, sizeof(ppu->cgram));
  ppu->cgramPointer = 0;
  ppu->cgramSecondWrite = false;
  ppu->cgramBuffer = 0;
  memset(ppu->oam, 0, sizeof(ppu->oam));
  memset(ppu->highOam, 0, sizeof(ppu->highOam));
  ppu->oamAdr = 0;
  ppu->oamAdrWritten = 0;
  ppu->oamInHigh = false;
  ppu->oamInHighWritten = false;
  ppu->oamSecondWrite = false;
  ppu->oamBuffer = 0;
  ppu->objPriority = false;
  ppu->objTileAdr1 = 0;
  ppu->objTileAdr2 = 0;
  ppu->objSize = 0;
  memset(ppu->objPixelBuffer, 0, sizeof(ppu->objPixelBuffer));
  memset(ppu->objPriorityBuffer, 0, sizeof(ppu->objPriorityBuffer));
  ppu->timeOver = false;
  ppu->rangeOver = false;
  ppu->objInterlace = false;
  for(int i = 0; i < 4; i++) {
    ppu->bgLayer[i].hScroll = 0;
    ppu->bgLayer[i].vScroll = 0;
    ppu->bgLayer[i].tilemapWider = false;
    ppu->bgLayer[i].tilemapHigher = false;
    ppu->bgLayer[i].tilemapAdr = 0;
    ppu->bgLayer[i].tileAdr = 0;
    ppu->bgLayer[i].bigTiles = false;
    ppu->bgLayer[i].mosaicEnabled = false;
  }
  ppu->scrollPrev = 0;
  ppu->scrollPrev2 = 0;
  ppu->mosaicSize = 1;
  ppu->mosaicStartLine = 1;
  for(int i = 0; i < 5; i++) {
    ppu->layer[i].mainScreenEnabled = false;
    ppu->layer[i].subScreenEnabled = false;
    ppu->layer[i].mainScreenWindowed = false;
    ppu->layer[i].subScreenWindowed = false;
  }
  memset(ppu->m7matrix, 0, sizeof(ppu->m7matrix));
  ppu->m7prev = 0;
  ppu->m7largeField = false;
  ppu->m7charFill = false;
  ppu->m7xFlip = false;
  ppu->m7yFlip = false;
  ppu->m7extBg = false;
  ppu->m7startX = 0;
  ppu->m7startY = 0;
  for(int i = 0; i < 6; i++) {
    ppu->windowLayer[i].window1enabled = false;
    ppu->windowLayer[i].window2enabled = false;
    ppu->windowLayer[i].window1inversed = false;
    ppu->windowLayer[i].window2inversed = false;
    ppu->windowLayer[i].maskLogic = 0;
  }
  ppu->window1left = 0;
  ppu->window1right = 0;
  ppu->window2left = 0;
  ppu->window2right = 0;
  ppu->clipMode = 0;
  ppu->preventMathMode = 0;
  ppu->addSubscreen = false;
  ppu->subtractColor = false;
  ppu->halfColor = false;
  memset(ppu->mathEnabled, 0, sizeof(ppu->mathEnabled));
  ppu->fixedColorR = 0;
  ppu->fixedColorG = 0;
  ppu->fixedColorB = 0;
  ppu->forcedBlank = true;
  ppu->brightness = 0;
  ppu->mode = 0;
  ppu->bg3priority = false;
  ppu->evenFrame = false;
  ppu->pseudoHires = false;
  ppu->overscan = false;
  ppu->frameOverscan = false;
  ppu->interlace = false;
  ppu->frameInterlace = false;
  ppu->directColor = false;
  ppu->hCount = 0;
  ppu->vCount = 0;
  ppu->hCountSecond = false;
  ppu->vCountSecond = false;
  ppu->countersLatched = false;
  ppu->ppu1openBus = 0;
  ppu->ppu2openBus = 0;
  memset(ppu->pixelBuffer, 0, sizeof(ppu->pixelBuffer));
  memset(ppu->wsBuffer, 0, sizeof(ppu->wsBuffer));
}

void ppu_handleState(Ppu* ppu, StateHandler* sh) {
  // [widescreen] after a save or load, all of VRAM counts as written for the current room (but
  // not as a complete set-up)
  for(int i = 0; i < 0x8000; i++) ppu->wsVramEpoch[i] = ppu->wsEpoch & 0x7fff;
  ppu->wsInLoad = false;
  sh_handleBools(sh,
    &ppu->vramIncrementOnHigh, &ppu->cgramSecondWrite, &ppu->oamInHigh, &ppu->oamInHighWritten, &ppu->oamSecondWrite,
    &ppu->objPriority, &ppu->timeOver, &ppu->rangeOver, &ppu->objInterlace, &ppu->m7largeField, &ppu->m7charFill,
    &ppu->m7xFlip, &ppu->m7yFlip, &ppu->m7extBg, &ppu->addSubscreen, &ppu->subtractColor, &ppu->halfColor,
    &ppu->mathEnabled[0], &ppu->mathEnabled[1], &ppu->mathEnabled[2], &ppu->mathEnabled[3], &ppu->mathEnabled[4],
    &ppu->mathEnabled[5], &ppu->forcedBlank, &ppu->bg3priority, &ppu->evenFrame, &ppu->pseudoHires, &ppu->overscan,
    &ppu->frameOverscan, &ppu->interlace, &ppu->frameInterlace, &ppu->directColor, &ppu->hCountSecond, &ppu->vCountSecond,
    &ppu->countersLatched, NULL
  );
  sh_handleBytes(sh,
    &ppu->vramRemapMode, &ppu->cgramPointer, &ppu->cgramBuffer, &ppu->oamAdr, &ppu->oamAdrWritten, &ppu->oamBuffer,
    &ppu->objSize, &ppu->scrollPrev, &ppu->scrollPrev2, &ppu->mosaicSize, &ppu->mosaicStartLine, &ppu->m7prev,
    &ppu->window1left, &ppu->window1right, &ppu->window2left, &ppu->window2right, &ppu->clipMode, &ppu->preventMathMode,
    &ppu->fixedColorR, &ppu->fixedColorG, &ppu->fixedColorB, &ppu->brightness, &ppu->mode,
    &ppu->ppu1openBus, &ppu->ppu2openBus, NULL
  );
  sh_handleWords(sh,
    &ppu->vramPointer, &ppu->vramIncrement, &ppu->vramReadBuffer, &ppu->objTileAdr1, &ppu->objTileAdr2,
    &ppu->hCount, &ppu->vCount, NULL
  );
  sh_handleWordsS(sh,
    &ppu->m7matrix[0], &ppu->m7matrix[1], &ppu->m7matrix[2], &ppu->m7matrix[3], &ppu->m7matrix[4], &ppu->m7matrix[5],
    &ppu->m7matrix[6], &ppu->m7matrix[7], NULL
  );
  sh_handleIntsS(sh, &ppu->m7startX, &ppu->m7startY, NULL);
  for(int i = 0; i < 4; i++) {
    sh_handleBools(sh,
      &ppu->bgLayer[i].tilemapWider, &ppu->bgLayer[i].tilemapHigher, &ppu->bgLayer[i].bigTiles,
      &ppu->bgLayer[i].mosaicEnabled, NULL
    );
    sh_handleWords(sh,
      &ppu->bgLayer[i].hScroll, &ppu->bgLayer[i].vScroll, &ppu->bgLayer[i].tilemapAdr, &ppu->bgLayer[i].tileAdr, NULL
    );
  }
  for(int i = 0; i < 5; i++) {
    sh_handleBools(sh,
      &ppu->layer[i].mainScreenEnabled, &ppu->layer[i].subScreenEnabled, &ppu->layer[i].mainScreenWindowed,
      &ppu->layer[i].subScreenWindowed, NULL
    );
  }
  for(int i = 0; i < 6; i++) {
    sh_handleBools(sh,
      &ppu->windowLayer[i].window1enabled, &ppu->windowLayer[i].window1inversed, &ppu->windowLayer[i].window2enabled,
      &ppu->windowLayer[i].window2inversed, NULL
    );
    sh_handleBytes(sh, &ppu->windowLayer[i].maskLogic, NULL);
  }
  sh_handleWordArray(sh, ppu->vram, 0x8000);
  sh_handleWordArray(sh, ppu->cgram, 0x100);
  sh_handleWordArray(sh, ppu->oam, 0x100);
  sh_handleByteArray(sh, ppu->highOam, 0x20);
  sh_handleByteArray(sh, ppu->objPixelBuffer, 256);
  sh_handleByteArray(sh, ppu->objPriorityBuffer, 256);
}

bool ppu_checkOverscan(Ppu* ppu) {
  // called at (0,225)
  ppu->frameOverscan = ppu->overscan; // set if we have a overscan-frame
  return ppu->frameOverscan;
}

void ppu_handleVblank(Ppu* ppu) {
  // called either right after ppu_checkOverscan at (0,225), or at (0,240)
  if(!ppu->forcedBlank) {
    ppu->oamAdr = ppu->oamAdrWritten;
    ppu->oamInHigh = ppu->oamInHighWritten;
    ppu->oamSecondWrite = false;
  }
  ppu->frameInterlace = ppu->interlace; // set if we have a interlaced frame
}

void ppu_handleFrameStart(Ppu* ppu) {
  // called at (0, 0)
  ppu->mosaicStartLine = 1;
  ppu->rangeOver = false;
  ppu->timeOver = false;
  ppu->evenFrame = !ppu->evenFrame;
}

// [recomp] Decoded tile rows, cached for the line being drawn. Eight neighbouring pixels on a
// line come from the same row of the same tile, so its planes are read and unpacked once, not
// per pixel. The cache is per thread (the display thread draws with its own Ppu at the same
// time) and is emptied at the start of every line, so a VRAM write between lines can't be missed.
#ifdef _MSC_VER
#define PPU_THREAD_LOCAL __declspec(thread)
#else
#define PPU_THREAD_LOCAL __thread
#endif
static PPU_THREAD_LOCAL unsigned tileGen;
static PPU_THREAD_LOCAL struct { unsigned gen; uint32_t key; uint8_t px[8]; } tileCache[4];

// [widescreen] test audit (host sets ppuAuditOn): which layers show at the 4:3 edges below the
// status bar (bit = layer, main screen in the low byte, sub screen in the next), and how many
// extension pixels were filled from the previous frame (holes). Reset by the host every frame.
int ppuAuditOn;
unsigned ppuAuditEdge, ppuAuditHoles;

// [recomp] Per-line BG layer buffers. Each BG layer's line is decoded once, before its pixels are
// composed: bgLine[layer][x + PPU_WS_MAX] is the pixel (cgram index, 0 = transparent) with the
// tile's priority in bit 15, exactly what ppu_getPixel would fetch for that layer at x. Used in the
// modes without offset-per-tile, hires or mode 7 (bgLineOn); ppu_getPixel then only looks pixels up.
static PPU_THREAD_LOCAL uint16_t bgLine[4][256 + 2 * PPU_WS_MAX];
static PPU_THREAD_LOCAL bool bgLineOn;
// ...and what ppu_getPixel finds at each x for the main screen [0] and the sub screen [1] (when the
// line uses it): the layer (0-3 BG, 4 sprites, 5 backdrop) and its pixel
static PPU_THREAD_LOCAL uint8_t resLayer[2][256 + 2 * PPU_WS_MAX];
static PPU_THREAD_LOCAL uint16_t resPix[2][256 + 2 * PPU_WS_MAX];
static bool ppu_getWindowState(Ppu* ppu, int layer, int x);

// the pixel of BG layer l at screen column x on line y, either priority (see ppu_decodeBgTile)
static int ppu_bgPixelAny(Ppu* ppu, int x, int y, int l) {
  bool outside = x < 0 || x > 255;
  if(outside && !(ppu->wsBgMask & (1 << l)) && !ppu->wsSrc[l].valid) return 0;
  int lx = x, ly = y;
  if(ppu->bgLayer[l].mosaicEnabled && ppu->mosaicSize > 1) {
    lx -= ((lx % ppu->mosaicSize) + ppu->mosaicSize) % ppu->mosaicSize;
    ly -= (ly - ppu->mosaicStartLine) % ppu->mosaicSize;
  }
  lx += ppu->bgLayer[l].hScroll;
  ly += ppu->bgLayer[l].vScroll;
  if(outside && ppu->wsSrc[l].valid) return ppu_getPixelForBgSource(ppu, x + ppu->bgLayer[l].hScroll, ly, l, 2);
  return ppu_getPixelForBgLayer(ppu, lx & 0x3ff, ly & 0x3ff, l, 2);
}

// planar to chunky: byte k of chunky[b] is bit 7 - k of b; of chunkyFlip[b], bit k
static uint64_t chunky[256], chunkyFlip[256];
static void ppu_makeChunky(void) {
  for(int b = 0; b < 256; b++) {
    uint64_t n = 0, f = 0;
    for(int k = 0; k < 8; k++) { n |= (uint64_t) ((b >> (7 - k)) & 1) << (8 * k); f |= (uint64_t) ((b >> k) & 1) << (8 * k); }
    chunky[b] = n; chunkyFlip[b] = f;
  }
}

// n pixels of a tile's row, starting at layer column lx (lx & 7 + n <= 8), as ppu_decodeBgTile works
// them out one by one (priority in bit 15); y: the layer row (vScroll applied, 10 bits)
static inline void ppu_decodeBgChunk(Ppu* ppu, int l, uint16_t tile, int lx, int y, uint16_t* out, int n) {
  const BgLayer* bg = &ppu->bgLayer[l];
  int bitDepth = bitDepthsPerMode[ppu->mode][l];
  int paletteSize = bitDepth > 4 ? 256 : bitDepth > 2 ? 16 : 4;
  int tilePrio = (tile & 0x2000) ? 0x8000 : 0;
  int paletteNum = (tile & 0x1c00) >> 10;
  int row = (tile & 0x8000) ? 7 - (y & 0x7) : (y & 0x7);
  int tileNum = tile & 0x3ff;
  if(bg->bigTiles && (((bool) (lx & 8)) ^ ((bool) (tile & 0x4000)))) tileNum += 1;
  if(bg->bigTiles && (((bool) (y & 8)) ^ ((bool) (tile & 0x8000)))) tileNum += 0x10;
  if(ppu->mode == 0) paletteNum += 8 * l;
  uint32_t planeAdr = (bg->tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + row) & 0x7fff;
  // the row's eight pixels at once: byte c of `pix` is the pixel in tile column c (bit 7 - c of each
  // plane byte, or bit c when flipped; as ppu_decodeBgTile picks them one by one)
  const uint64_t* T = (tile & 0x4000) ? chunkyFlip : chunky;
  uint32_t p1 = ppu->vram[planeAdr];
  uint64_t pix = T[p1 & 0xff] | (T[p1 >> 8] << 1);
  if(bitDepth > 2) { uint32_t p2 = ppu->vram[(planeAdr + 8) & 0x7fff]; pix |= (T[p2 & 0xff] << 2) | (T[p2 >> 8] << 3); }
  if(bitDepth > 4) {
    uint32_t p3 = ppu->vram[(planeAdr + 16) & 0x7fff], p4 = ppu->vram[(planeAdr + 24) & 0x7fff];
    pix |= (T[p3 & 0xff] << 4) | (T[p3 >> 8] << 5) | (T[p4 & 0xff] << 6) | (T[p4 >> 8] << 7);
  }
  int base = (paletteSize * paletteNum) | tilePrio;
  for(int i = 0; i < n; i++) {
    int v = (int) ((pix >> (8 * ((lx + i) & 7))) & 0xff);
    out[i] = v ? (uint16_t) (base + v) : 0;
  }
}

// The original 256 columns of a layer without mosaic, straight from VRAM, one tile row at a time:
// the same values ppu_getPixelForBgLayer gives pixel by pixel.
static void ppu_renderBgSpan(Ppu* ppu, int line, int l, uint16_t* out) {
  const BgLayer* bg = &ppu->bgLayer[l];
  int y = (line + bg->vScroll) & 0x3ff;
  int h = bg->hScroll;
  bool big = bg->bigTiles;
  int tileBitsX = big ? 4 : 3, tileHighBitX = big ? 0x200 : 0x100;
  int tileBitsY = big ? 4 : 3, tileHighBitY = big ? 0x200 : 0x100;
  uint16_t rowAdr = bg->tilemapAdr + (((y >> tileBitsY) & 0x1f) << 5);
  if((y & tileHighBitY) && bg->tilemapHigher) rowAdr += bg->tilemapWider ? 0x800 : 0x400;
  for(int x = 0; x < 256; ) {
    int lx = (x + h) & 0x3ff;
    uint16_t adr = rowAdr + ((lx >> tileBitsX) & 0x1f);
    if((lx & tileHighBitX) && bg->tilemapWider) adr += 0x400;
    int n = 8 - (lx & 7); if(n > 256 - x) n = 256 - x;
    ppu_decodeBgChunk(ppu, l, ppu->vram[adr & 0x7fff], lx, y, out + x, n);
    x += n;
  }
}

// [widescreen] Extension columns x0..x1-1 (outside 0..255) of a layer without
// mosaic, from its tile source, one tile row at a time: the values ppu_getPixelForBgSource gives.
static void ppu_renderBgSourceSpan(Ppu* ppu, int line, int l, int x0, int x1, uint16_t* out) {
  const BgLayer* bg = &ppu->bgLayer[l];
  int y = line + bg->vScroll;             // as ppu_getPixel passes it (not wrapped)
  int tileBits = bg->bigTiles ? 4 : 3, tw = 1 << tileBits;
  // the page (see ppu_getPixelForBgSource): worked out by one ordinary call, which caches it
  if(x0 < x1) (void) ppu_getPixelForBgSource(ppu, x0 + bg->hScroll, y, l, 2);
  int page = 0;
  if(!ppu->wsSrc[l].vring && ppu->wsSrc[l].refCol >= 0 && ((bg->hScroll & 0x3ff) >> tileBits) != ppu->wsSrc[l].refCol)
    page = ppu->wsPg[l];
  int w = ppu->wsSrc[l].width;
  int row = ppu->wsSrc[l].vring ? ((y & 0x3ff) >> tileBits) & 0x1f : (y >> tileBits) + ppu->wsSrc[l].rdelta;
  bool rowOk = row >= 0 && row < ppu->wsSrc[l].rows;
  const uint8_t* rowp = rowOk ? ppu->wsSrc[l].base + row * ppu->wsSrc[l].stride : NULL;
  for(int x = x0; x < x1; ) {
    int L = x + bg->hScroll;
    int n = 8 - (L & 7); if(n > x1 - x) n = x1 - x;
    if(!rowOk) { for(int i = 0; i < n; i++) out[x + i] = 0; x += n; continue; }
    int col = (L >= 0 ? L / tw : -((-L + tw - 1) / tw)) + ppu->wsSrc[l].delta + page;
    col = ((col % w) + w) % w;
    uint16_t tile = rowp[col * 2] | (rowp[col * 2 + 1] << 8);
    ppu_decodeBgChunk(ppu, l, tile, L & 0x3ff, y & 0x3ff, out + x, n);
    x += n;
  }
}

// What ppu_getPixel's search finds at each column, for the whole line in one pass: its winner is the
// first entry of the mode's priority list whose layer is active there with a visible pixel of that
// entry's priority. A column's BG pixel has one priority (its tile's) and its sprite pixel one, so
// each layer can match only one entry there; the winner is the matching entry earliest in the list.
static void ppu_resolveLine(Ppu* ppu, int ws, bool sub) {
  int actMode = ppu->mode == 1 && ppu->bg3priority ? 8 : ppu->mode;
  uint8_t rank[5][4];                      // [layer][priority]: place in the list (255: none / off)
  memset(rank, 255, sizeof rank);
  bool used[5] = {false, false, false, false, false};
  for(int i = 0; i < layerCountPerMode[actMode]; i++) {
    int L = layersPerMode[actMode][i], P = prioritysPerMode[actMode][i];
    const Layer* ly = &ppu->layer[L];
    if(!(sub ? ly->subScreenEnabled : ly->mainScreenEnabled) || ((ppu->wsDebugHide >> L) & 1)) continue;
    if(rank[L][P] == 255) rank[L][P] = (uint8_t) i;
    used[L] = true;
  }
  // windows: a layer is off where its window is on (outside 0..255: as at the nearest edge)
  static PPU_THREAD_LOCAL uint8_t win[5][256];
  bool windowed[5];
  for(int L = 0; L < 5; L++) {
    windowed[L] = used[L] && (sub ? ppu->layer[L].subScreenWindowed : ppu->layer[L].mainScreenWindowed);
    if(windowed[L]) for(int x = 0; x < 256; x++) win[L][x] = ppu_getWindowState(ppu, L, x);
  }
  const uint8_t* op = ppu->objPriorityBuffer + PPU_WS_MAX;
  const uint8_t* ox = ppu->objPixelBuffer + PPU_WS_MAX;
  uint8_t* rl = resLayer[sub] + PPU_WS_MAX;
  uint16_t* rp = resPix[sub] + PPU_WS_MAX;
  for(int x = -ws; x < 256 + ws; x++) {
    bool outside = x < 0 || x > 255;
    int wx = x < 0 ? 0 : x > 255 ? 255 : x;
    int best = 255, layer = 5, pixel = 0;
    for(int L = 0; L < 4; L++) {
      if(!used[L]) continue;
      uint16_t v = bgLine[L][x + PPU_WS_MAX];
      if(!v) continue;
      int r = rank[L][v >> 15];
      if(r >= best) continue;
      if(outside && !(ppu->wsBgMask & (1 << L)) && !ppu->wsSrc[L].valid) continue;
      if(windowed[L] && win[L][wx]) continue;
      best = r; layer = L; pixel = v & 0x7fff;
    }
    if(used[4] && ox[x]) {
      int r = rank[4][op[x] & 3];
      if(r < best && !(windowed[4] && win[4][wx])) { best = r; layer = 4; pixel = ox[x]; }
    }
    rl[x] = (uint8_t) layer; rp[x] = (uint16_t) pixel;
  }
}

static void ppu_renderBgLines(Ppu* ppu, int line, int ws) {
  bgLineOn = ppu->mode == 0 || ppu->mode == 1 || ppu->mode == 3;
  if(!bgLineOn || ppu->forcedBlank) return;
  int actMode = ppu->mode == 1 && ppu->bg3priority ? 8 : ppu->mode;
  for(int l = 0; l < 4; l++) {
    bool used = false;
    for(int i = 0; i < layerCountPerMode[actMode]; i++) used |= layersPerMode[actMode][i] == l;
    if(!used || !(ppu->layer[l].mainScreenEnabled || ppu->layer[l].subScreenEnabled)) continue;
    uint16_t* out = bgLine[l] + PPU_WS_MAX;
    if(ppu->bgLayer[l].mosaicEnabled && ppu->mosaicSize > 1) {
      for(int x = -ws; x < 256 + ws; x++) out[x] = (uint16_t) ppu_bgPixelAny(ppu, x, line, l);
      continue;
    }
    ppu_renderBgSpan(ppu, line, l, out);
    if(!ws) continue;
    // the extension: from the tile source by spans (else pixel by pixel)
    if(ppu->wsSrc[l].valid) {
      ppu_renderBgSourceSpan(ppu, line, l, -ws, 0, out);
      ppu_renderBgSourceSpan(ppu, line, l, 256, 256 + ws, out);
    } else {
      for(int x = -ws; x < 0; x++) out[x] = (uint16_t) ppu_bgPixelAny(ppu, x, line, l);
      for(int x = 256; x < 256 + ws; x++) out[x] = (uint16_t) ppu_bgPixelAny(ppu, x, line, l);
    }
  }
  ppu_resolveLine(ppu, ws, false);
  if(ppu->addSubscreen || ppu->pseudoHires) ppu_resolveLine(ppu, ws, true);
}

void ppu_runLine(Ppu* ppu, int line) {
  // called for lines 1-224/239
  tileGen++; // [recomp] new line: the tile cache starts empty
  if(ppu->lineHook) ppu->lineHook(ppu, line, ppu->lineUser); // [recomp]
  // evaluate sprites
  memset(ppu->objPixelBuffer, 0, sizeof(ppu->objPixelBuffer));
  if(!ppu->forcedBlank) ppu_evaluateSprites(ppu, line - 1);
  // actual line
  if(ppu->mode == 7) ppu_calculateMode7Starts(ppu, line);
  // [widescreen] render wsExtra extra columns on each side
  int ws = ppu->wsExtra;
  if(ws < 0) ws = 0;
  if(ws > PPU_WS_MAX) ws = PPU_WS_MAX;
  if(ppu->mode == 5 || ppu->mode == 6) ws = 0; // hires modes: not extended
  if(!ppu->forcedBlank && ppu->brightness > 0) ppu->wsInLoad = false;   // [widescreen] a picture is showing: any load is over
  if(line == 1) {
    static int dbg = -1; if(dbg < 0) dbg = getenv("POPN_LOAD_DEBUG") != NULL;
    if(dbg && ppu->wsDbgMapWrites) fprintf(stderr, "bg1 map writes %d (last at bright/fb %02x) load %d\n", ppu->wsDbgMapWrites, ppu->wsDbgBright, ppu->wsEpoch);
    ppu->wsDbgMapWrites = 0;
    ppu->wsBlank = false;
    if(ws && ppu->wsFrameHook) ppu->wsFrameHook(ppu, ppu->wsUser);
    for(int l = 0; l < 4; l++) ppu->wsPgY[l] = -100000;
  }
  // [widescreen] the camera's scroll for this picture (level edges are blacked out from it
  // once the picture is complete, in ppu_putPixelsWide)
  if(line == ppu->wsCamLine) ppu->wsEdgeScroll = ppu->bgLayer[0].hScroll;
  // [widescreen] each line's own scroll, for the next frame's source matching (scenes drawn in
  // bands that scroll separately); in-between pictures don't count
  if(!ppu->wsNoRecord && line < 240) {
    ppu->wsLineH[0][line] = ppu->bgLayer[0].hScroll;
    ppu->wsLineH[1][line] = ppu->bgLayer[1].hScroll;
    ppu->wsLineV[0][line] = ppu->bgLayer[0].vScroll;
    ppu->wsLineV[1][line] = ppu->bgLayer[1].vScroll;
  }
  int row = (line - 1) + (ppu->evenFrame ? 0 : 239);
  // clear the whole wide line (the extension stays black when it isn't drawn)
  memset(&ppu->wsBuffer[row * PPU_WS_PITCH], 0, PPU_WS_PITCH);
  if(ppu->wsBlank) ws = 0;
  ppu_renderBgLines(ppu, line, ws);
  if(bgLineOn && !ppu->forcedBlank && !ppu->wsDebugLayers) ppu_composeLine(ppu, line, ws);   // [recomp]
  else for(int x = -ws; x < 256 + ws; x++) {
    ppu_handlePixel(ppu, x, line);
  }
  // [widescreen] test statistics: sprite pixels in 16 px bands either side of the 4:3 edges
  if(ppu->wsStats && ws >= 16 && !ppu->wsNoRecord && !ppu->forcedBlank && line > 32) {
    static const int b0[4] = {-16, 0, 240, 256};
    for(int b = 0; b < 4; b++)
      for(int x = b0[b]; x < b0[b] + 16; x++) ppu->wsStatObj[b] += ppu->wsLineLayer[x + PPU_WS_MAX] == 4;
  }
  // [widescreen] holes: the backdrop showing in the extra columns while it shows nowhere in the
  // original 256 columns of this line means a layer's off-screen tiles weren't there yet (the game
  // refreshes them on the fly). Keep the previous frame's pixel there instead.
  if(ws && !ppu->forcedBlank && !ppu->wsDebugLayers) {
    int center = 0;
    for(int x = 0; x < 256 && !center; x++) center = ppu->wsLineLayer[x + PPU_WS_MAX] == 5;
    if(!center) {
      int prow = (line - 1) + (ppu->evenFrame ? 239 : 0);
      for(int x = -ws; x < 256 + ws; x++) {
        if(x == 0) x = 256;
        if(ppu->wsLineLayer[x + PPU_WS_MAX] == 5 && ppuAuditOn && !ppu->wsNoRecord) ppuAuditHoles++;
        if(ppu->wsLineLayer[x + PPU_WS_MAX] == 5)
          memcpy(&ppu->wsBuffer[row * PPU_WS_PITCH + (x + PPU_WS_MAX) * 4], &ppu->wsBuffer[prow * PPU_WS_PITCH + (x + PPU_WS_MAX) * 4], 4);
      }
    }
  }
}

int ppu_putPixelsWide(Ppu* ppu, uint8_t* pixels) {
  int ws = ppu->wsExtra < 0 ? 0 : ppu->wsExtra > PPU_WS_MAX ? PPU_WS_MAX : ppu->wsExtra;
  int width = 256 + 2 * ws;
  // [widescreen] beyond the level's own edges there is nothing to show: black, full height.
  // The edges follow the camera's line (in a scene drawn in bands, the band that moves with the
  // camera), taken relative to the scroll they were worked out for, across the 1024 px wrap
  // (an in-between picture's scroll can sit on the other side of the wrap from its frame's).
  int blackL = -ws, blackR = 256 + ws;
  if(ws && ppu->wsSrc[0].valid && ppu->wsSrc[0].clamp) {
    int edgeH = ppu->wsRefH + ((((ppu->wsEdgeScroll & 0x3ff) - ppu->wsRefH + 512) & 0x3ff) - 512);
    // a scroll far from the expected one means the camera's line was in another band this time
    if(edgeH - ppu->wsRefH > 48 || ppu->wsRefH - edgeH > 48) edgeH = ppu->wsRefH;
    blackL = ppu->wsSrc[0].lxMin - edgeH; if(blackL > 0) blackL = 0;
    blackR = ppu->wsSrc[0].lxMax - edgeH; if(blackR < 256) blackR = 256;
  }
  for(int y = 0; y < 224; y++) {
    int line = ppu->frameOverscan ? y + 8 : y; // center 224 of 239 overscan lines
    int src = line + (ppu->evenFrame ? 0 : 239);
    uint8_t* rowp = &ppu->wsBuffer[src * PPU_WS_PITCH];
    if(blackL > -ws) memset(rowp + (PPU_WS_MAX - ws) * 4, 0, (size_t)(blackL + ws) * 4);
    if(blackR < 256 + ws) memset(rowp + (PPU_WS_MAX + blackR) * 4, 0, (size_t)(256 + ws - blackR) * 4);
    memcpy(pixels + y * width * 4, &ppu->wsBuffer[src * PPU_WS_PITCH + (PPU_WS_MAX - ws) * 4], width * 4);
  }
  return width;
}

void ppu_setPixelOutputFormat(Ppu* ppu, int pixelOutputFormat) {
  ppu->pixelOutputFormat = pixelOutputFormat;
}

// [recomp] a 5-bit colour channel at each of the 16 brightness levels, as an 8-bit value
// (exactly ((v << 3) | (v >> 2)) * brightness / 15, worked out once)
static uint8_t brightTable[16][32];
static uint8_t brightFineTable[256][32];   // [recomp] the same on a 0..255 scale (255 = full); 17 * b matches b
static bool brightTableReady;
static void ppu_makeBrightTable(void) {
  for(int br = 0; br < 16; br++)
    for(int v = 0; v < 32; v++) brightTable[br][v] = (uint8_t) (((v << 3) | (v >> 2)) * br / 15);
  for(int br = 0; br < 256; br++)
    for(int v = 0; v < 32; v++) brightFineTable[br][v] = (uint8_t) (((v << 3) | (v >> 2)) * br / 255);
  brightTableReady = true;
}

// [recomp] ppu_getPixel for a line already resolved (bgLineOn): the colour of what ppu_resolveLine found
static inline int ppu_resolvedPixel(Ppu* ppu, int x, bool sub, int* r, int* g, int* b) {
  int layer = resLayer[sub][x + PPU_WS_MAX], pixel = resPix[sub][x + PPU_WS_MAX];
  int actMode = ppu->mode == 1 && ppu->bg3priority ? 8 : ppu->mode;
  if(ppu->directColor && layer < 4 && bitDepthsPerMode[actMode][layer] == 8) {
    *r = ((pixel & 0x7) << 2) | ((pixel & 0x100) >> 7);
    *g = ((pixel & 0x38) >> 1) | ((pixel & 0x200) >> 8);
    *b = ((pixel & 0xc0) >> 3) | ((pixel & 0x400) >> 8);
  } else {
    uint16_t color = ppu->cgram[pixel & 0xff];
    *r = color & 0x1f;
    *g = (color >> 5) & 0x1f;
    *b = (color >> 10) & 0x1f;
  }
  if(layer == 4 && pixel < 0xc0) layer = 6; // sprites with palette color < 0xc0
  return layer;
}

static void ppu_handlePixel(Ppu* ppu, int x, int y) {
  int r = 0, r2 = 0;
  int g = 0, g2 = 0;
  int b = 0, b2 = 0;
  if(!ppu->forcedBlank) {
    int mainLayer = bgLineOn ? ppu_resolvedPixel(ppu, x, false, &r, &g, &b) : ppu_getPixel(ppu, x, y, false, &r, &g, &b);
    ppu->wsLineLayer[x + PPU_WS_MAX] = (uint8_t)mainLayer;
    if(ppu->wsDebugLayers && (x < 0 || x > 255)) {   // [widescreen] debug: color by source layer
      static const uint8_t dc[7][3] = {{31,0,0},{0,31,0},{0,0,31},{31,31,0},{31,31,31},{31,0,31},{31,31,31}};
      r = r2 = dc[mainLayer][0]; g = g2 = dc[mainLayer][1]; b = b2 = dc[mainLayer][2];
      goto ws_store;
    }
    // [widescreen] windows outside 0..255 keep the state of the nearest edge
    bool colorWindowState = ppu_getWindowState(ppu, 5, x < 0 ? 0 : x > 255 ? 255 : x);
    if(
      ppu->clipMode == 3 ||
      (ppu->clipMode == 2 && colorWindowState) ||
      (ppu->clipMode == 1 && !colorWindowState)
    ) {
      r = 0;
      g = 0;
      b = 0;
    }
    int secondLayer = 5; // backdrop
    bool mathEnabled = mainLayer < 6 && ppu->mathEnabled[mainLayer] && !(
      ppu->preventMathMode == 3 ||
      (ppu->preventMathMode == 2 && colorWindowState) ||
      (ppu->preventMathMode == 1 && !colorWindowState)
    );
    if((mathEnabled && ppu->addSubscreen) || ppu->pseudoHires || ppu->mode == 5 || ppu->mode == 6) {
      secondLayer = bgLineOn ? ppu_resolvedPixel(ppu, x, true, &r2, &g2, &b2) : ppu_getPixel(ppu, x, y, true, &r2, &g2, &b2);
    }
    if(ppuAuditOn && (x == 0 || x == 255) && y > 48 && !ppu->wsNoRecord) {   // [widescreen] test audit
      if(mainLayer < 4) ppuAuditEdge |= 1u << mainLayer;
      if(secondLayer < 4 && mathEnabled) ppuAuditEdge |= 0x100u << secondLayer;
    }
    // TODO: subscreen pixels can be clipped to black as well
    // TODO: math for subscreen pixels (add/sub sub to main)
    if(mathEnabled) {
      if(ppu->subtractColor) {
        r -= (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g -= (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b -= (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      } else {
        r += (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g += (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b += (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      }
      if(ppu->halfColor && (secondLayer != 5 || !ppu->addSubscreen)) {
        r >>= 1;
        g >>= 1;
        b >>= 1;
      }
      if(r > 31) r = 31;
      if(g > 31) g = 31;
      if(b > 31) b = 31;
      if(r < 0) r = 0;
      if(g < 0) g = 0;
      if(b < 0) b = 0;
    }
    if(!(ppu->pseudoHires || ppu->mode == 5 || ppu->mode == 6)) {
      r2 = r; g2 = g; b2 = b;
    }
  }
ws_store:;
  int row = (y - 1) + (ppu->evenFrame ? 0 : 239);
  // [widescreen] one pixel per dot for the wide output
  uint8_t* ws = &ppu->wsBuffer[row * PPU_WS_PITCH + (x + PPU_WS_MAX) * 4];
  if(!brightTableReady) ppu_makeBrightTable();
  const uint8_t* bt = ppu->wsBrightFine >= 0 ? brightFineTable[ppu->wsBrightFine & 255] : brightTable[ppu->brightness & 15];
  int fmt = ppu->pixelOutputFormat;
  uint8_t ob = bt[b & 31], og = bt[g & 31], or_ = bt[r & 31];
  ws[0 + fmt] = ob;
  ws[1 + fmt] = og;
  ws[2 + fmt] = or_;
  if(x < 0 || x > 255 || ppu->noPixelBuffer) return;
  uint8_t* pb = &ppu->pixelBuffer[row * 2048 + x * 8 + fmt];
  pb[0] = bt[b2 & 31];
  pb[1] = bt[g2 & 31];
  pb[2] = bt[r2 & 31];
  pb[4] = ob;
  pb[5] = og;
  pb[6] = or_;
}

// [recomp] ppu_handlePixel for every column of a resolved line (bgLineOn, not forced blank, no
// debug colouring): the same arithmetic, with what stays the same along a line worked out once
static void ppu_composeLine(Ppu* ppu, int y, int ws) {
  if(!brightTableReady) ppu_makeBrightTable();
  const uint8_t* bt = ppu->wsBrightFine >= 0 ? brightFineTable[ppu->wsBrightFine & 255] : brightTable[ppu->brightness & 15];
  int fmt = ppu->pixelOutputFormat;
  int row = (y - 1) + (ppu->evenFrame ? 0 : 239);
  uint8_t* wsRow = &ppu->wsBuffer[row * PPU_WS_PITCH];
  uint8_t* pbRow = &ppu->pixelBuffer[row * 2048];
  bool colorWindowOn = ppu->windowLayer[5].window1enabled || ppu->windowLayer[5].window2enabled;
  bool hires = ppu->pseudoHires;          // (modes 5/6 never come here)
  bool audit = ppuAuditOn && y > 48 && !ppu->wsNoRecord;
  bool noPb = ppu->noPixelBuffer;
  for(int x = -ws; x < 256 + ws; x++) {
    int r, g, b, r2 = 0, g2 = 0, b2 = 0;
    int mainLayer = ppu_resolvedPixel(ppu, x, false, &r, &g, &b);
    ppu->wsLineLayer[x + PPU_WS_MAX] = (uint8_t) mainLayer;
    bool colorWindowState = colorWindowOn && ppu_getWindowState(ppu, 5, x < 0 ? 0 : x > 255 ? 255 : x);
    if(
      ppu->clipMode == 3 ||
      (ppu->clipMode == 2 && colorWindowState) ||
      (ppu->clipMode == 1 && !colorWindowState)
    ) {
      r = 0;
      g = 0;
      b = 0;
    }
    int secondLayer = 5; // backdrop
    bool mathEnabled = mainLayer < 6 && ppu->mathEnabled[mainLayer] && !(
      ppu->preventMathMode == 3 ||
      (ppu->preventMathMode == 2 && colorWindowState) ||
      (ppu->preventMathMode == 1 && !colorWindowState)
    );
    if((mathEnabled && ppu->addSubscreen) || hires) {
      secondLayer = ppu_resolvedPixel(ppu, x, true, &r2, &g2, &b2);
    }
    if(audit && (x == 0 || x == 255)) {   // [widescreen] test audit
      if(mainLayer < 4) ppuAuditEdge |= 1u << mainLayer;
      if(secondLayer < 4 && mathEnabled) ppuAuditEdge |= 0x100u << secondLayer;
    }
    if(mathEnabled) {
      if(ppu->subtractColor) {
        r -= (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g -= (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b -= (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      } else {
        r += (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g += (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b += (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      }
      if(ppu->halfColor && (secondLayer != 5 || !ppu->addSubscreen)) {
        r >>= 1;
        g >>= 1;
        b >>= 1;
      }
      if(r > 31) r = 31;
      if(g > 31) g = 31;
      if(b > 31) b = 31;
      if(r < 0) r = 0;
      if(g < 0) g = 0;
      if(b < 0) b = 0;
    }
    if(!hires) {
      r2 = r; g2 = g; b2 = b;
    }
    uint8_t ob = bt[b & 31], og = bt[g & 31], or_ = bt[r & 31];
    uint8_t* ws4 = wsRow + (x + PPU_WS_MAX) * 4 + fmt;
    ws4[0] = ob;
    ws4[1] = og;
    ws4[2] = or_;
    if(x < 0 || x > 255 || noPb) continue;
    uint8_t* pb = pbRow + x * 8 + fmt;
    pb[0] = bt[b2 & 31];
    pb[1] = bt[g2 & 31];
    pb[2] = bt[r2 & 31];
    pb[4] = ob;
    pb[5] = og;
    pb[6] = or_;
  }
}

static int ppu_getPixel(Ppu* ppu, int x, int y, bool sub, int* r, int* g, int* b) {
  // figure out which color is on this location on main- or subscreen, sets it in r, g, b
  // returns which layer it is: 0-3 for bg layer, 4 or 6 for sprites (depending on palette), 5 for backdrop
  int actMode = ppu->mode == 1 && ppu->bg3priority ? 8 : ppu->mode;
  actMode = ppu->mode == 7 && ppu->m7extBg ? 9 : actMode;
  int layer = 5;
  int pixel = 0;
  bool outside = x < 0 || x > 255; // [widescreen]
  int wx = x < 0 ? 0 : x > 255 ? 255 : x;
  if(bgLineOn) {   // [recomp] found for the whole line already (ppu_resolveLine)
    layer = resLayer[sub][x + PPU_WS_MAX]; pixel = resPix[sub][x + PPU_WS_MAX];
  } else
  for(int i = 0; i < layerCountPerMode[actMode]; i++) {
    int curLayer = layersPerMode[actMode][i];
    int curPriority = prioritysPerMode[actMode][i];
    bool layerActive = false;
    if(!sub) {
      layerActive = ppu->layer[curLayer].mainScreenEnabled && (
        !ppu->layer[curLayer].mainScreenWindowed || !ppu_getWindowState(ppu, curLayer, wx)
      );
    } else {
      layerActive = ppu->layer[curLayer].subScreenEnabled && (
        !ppu->layer[curLayer].subScreenWindowed || !ppu_getWindowState(ppu, curLayer, wx)
      );
    }
    // [widescreen] layers not allowed to extend are transparent outside 0..255
    if(outside && curLayer < 4 && !(ppu->wsBgMask & (1 << curLayer)) && !ppu->wsSrc[curLayer].valid) layerActive = false;
    if((ppu->wsDebugHide >> curLayer) & 1) layerActive = false;   // [widescreen] debug: layer hidden (bit 4 = sprites)
    if(layerActive) {
      if(curLayer < 4 && bgLineOn) {
        // [recomp] bg layer: from this line's buffer (ppu_renderBgLines)
        uint16_t v = bgLine[curLayer][x + PPU_WS_MAX];
        pixel = (v && (v >> 15) == curPriority) ? (v & 0x7fff) : 0;
      } else if(curLayer < 4) {
        // bg layer
        int lx = x;
        int ly = y;
        if(ppu->bgLayer[curLayer].mosaicEnabled && ppu->mosaicSize > 1) {
          lx -= ((lx % ppu->mosaicSize) + ppu->mosaicSize) % ppu->mosaicSize; // [widescreen] x may be negative
          ly -= (ly - ppu->mosaicStartLine) % ppu->mosaicSize;
        }
        if(ppu->mode == 7) {
          pixel = ppu_getPixelForMode7(ppu, lx, curLayer, curPriority);
        } else {
          lx += ppu->bgLayer[curLayer].hScroll;
          if(ppu->mode == 5 || ppu->mode == 6) {
            lx *= 2;
            lx += (sub || ppu->bgLayer[curLayer].mosaicEnabled) ? 0 : 1;
            if(ppu->interlace) {
              ly *= 2;
              ly += (ppu->evenFrame || ppu->bgLayer[curLayer].mosaicEnabled) ? 0 : 1;
            }
          }
          ly += ppu->bgLayer[curLayer].vScroll;
          if(ppu->mode == 2 || ppu->mode == 4 || ppu->mode == 6) {
            ppu_handleOPT(ppu, curLayer, &lx, &ly);
          }
          if(outside && ppu->wsSrc[curLayer].valid && ppu->mode != 5 && ppu->mode != 6) {
            // [widescreen] extension column comes from the host-provided tile source
            pixel = ppu_getPixelForBgSource(ppu, x + ppu->bgLayer[curLayer].hScroll, ly, curLayer, curPriority);
          } else {
            pixel = ppu_getPixelForBgLayer(
              ppu, lx & 0x3ff, ly & 0x3ff,
              curLayer, curPriority
            );
          }
        }
      } else {
        // get a pixel from the sprite buffer
        pixel = 0;
        if(ppu->objPriorityBuffer[x + PPU_WS_MAX] == curPriority) pixel = ppu->objPixelBuffer[x + PPU_WS_MAX];
      }
    }
    if(pixel > 0) {
      layer = curLayer;
      break;
    }
  }
  if(ppu->directColor && layer < 4 && bitDepthsPerMode[actMode][layer] == 8) {
    *r = ((pixel & 0x7) << 2) | ((pixel & 0x100) >> 7);
    *g = ((pixel & 0x38) >> 1) | ((pixel & 0x200) >> 8);
    *b = ((pixel & 0xc0) >> 3) | ((pixel & 0x400) >> 8);
  } else {
    uint16_t color = ppu->cgram[pixel & 0xff];
    *r = color & 0x1f;
    *g = (color >> 5) & 0x1f;
    *b = (color >> 10) & 0x1f;
  }
  if(layer == 4 && pixel < 0xc0) layer = 6; // sprites with palette color < 0xc0
  return layer;
}

static void ppu_handleOPT(Ppu* ppu, int layer, int* lx, int* ly) {
  int x = *lx;
  int y = *ly;
  int column = 0;
  if(ppu->mode == 6) {
    column = ((x - (x & 0xf)) - ((ppu->bgLayer[layer].hScroll * 2) & 0xfff0)) >> 4;
  } else {
    column = ((x - (x & 0x7)) - (ppu->bgLayer[layer].hScroll & 0xfff8)) >> 3;
  }
  if(column > 0) {
    // fetch offset values from layer 3 tilemap
    int valid = layer == 0 ? 0x2000 : 0x4000;
    uint16_t hOffset = ppu_getOffsetValue(ppu, column - 1, 0);
    uint16_t vOffset = 0;
    if(ppu->mode == 4) {
      if(hOffset & 0x8000) {
        vOffset = hOffset;
        hOffset = 0;
      }
    } else {
      vOffset = ppu_getOffsetValue(ppu, column - 1, 1);
    }
    if(ppu->mode == 6) {
      // TODO: not sure if correct
      if(hOffset & valid) *lx = (((hOffset & 0x3f8) + (column * 8)) * 2) | (x & 0xf);
    } else {
      if(hOffset & valid) *lx = ((hOffset & 0x3f8) + (column * 8)) | (x & 0x7);
    }
    // TODO: not sure if correct for interlace
    if(vOffset & valid) *ly = (vOffset & 0x3ff) + (y - ppu->bgLayer[layer].vScroll);
  }
}

static uint16_t ppu_getOffsetValue(Ppu* ppu, int col, int row) {
  int x = col * 8 + ppu->bgLayer[2].hScroll;
  int y = row * 8 + ppu->bgLayer[2].vScroll;
  int tileBits = ppu->bgLayer[2].bigTiles ? 4 : 3;
  int tileHighBit = ppu->bgLayer[2].bigTiles ? 0x200 : 0x100;
  uint16_t tilemapAdr = ppu->bgLayer[2].tilemapAdr + (((y >> tileBits) & 0x1f) << 5 | ((x >> tileBits) & 0x1f));
  if((x & tileHighBit) && ppu->bgLayer[2].tilemapWider) tilemapAdr += 0x400;
  if((y & tileHighBit) && ppu->bgLayer[2].tilemapHigher) tilemapAdr += ppu->bgLayer[2].tilemapWider ? 0x800 : 0x400;
  return ppu->vram[tilemapAdr & 0x7fff];
}

static int ppu_getPixelForBgLayer(Ppu* ppu, int x, int y, int layer, int priority) {
  // figure out address of tilemap word and read it
  bool wideTiles = ppu->bgLayer[layer].bigTiles || ppu->mode == 5 || ppu->mode == 6;
  int tileBitsX = wideTiles ? 4 : 3;
  int tileHighBitX = wideTiles ? 0x200 : 0x100;
  int tileBitsY = ppu->bgLayer[layer].bigTiles ? 4 : 3;
  int tileHighBitY = ppu->bgLayer[layer].bigTiles ? 0x200 : 0x100;
  uint16_t tilemapAdr = ppu->bgLayer[layer].tilemapAdr + (((y >> tileBitsY) & 0x1f) << 5 | ((x >> tileBitsX) & 0x1f));
  if((x & tileHighBitX) && ppu->bgLayer[layer].tilemapWider) tilemapAdr += 0x400;
  if((y & tileHighBitY) && ppu->bgLayer[layer].tilemapHigher) tilemapAdr += ppu->bgLayer[layer].tilemapWider ? 0x800 : 0x400;
  uint16_t tile = ppu->vram[tilemapAdr & 0x7fff];
  return ppu_decodeBgTile(ppu, tile, x, y, layer, priority);
}

// [widescreen] fetch the tile word from wsSrc instead of VRAM; x is unwrapped (may be < 0 or > 1023)
static int ppu_getPixelForBgSource(Ppu* ppu, int x, int y, int layer, int priority) {
  int tileBits = ppu->bgLayer[layer].bigTiles ? 4 : 3;
  int tw = 1 << tileBits;
  // a band of lines scrolled elsewhere than where the source was matched (a scene drawn in bands
  // with their own scroll) shows the same 32-column tilemap, whose columns can sit a whole tilemap
  // width further along the level map: which page is found by comparing the line's visible tiles
  int page = 0;
  if(!ppu->wsSrc[layer].vring && ppu->wsSrc[layer].refCol >= 0) {
    int h = ppu->bgLayer[layer].hScroll & 0x3ff;
    int c0 = h >> tileBits, ref = ppu->wsSrc[layer].refCol;
    if(c0 != ref) {
      if(ppu->wsPgY[layer] == y && ppu->wsPgH[layer] == h) page = ppu->wsPg[layer];
      else {
        int guess = ref + (((c0 - ref + 16) & 31) - 16) - c0, best = -1;
        int w = ppu->wsSrc[layer].width, r = (y >> tileBits) + ppu->wsSrc[layer].rdelta;
        page = guess;
        if(r >= 0 && r < ppu->wsSrc[layer].rows) {
          const uint8_t* rowp = ppu->wsSrc[layer].base + r * ppu->wsSrc[layer].stride;
          uint16_t mapRow = ppu->bgLayer[layer].tilemapAdr + (((y >> tileBits) & 0x1f) << 5);
          for(int i = 0; i < 3; i++) {
            int cand = guess + (i == 0 ? 0 : i == 1 ? -32 : 32), score = 0;
            for(int k = 0; k < 17; k++) {
              int col = (((c0 + k + ppu->wsSrc[layer].delta + cand) % w) + w) % w;
              score += (uint16_t)(rowp[col * 2] | (rowp[col * 2 + 1] << 8)) == ppu->vram[(mapRow + ((c0 + k) & 0x1f)) & 0x7fff];
            }
            static int pagesDbg = -1; if(pagesDbg < 0) pagesDbg = getenv("POPN_WS_PAGES") != NULL;   // debug
            if(pagesDbg) fprintf(stderr, "page L%d y%d h%d c0 %d ref %d row %d cand %d score %d\n", layer, y, h, c0, ref, r, cand, score);
            if(score > best) { best = score; page = cand; }
          }
        }
        ppu->wsPgY[layer] = y; ppu->wsPgH[layer] = h; ppu->wsPg[layer] = page;
      }
    }
  }
  int col = (x >= 0 ? x / tw : -((-x + tw - 1) / tw)) + ppu->wsSrc[layer].delta + page;
  int w = ppu->wsSrc[layer].width;
  col = ((col % w) + w) % w;
  int row = ppu->wsSrc[layer].vring ? ((y & 0x3ff) >> tileBits) & 0x1f : (y >> tileBits) + ppu->wsSrc[layer].rdelta;
  if(row < 0 || row >= ppu->wsSrc[layer].rows) return 0;
  const uint8_t* p = ppu->wsSrc[layer].base + row * ppu->wsSrc[layer].stride + col * 2;
  uint16_t tile = p[0] | (p[1] << 8);
  return ppu_decodeBgTile(ppu, tile, x & 0x3ff, y & 0x3ff, layer, priority);
}

static int ppu_decodeBgTile(Ppu* ppu, uint16_t tile, int x, int y, int layer, int priority) {
  bool wideTiles = ppu->bgLayer[layer].bigTiles || ppu->mode == 5 || ppu->mode == 6;
  // check priority, get palette ([recomp] priority 2: either; the tile's priority is then returned
  // in bit 15 of a visible pixel, for the per-line layer buffers)
  int tilePrio = (tile & 0x2000) ? 1 : 0;
  if(priority < 2 && tilePrio != priority) return 0; // wrong priority
  int paletteNum = (tile & 0x1c00) >> 10;
  // figure out position within tile
  int row = (tile & 0x8000) ? 7 - (y & 0x7) : (y & 0x7);
  int col = (tile & 0x4000) ? (x & 0x7) : 7 - (x & 0x7);
  int tileNum = tile & 0x3ff;
  if(wideTiles) {
    // if unflipped right half of tile, or flipped left half of tile
    if(((bool) (x & 8)) ^ ((bool) (tile & 0x4000))) tileNum += 1;
  }
  if(ppu->bgLayer[layer].bigTiles) {
    // if unflipped bottom half of tile, or flipped upper half of tile
    if(((bool) (y & 8)) ^ ((bool) (tile & 0x8000))) tileNum += 0x10;
  }
  // read tiledata, ajust palette for mode 0
  int bitDepth = bitDepthsPerMode[ppu->mode][layer];
  if(ppu->mode == 0) paletteNum += 8 * layer;
  int paletteSize = bitDepth > 4 ? 256 : bitDepth > 2 ? 16 : 4;
  // [recomp] the row's eight pixels, from the cache when this line already decoded this row
  uint32_t planeAdr = (ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + row) & 0x7fff;
  uint32_t key = planeAdr | ((uint32_t) bitDepth << 16);
  uint8_t* px = tileCache[layer].px;
  if(tileCache[layer].gen != tileGen || tileCache[layer].key != key) {
    tileCache[layer].gen = tileGen; tileCache[layer].key = key;
    uint16_t plane1 = ppu->vram[planeAdr];
    uint16_t plane2 = bitDepth > 2 ? ppu->vram[(planeAdr + 8) & 0x7fff] : 0;
    uint16_t plane3 = bitDepth > 4 ? ppu->vram[(planeAdr + 16) & 0x7fff] : 0;
    uint16_t plane4 = bitDepth > 4 ? ppu->vram[(planeAdr + 24) & 0x7fff] : 0;
    for(int c = 0; c < 8; c++) {
      int v = ((plane1 >> c) & 1) | (((plane1 >> (8 + c)) & 1) << 1)
            | (((plane2 >> c) & 1) << 2) | (((plane2 >> (8 + c)) & 1) << 3)
            | (((plane3 >> c) & 1) << 4) | (((plane3 >> (8 + c)) & 1) << 5)
            | (((plane4 >> c) & 1) << 6) | (((plane4 >> (8 + c)) & 1) << 7);
      px[c] = (uint8_t) v;
    }
  }
  int pixel = px[col];
  // return cgram index, or 0 if transparent, palette number in bits 10-8 for 8-color layers
  if(pixel == 0) return 0;
  return (paletteSize * paletteNum + pixel) | (priority == 2 && tilePrio ? 0x8000 : 0);
}

static void ppu_calculateMode7Starts(Ppu* ppu, int y) {
  // expand 13-bit values to signed values
  int hScroll = ((int16_t) (ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t) (ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t) (ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t) (ppu->m7matrix[5] << 3)) >> 3;
  // do calculation
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  if(ppu->bgLayer[0].mosaicEnabled && ppu->mosaicSize > 1) {
    y -= (y - ppu->mosaicStartLine) % ppu->mosaicSize;
  }
  uint8_t ry = ppu->m7yFlip ? 255 - y : y;
  ppu->m7startX = (
    ((ppu->m7matrix[0] * clippedH) & ~63) +
    ((ppu->m7matrix[1] * ry) & ~63) +
    ((ppu->m7matrix[1] * clippedV) & ~63) +
    (xCenter << 8)
  );
  ppu->m7startY = (
    ((ppu->m7matrix[2] * clippedH) & ~63) +
    ((ppu->m7matrix[3] * ry) & ~63) +
    ((ppu->m7matrix[3] * clippedV) & ~63) +
    (yCenter << 8)
  );
}

static int ppu_getPixelForMode7(Ppu* ppu, int x, int layer, bool priority) {
  int rx = ppu->m7xFlip ? 255 - x : x; // [widescreen] int: x may be outside 0..255
  int xPos = (ppu->m7startX + ppu->m7matrix[0] * rx) >> 8;
  int yPos = (ppu->m7startY + ppu->m7matrix[2] * rx) >> 8;
  bool outsideMap = xPos < 0 || xPos >= 1024 || yPos < 0 || yPos >= 1024;
  xPos &= 0x3ff;
  yPos &= 0x3ff;
  if(!ppu->m7largeField) outsideMap = false;
  uint8_t tile = outsideMap ? 0 : ppu->vram[(yPos >> 3) * 128 + (xPos >> 3)] & 0xff;
  uint8_t pixel = outsideMap && !ppu->m7charFill ? 0 : ppu->vram[tile * 64 + (yPos & 7) * 8 + (xPos & 7)] >> 8;
  if(layer == 1) {
    if(((bool) (pixel & 0x80)) != priority) return 0;
    return pixel & 0x7f;
  }
  return pixel;
}

static bool ppu_getWindowState(Ppu* ppu, int layer, int x) {
  if(!ppu->windowLayer[layer].window1enabled && !ppu->windowLayer[layer].window2enabled) {
    return false;
  }
  if(ppu->windowLayer[layer].window1enabled && !ppu->windowLayer[layer].window2enabled) {
    bool test = x >= ppu->window1left && x <= ppu->window1right;
    return ppu->windowLayer[layer].window1inversed ? !test : test;
  }
  if(!ppu->windowLayer[layer].window1enabled && ppu->windowLayer[layer].window2enabled) {
    bool test = x >= ppu->window2left && x <= ppu->window2right;
    return ppu->windowLayer[layer].window2inversed ? !test : test;
  }
  bool test1 = x >= ppu->window1left && x <= ppu->window1right;
  bool test2 = x >= ppu->window2left && x <= ppu->window2right;
  if(ppu->windowLayer[layer].window1inversed) test1 = !test1;
  if(ppu->windowLayer[layer].window2inversed) test2 = !test2;
  switch(ppu->windowLayer[layer].maskLogic) {
    case 0: return test1 || test2;
    case 1: return test1 && test2;
    case 2: return test1 != test2;
    case 3: return test1 == test2;
  }
  return false;
}

static void ppu_evaluateSprites(Ppu* ppu, int line) {
  // TODO: rectangular sprites, wierdness with sprites at -256
  uint8_t index = ppu->objPriority ? (ppu->oamAdr & 0xfe) : 0;
  int spritesFound = 0;     // sprites counted against the 32-per-line limit (original 256 columns)
  int listed = 0;           // [widescreen] all sprites to draw, including ones only in the extension
  int tilesFound = 0;
  bool over = false;
  uint8_t foundSprites[128] = {};
  // [widescreen] with extra columns, X values 256..383 mean "right of the screen" rather than
  // wrapping to the far left, so objects just past the right edge appear there.
  int ws = (ppu->mode == 5 || ppu->mode == 6) ? 0 : ppu->wsExtra;
  if(ws < 0) ws = 0;
  if(ws > PPU_WS_MAX) ws = PPU_WS_MAX;
  int wrapAt = ws ? 384 : 256;
  // iterate over oam to find sprites in range
  for(int i = 0; i < 128; i++) {
    uint8_t y = ppu->oam[index] >> 8;
    // check if the sprite is on this line and get the sprite size
    uint8_t row = line - y;
    int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int spriteHeight = ppu->objInterlace ? spriteSize / 2 : spriteSize;
    if(row < spriteHeight) {
      // in y-range, get the x location, using the high bit as well
      int x = ppu->oam[index] & 0xff;
      x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
      if(x >= wrapAt) x -= 512;
      bool inOriginal = x > -spriteSize && x < 256;
      bool inWide = x > -spriteSize - ws && x < 256 + ws;
      if(inOriginal) {
        // break if we found 32 sprites already
        spritesFound++;
        if(spritesFound > 32) {
          ppu->rangeOver = true;
          over = true;
          if(!ppu->noSpriteLimit) {
            spritesFound = 32;
            break;
          }
        }
      }
      if(inWide) foundSprites[listed++] = index;
    }
    index += 2;
  }
  // iterate over found sprites backwards to fetch max 34 tile slivers
  for(int i = listed; i > 0; i--) {
    index = foundSprites[i - 1];
    uint8_t y = ppu->oam[index] >> 8;
    uint8_t row = line - y;
    int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int x = ppu->oam[index] & 0xff;
    x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if(x >= wrapAt) x -= 512;
    if(x > -spriteSize - ws) {
      // update row according to obj-interlace
      if(ppu->objInterlace) row = row * 2 + (ppu->evenFrame ? 0 : 1);
      // get some data for the sprite and y-flip row if needed
      int tile = ppu->oam[index + 1] & 0xff;
      int palette = (ppu->oam[index + 1] & 0xe00) >> 9;
      bool hFlipped = ppu->oam[index + 1] & 0x4000;
      if(ppu->oam[index + 1] & 0x8000) row = spriteSize - 1 - row;
      // fetch all tiles in x-range
      for(int col = 0; col < spriteSize; col += 8) {
        if(col + x > -8 - ws && col + x < 256 + ws) {
          // break if we found > 34 8*1 slivers already (only slivers in the original 256 count)
          if(col + x > -8 && col + x < 256) {
            tilesFound++;
            if(tilesFound > 34) {
              ppu->timeOver = true;
              over = true;
              if(!ppu->noSpriteLimit) break;
            }
          }
          // figure out which tile this uses, looping within 16x16 pages, and get it's data
          int usedCol = hFlipped ? spriteSize - 1 - col : col;
          uint8_t usedTile = (((tile >> 4) + (row / 8)) << 4) | (((tile & 0xf) + (usedCol / 8)) & 0xf);
          uint16_t objAdr = (ppu->oam[index + 1] & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
          uint16_t plane1 = ppu->vram[(objAdr + usedTile * 16 + (row & 0x7)) & 0x7fff];
          uint16_t plane2 = ppu->vram[(objAdr + usedTile * 16 + 8 + (row & 0x7)) & 0x7fff];
          // go over each pixel
          for(int px = 0; px < 8; px++) {
            int shift = hFlipped ? px : 7 - px;
            int pixel = (plane1 >> shift) & 1;
            pixel |= ((plane1 >> (8 + shift)) & 1) << 1;
            pixel |= ((plane2 >> shift) & 1) << 2;
            pixel |= ((plane2 >> (8 + shift)) & 1) << 3;
            // draw it in the buffer if there is a pixel here
            int screenCol = col + x + px;
            if(pixel > 0 && screenCol >= -ws && screenCol < 256 + ws) {
              ppu->objPixelBuffer[screenCol + PPU_WS_MAX] = 0x80 + 16 * palette + pixel;
              ppu->objPriorityBuffer[screenCol + PPU_WS_MAX] = (ppu->oam[index + 1] & 0x3000) >> 12;
            }
          }
        }
      }
      if(tilesFound > 34 && !ppu->noSpriteLimit) break; // break out of sprite-loop if max tiles found
    }
  }
  if(over) ppu->spriteOverflowLines++;
  if(spritesFound > ppu->spriteMaxOnLine) ppu->spriteMaxOnLine = spritesFound;
  if(tilesFound > ppu->sliverMaxOnLine) ppu->sliverMaxOnLine = tilesFound;
}

static uint16_t ppu_getVramRemap(Ppu* ppu) {
  uint16_t adr = ppu->vramPointer;
  switch(ppu->vramRemapMode) {
    case 0: return adr;
    case 1: return (adr & 0xff00) | ((adr & 0xe0) >> 5) | ((adr & 0x1f) << 3);
    case 2: return (adr & 0xfe00) | ((adr & 0x1c0) >> 6) | ((adr & 0x3f) << 3);
    case 3: return (adr & 0xfc00) | ((adr & 0x380) >> 7) | ((adr & 0x7f) << 3);
  }
  return adr;
}

uint8_t ppu_read(Ppu* ppu, uint8_t adr) {
  switch(adr) {
    case 0x04: case 0x14: case 0x24:
    case 0x05: case 0x15: case 0x25:
    case 0x06: case 0x16: case 0x26:
    case 0x08: case 0x18: case 0x28:
    case 0x09: case 0x19: case 0x29:
    case 0x0a: case 0x1a: case 0x2a: {
      return ppu->ppu1openBus;
    }
    case 0x34:
    case 0x35:
    case 0x36: {
      int result = ppu->m7matrix[0] * (ppu->m7matrix[1] >> 8);
      ppu->ppu1openBus = (result >> (8 * (adr - 0x34))) & 0xff;
      return ppu->ppu1openBus;
    }
    case 0x37: {
      // TODO: only when ppulatch is set
      ppu->hCount = ppu->snes->hPos / 4;
      ppu->vCount = ppu->snes->vPos;
      ppu->countersLatched = true;
      return ppu->snes->openBus;
    }
    case 0x38: {
      uint8_t ret = 0;
      if(ppu->oamInHigh) {
        ret = ppu->highOam[((ppu->oamAdr & 0xf) << 1) | ppu->oamSecondWrite];
        if(ppu->oamSecondWrite) {
          ppu->oamAdr++;
          if(ppu->oamAdr == 0) ppu->oamInHigh = false;
        }
      } else {
        if(!ppu->oamSecondWrite) {
          ret = ppu->oam[ppu->oamAdr] & 0xff;
        } else {
          ret = ppu->oam[ppu->oamAdr++] >> 8;
          if(ppu->oamAdr == 0) ppu->oamInHigh = true;
        }
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      ppu->ppu1openBus = ret;
      return ret;
    }
    case 0x39: {
      uint16_t val = ppu->vramReadBuffer;
      if(!ppu->vramIncrementOnHigh) {
        ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
        ppu->vramPointer += ppu->vramIncrement;
      }
      ppu->ppu1openBus = val & 0xff;
      return val & 0xff;
    }
    case 0x3a: {
      uint16_t val = ppu->vramReadBuffer;
      if(ppu->vramIncrementOnHigh) {
        ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
        ppu->vramPointer += ppu->vramIncrement;
      }
      ppu->ppu1openBus = val >> 8;
      return val >> 8;
    }
    case 0x3b: {
      uint8_t ret = 0;
      if(!ppu->cgramSecondWrite) {
        ret = ppu->cgram[ppu->cgramPointer] & 0xff;
      } else {
        ret = ((ppu->cgram[ppu->cgramPointer++] >> 8) & 0x7f) | (ppu->ppu2openBus & 0x80);
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      ppu->ppu2openBus = ret;
      return ret;
    }
    case 0x3c: {
      uint8_t val = 0;
      if(ppu->hCountSecond) {
        val = ((ppu->hCount >> 8) & 1) | (ppu->ppu2openBus & 0xfe);
      } else {
        val = ppu->hCount & 0xff;
      }
      ppu->hCountSecond = !ppu->hCountSecond;
      ppu->ppu2openBus = val;
      return val;
    }
    case 0x3d: {
      uint8_t val = 0;
      if(ppu->vCountSecond) {
        val = ((ppu->vCount >> 8) & 1) | (ppu->ppu2openBus & 0xfe);
      } else {
        val = ppu->vCount & 0xff;
      }
      ppu->vCountSecond = !ppu->vCountSecond;
      ppu->ppu2openBus = val;
      return val;
    }
    case 0x3e: {
      uint8_t val = 0x1; // ppu1 version (4 bit)
      val |= ppu->ppu1openBus & 0x10;
      val |= ppu->rangeOver << 6;
      val |= ppu->timeOver << 7;
      ppu->ppu1openBus = val;
      return val;
    }
    case 0x3f: {
      uint8_t val = 0x3; // ppu2 version (4 bit)
      val |= ppu->snes->palTiming << 4; // ntsc/pal
      val |= ppu->ppu2openBus & 0x20;
      val |= ppu->countersLatched << 6;
      val |= ppu->evenFrame << 7;
      ppu->countersLatched = false; // TODO: only when ppulatch is set
      ppu->hCountSecond = false;
      ppu->vCountSecond = false;
      ppu->ppu2openBus = val;
      return val;
    }
    default: {
      return ppu->snes->openBus;
    }
  }
}

void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val) {
  switch(adr) {
    case 0x00: {
      // TODO: oam address reset when written on first line of vblank, (and when forced blank is disabled?)
      ppu->brightness = val & 0xf;
      ppu->forcedBlank = val & 0x80;
      break;
    }
    case 0x01: {
      ppu->objSize = val >> 5;
      ppu->objTileAdr1 = (val & 7) << 13;
      ppu->objTileAdr2 = ppu->objTileAdr1 + (((val & 0x18) + 8) << 9);
      break;
    }
    case 0x02: {
      ppu->oamAdr = val;
      ppu->oamAdrWritten = ppu->oamAdr;
      ppu->oamInHigh = ppu->oamInHighWritten;
      ppu->oamSecondWrite = false;
      break;
    }
    case 0x03: {
      ppu->objPriority = val & 0x80;
      ppu->oamInHigh = val & 1;
      ppu->oamInHighWritten = ppu->oamInHigh;
      ppu->oamAdr = ppu->oamAdrWritten;
      ppu->oamSecondWrite = false;
      break;
    }
    case 0x04: {
      if(ppu->oamInHigh) {
        ppu->highOam[((ppu->oamAdr & 0xf) << 1) | ppu->oamSecondWrite] = val;
        if(ppu->oamSecondWrite) {
          ppu->oamAdr++;
          if(ppu->oamAdr == 0) ppu->oamInHigh = false;
        }
      } else {
        if(!ppu->oamSecondWrite) {
          ppu->oamBuffer = val;
        } else {
          ppu->oam[ppu->oamAdr++] = (val << 8) | ppu->oamBuffer;
          if(ppu->oamAdr == 0) ppu->oamInHigh = true;
        }
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      break;
    }
    case 0x05: {
      ppu->mode = val & 0x7;
      ppu->bg3priority = val & 0x8;
      ppu->bgLayer[0].bigTiles = val & 0x10;
      ppu->bgLayer[1].bigTiles = val & 0x20;
      ppu->bgLayer[2].bigTiles = val & 0x40;
      ppu->bgLayer[3].bigTiles = val & 0x80;
      break;
    }
    case 0x06: {
      // TODO: mosaic line reset specifics
      ppu->bgLayer[0].mosaicEnabled = val & 0x1;
      ppu->bgLayer[1].mosaicEnabled = val & 0x2;
      ppu->bgLayer[2].mosaicEnabled = val & 0x4;
      ppu->bgLayer[3].mosaicEnabled = val & 0x8;
      ppu->mosaicSize = (val >> 4) + 1;
      ppu->mosaicStartLine = ppu->snes->vPos;
      break;
    }
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0a: {
      ppu->bgLayer[adr - 7].tilemapWider = val & 0x1;
      ppu->bgLayer[adr - 7].tilemapHigher = val & 0x2;
      ppu->bgLayer[adr - 7].tilemapAdr = (val & 0xfc) << 8;
      break;
    }
    case 0x0b: {
      ppu->bgLayer[0].tileAdr = (val & 0xf) << 12;
      ppu->bgLayer[1].tileAdr = (val & 0xf0) << 8;
      break;
    }
    case 0x0c: {
      ppu->bgLayer[2].tileAdr = (val & 0xf) << 12;
      ppu->bgLayer[3].tileAdr = (val & 0xf0) << 8;
      break;
    }
    case 0x0d: {
      ppu->m7matrix[6] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-HOFS
    }
    case 0x0f:
    case 0x11:
    case 0x13: {
      ppu->bgLayer[(adr - 0xd) / 2].hScroll = ((val << 8) | (ppu->scrollPrev & 0xf8) | (ppu->scrollPrev2 & 0x7)) & 0x3ff;
      ppu->scrollPrev = val;
      ppu->scrollPrev2 = val;
      break;
    }
    case 0x0e: {
      ppu->m7matrix[7] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-VOFS
    }
    case 0x10:
    case 0x12:
    case 0x14: {
      ppu->bgLayer[(adr - 0xe) / 2].vScroll = ((val << 8) | ppu->scrollPrev) & 0x3ff;
      ppu->scrollPrev = val;
      break;
    }
    case 0x15: {
      if((val & 3) == 0) {
        ppu->vramIncrement = 1;
      } else if((val & 3) == 1) {
        ppu->vramIncrement = 32;
      } else {
        ppu->vramIncrement = 128;
      }
      ppu->vramRemapMode = (val & 0xc) >> 2;
      ppu->vramIncrementOnHigh = val & 0x80;
      break;
    }
    case 0x16: {
      ppu->vramPointer = (ppu->vramPointer & 0xff00) | val;
      ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
      break;
    }
    case 0x17: {
      ppu->vramPointer = (ppu->vramPointer & 0x00ff) | (val << 8);
      ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
      break;
    }
    case 0x18: {
      // TODO: vram access during rendering (also cgram and oam)
      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0xff00) | val;
      ppu_wsStampVram(ppu, vramAdr);
      if(!ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case 0x19: {
      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0x00ff) | (val << 8);
      ppu_wsStampVram(ppu, vramAdr);
      if(ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case 0x1a: {
      ppu->m7largeField = val & 0x80;
      ppu->m7charFill = val & 0x40;
      ppu->m7yFlip = val & 0x2;
      ppu->m7xFlip = val & 0x1;
      break;
    }
    case 0x1b:
    case 0x1c:
    case 0x1d:
    case 0x1e: {
      ppu->m7matrix[adr - 0x1b] = (val << 8) | ppu->m7prev;
      ppu->m7prev = val;
      break;
    }
    case 0x1f:
    case 0x20: {
      ppu->m7matrix[adr - 0x1b] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      break;
    }
    case 0x21: {
      ppu->cgramPointer = val;
      ppu->cgramSecondWrite = false;
      break;
    }
    case 0x22: {
      if(!ppu->cgramSecondWrite) {
        ppu->cgramBuffer = val;
      } else {
        ppu->cgram[ppu->cgramPointer++] = (val << 8) | ppu->cgramBuffer;
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      break;
    }
    case 0x23:
    case 0x24:
    case 0x25: {
      ppu->windowLayer[(adr - 0x23) * 2].window1inversed = val & 0x1;
      ppu->windowLayer[(adr - 0x23) * 2].window1enabled = val & 0x2;
      ppu->windowLayer[(adr - 0x23) * 2].window2inversed = val & 0x4;
      ppu->windowLayer[(adr - 0x23) * 2].window2enabled = val & 0x8;
      ppu->windowLayer[(adr - 0x23) * 2 + 1].window1inversed = val & 0x10;
      ppu->windowLayer[(adr - 0x23) * 2 + 1].window1enabled = val & 0x20;
      ppu->windowLayer[(adr - 0x23) * 2 + 1].window2inversed = val & 0x40;
      ppu->windowLayer[(adr - 0x23) * 2 + 1].window2enabled = val & 0x80;
      break;
    }
    case 0x26: {
      ppu->window1left = val;
      break;
    }
    case 0x27: {
      ppu->window1right = val;
      break;
    }
    case 0x28: {
      ppu->window2left = val;
      break;
    }
    case 0x29: {
      ppu->window2right = val;
      break;
    }
    case 0x2a: {
      ppu->windowLayer[0].maskLogic = val & 0x3;
      ppu->windowLayer[1].maskLogic = (val >> 2) & 0x3;
      ppu->windowLayer[2].maskLogic = (val >> 4) & 0x3;
      ppu->windowLayer[3].maskLogic = (val >> 6) & 0x3;
      break;
    }
    case 0x2b: {
      ppu->windowLayer[4].maskLogic = val & 0x3;
      ppu->windowLayer[5].maskLogic = (val >> 2) & 0x3;
      break;
    }
    case 0x2c: {
      ppu->layer[0].mainScreenEnabled = val & 0x1;
      ppu->layer[1].mainScreenEnabled = val & 0x2;
      ppu->layer[2].mainScreenEnabled = val & 0x4;
      ppu->layer[3].mainScreenEnabled = val & 0x8;
      ppu->layer[4].mainScreenEnabled = val & 0x10;
      break;
    }
    case 0x2d: {
      ppu->layer[0].subScreenEnabled = val & 0x1;
      ppu->layer[1].subScreenEnabled = val & 0x2;
      ppu->layer[2].subScreenEnabled = val & 0x4;
      ppu->layer[3].subScreenEnabled = val & 0x8;
      ppu->layer[4].subScreenEnabled = val & 0x10;
      break;
    }
    case 0x2e: {
      ppu->layer[0].mainScreenWindowed = val & 0x1;
      ppu->layer[1].mainScreenWindowed = val & 0x2;
      ppu->layer[2].mainScreenWindowed = val & 0x4;
      ppu->layer[3].mainScreenWindowed = val & 0x8;
      ppu->layer[4].mainScreenWindowed = val & 0x10;
      break;
    }
    case 0x2f: {
      ppu->layer[0].subScreenWindowed = val & 0x1;
      ppu->layer[1].subScreenWindowed = val & 0x2;
      ppu->layer[2].subScreenWindowed = val & 0x4;
      ppu->layer[3].subScreenWindowed = val & 0x8;
      ppu->layer[4].subScreenWindowed = val & 0x10;
      break;
    }
    case 0x30: {
      ppu->directColor = val & 0x1;
      ppu->addSubscreen = val & 0x2;
      ppu->preventMathMode = (val & 0x30) >> 4;
      ppu->clipMode = (val & 0xc0) >> 6;
      break;
    }
    case 0x31: {
      ppu->subtractColor = val & 0x80;
      ppu->halfColor = val & 0x40;
      for(int i = 0; i < 6; i++) {
        ppu->mathEnabled[i] = val & (1 << i);
      }
      break;
    }
    case 0x32: {
      if(val & 0x80) ppu->fixedColorB = val & 0x1f;
      if(val & 0x40) ppu->fixedColorG = val & 0x1f;
      if(val & 0x20) ppu->fixedColorR = val & 0x1f;
      break;
    }
    case 0x33: {
      ppu->interlace = val & 0x1;
      ppu->objInterlace = val & 0x2;
      ppu->overscan = val & 0x4;
      ppu->pseudoHires = val & 0x8;
      ppu->m7extBg = val & 0x40;
      break;
    }
    default: {
      break;
    }
  }
}

void ppu_putPixels(Ppu* ppu, uint8_t* pixels) {
  for(int y = 0; y < (ppu->frameOverscan ? 239 : 224); y++) {
    int dest = y * 2 + (ppu->frameOverscan ? 2 : 16);
    int y1 = y, y2 = y + 239;
    if(!ppu->frameInterlace) {
      y1 = y + (ppu->evenFrame ? 0 : 239);
      y2 = y1;
    }
    memcpy(pixels + (dest * 2048), &ppu->pixelBuffer[y1 * 2048], 2048);
    memcpy(pixels + ((dest + 1) * 2048), &ppu->pixelBuffer[y2 * 2048], 2048);
  }
  // clear top 2 lines, and following 14 and last 16 lines if not overscanning
  memset(pixels, 0, 2048 * 2);
  if(!ppu->frameOverscan) {
    memset(pixels + (2 * 2048), 0, 2048 * 14);
    memset(pixels + (464 * 2048), 0, 2048 * 16);
  }
}

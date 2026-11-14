
#ifndef PPU_H
#define PPU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Ppu Ppu;

// [widescreen] maximum extra columns per side (enough for 21:9 with square pixels)
#define PPU_WS_MAX 144
#define PPU_WS_PITCH ((256 + 2 * PPU_WS_MAX) * 4)

#include "snes.h"
#include "statehandler.h"

typedef struct BgLayer {
  uint16_t hScroll;
  uint16_t vScroll;
  bool tilemapWider;
  bool tilemapHigher;
  uint16_t tilemapAdr;
  uint16_t tileAdr;
  bool bigTiles;
  bool mosaicEnabled;
} BgLayer;

typedef struct Layer {
  bool mainScreenEnabled;
  bool subScreenEnabled;
  bool mainScreenWindowed;
  bool subScreenWindowed;
} Layer;

typedef struct WindowLayer {
  bool window1enabled;
  bool window2enabled;
  bool window1inversed;
  bool window2inversed;
  uint8_t maskLogic;
} WindowLayer;

struct Ppu {
  Snes* snes;
  // vram access
  uint16_t vram[0x8000];
  uint16_t vramPointer;
  bool vramIncrementOnHigh;
  uint16_t vramIncrement;
  uint8_t vramRemapMode;
  uint16_t vramReadBuffer;
  // cgram access
  uint16_t cgram[0x100];
  uint8_t cgramPointer;
  bool cgramSecondWrite;
  uint8_t cgramBuffer;
  // oam access
  uint16_t oam[0x100];
  uint8_t highOam[0x20];
  uint8_t oamAdr;
  uint8_t oamAdrWritten;
  bool oamInHigh;
  bool oamInHighWritten;
  bool oamSecondWrite;
  uint8_t oamBuffer;
  // object/sprites
  bool objPriority;
  uint16_t objTileAdr1;
  uint16_t objTileAdr2;
  uint8_t objSize;
  // [widescreen] line buffers cover -PPU_WS_MAX..255+PPU_WS_MAX, index = x + PPU_WS_MAX
  uint8_t objPixelBuffer[256 + 2 * PPU_WS_MAX];
  uint8_t objPriorityBuffer[256 + 2 * PPU_WS_MAX];
  bool timeOver;
  bool rangeOver;
  bool objInterlace;
  // background layers
  BgLayer bgLayer[4];
  uint8_t scrollPrev;
  uint8_t scrollPrev2;
  uint8_t mosaicSize;
  uint8_t mosaicStartLine;
  // layers
  Layer layer[5];
  // mode 7
  int16_t m7matrix[8]; // a, b, c, d, x, y, h, v
  uint8_t m7prev;
  bool m7largeField;
  bool m7charFill;
  bool m7xFlip;
  bool m7yFlip;
  bool m7extBg;
  // mode 7 internal
  int32_t m7startX;
  int32_t m7startY;
  // windows
  WindowLayer windowLayer[6];
  uint8_t window1left;
  uint8_t window1right;
  uint8_t window2left;
  uint8_t window2right;
  // color math
  uint8_t clipMode;
  uint8_t preventMathMode;
  bool addSubscreen;
  bool subtractColor;
  bool halfColor;
  bool mathEnabled[6];
  uint8_t fixedColorR;
  uint8_t fixedColorG;
  uint8_t fixedColorB;
  // settings
  bool forcedBlank;
  uint8_t brightness;
  uint8_t mode;
  bool bg3priority;
  bool evenFrame;
  bool pseudoHires;
  bool overscan;
  bool frameOverscan; // if we are overscanning this frame (determined at 0,225)
  bool interlace;
  bool frameInterlace; // if we are interlacing this frame (determined at start vblank)
  bool directColor;
  // latching
  uint16_t hCount;
  uint16_t vCount;
  bool hCountSecond;
  bool vCountSecond;
  bool countersLatched;
  uint8_t ppu1openBus;
  uint8_t ppu2openBus;
  // pixel buffer (xbgr)
  // times 2 for even and odd frame
  uint8_t pixelBuffer[512 * 4 * 239 * 2];
  uint8_t pixelOutputFormat;
  // [widescreen] extra columns rendered on each side (0 = off), and which BG layers may
  // extend (bit n = BG n+1; a layer that doesn't extend is transparent outside 0..255);
  // bit 4 = don't extend layers whose horizontal scroll is 0 (HUDs, still pictures)
  int wsExtra;
  uint8_t wsBgMask;
  // [widescreen] per-frame hook (called before line 1) that may provide tile sources for
  // the extension and/or blank the extension for the frame (wsBlank)
  void (*wsFrameHook)(Ppu* ppu, void* user);
  void* wsUser;
  // [recomp] called at the start of every rendered line (motion smoothing records registers)
  void (*lineHook)(Ppu* ppu, int line, void* user);
  void* lineUser;
  // [recomp] draw every sprite on a line, ignoring the 32-sprite / 34-sliver hardware limit
  // (the overflow flags in $213E are still set as on hardware, so game logic is unchanged)
  bool noSpriteLimit;
  uint32_t spriteOverflowLines; // [recomp] lines that went over the limit (diagnostics)
  int spriteMaxOnLine, sliverMaxOnLine; // [recomp] busiest line seen (diagnostics)
  bool wsBlank;
  int wsEdgeScroll; // [widescreen] BG1 scroll on the camera's line (wsCamLine) this picture: the level edges (lxMin/lxMax) are relative to it
  int wsRefH;       // [widescreen] the (10-bit) BG1 scroll the level edges were worked out for
  bool wsDebugLayers;
  int wsCamLine;                // [widescreen] line whose scroll is the camera's (the level edges follow it)
  uint8_t wsDebugHide;         // [widescreen] debug: BG layers (bit n = BG n+1) not drawn at all
  bool wsNoRecord;             // [widescreen] set while drawing an in-between picture
  int16_t wsBrightFine;        // [recomp] -1, or a brightness on a 0..255 scale used instead of the
                               // register's 16 steps (in-between pictures of a fade)
  uint16_t wsEpoch;            // [widescreen] current load (see wsVramEpoch)
  bool wsInLoad;
  int wsDbgMapWrites, wsDbgBright;
  uint16_t wsVramEpoch[0x8000]; // [widescreen] load in which each VRAM word was last written
  bool wsStats;                // [widescreen] test: count sprite pixels by the 4:3 edges (wsStatObj)
  uint32_t wsStatObj[4];       // [widescreen] test: 16 px bands left-out, left-in, right-in, right-out
  uint16_t wsLineH[2][240];     // [widescreen] BG1/BG2 horizontal scroll of each line of the last frame
  uint16_t wsLineV[2][240];     // [widescreen] ...and vertical scroll (tests)
  int wsPgY[4], wsPgH[4], wsPg[4]; // [widescreen] page found for the line being drawn (cache)
  uint8_t wsLineLayer[256 + 2 * PPU_WS_MAX]; // [widescreen] layer shown per dot on the current line   // debug: color extension pixels by the layer that produced them
  bool noPixelBuffer;          // [recomp] the host only uses the wide output (wsBuffer): pixelBuffer isn't written
  struct {
    bool valid;
    const uint8_t* base;  // tile word for (row r, column c) at base[r * stride + c * 2]
    int stride, width, rows;
    int delta;            // column = floor((x + hScroll) / tileWidth) + delta, wrapped to width
    int refCol;           // tilemap column (scroll / tileWidth) delta was found at; -1 = none. A line
                          // scrolled elsewhere shows the same 32-column tilemap, so its delta is taken
                          // at the page (multiple of 32 columns) nearest to this one
    int rdelta;           // row = floor((line + vScroll) / tileHeight) + rdelta (unwrapped)
    bool vring;           // rows wrap like the hardware map (VRAM-backed source) instead of rdelta
    bool clamp;           // if set, only x + hScroll in [lxMin, lxMax) is inside the map
    int lxMin, lxMax;
    int refV;             // the (10-bit) vScroll rdelta was found at: a scroll on the other side of
                          // the 1024 wrap from it (an in-between picture) is counted from there
  } wsSrc[4];
  // [widescreen] one pixel per dot, pitch PPU_WS_PITCH bytes, x = -PPU_WS_MAX at offset 0
  uint8_t wsBuffer[PPU_WS_PITCH * 239 * 2];
};

enum { ppu_pixelOutputFormatXBGR = 0, ppu_pixelOutputFormatBGRX = 1 };

Ppu* ppu_init(Snes* snes);
void ppu_free(Ppu* ppu);
void ppu_reset(Ppu* ppu);
void ppu_handleState(Ppu* ppu, StateHandler* sh);
bool ppu_checkOverscan(Ppu* ppu);
void ppu_handleVblank(Ppu* ppu);
void ppu_handleFrameStart(Ppu* ppu);
void ppu_runLine(Ppu* ppu, int line);
uint8_t ppu_read(Ppu* ppu, uint8_t adr);
void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val);
void ppu_putPixels(Ppu* ppu, uint8_t* pixels);
// [widescreen] copies the visible 224 lines, (256 + 2 * wsExtra) pixels wide, 4 bytes each
int ppu_putPixelsWide(Ppu* ppu, uint8_t* pixels);
void ppu_setPixelOutputFormat(Ppu* ppu, int pixelOutputFormat);

#endif

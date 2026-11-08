/* Tiny dependency-free PNG writer (stored deflate). Header-only. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t png__crc(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; t[i] = v; }
    c ^= 0xFFFFFFFFu; while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}
static void png__be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void png__chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; png__be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = png__crc(0, (const uint8_t *)type, 4); c = png__crc(c, d, n); png__be32(h, c); fwrite(h, 1, 4, f);
}

/* Write w*h RGB8 pixels. Returns 0 on success. */
static int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h) {
    size_t row = 1 + (size_t)w * 3, rawlen = row * h;
    uint8_t *raw = malloc(rawlen);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t nblocks = (rawlen + 65534) / 65535, zlen = 2 + rawlen + nblocks * 5 + 4;
    uint8_t *z = malloc(zlen), *o = z;
    if (!z) { free(raw); return -1; }
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawlen; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < rawlen; off += 65535) {
        uint16_t len = (uint16_t)((rawlen - off) < 65535 ? (rawlen - off) : 65535);
        *o++ = off + len >= rawlen; *o++ = (uint8_t)len; *o++ = (uint8_t)(len >> 8);
        *o++ = (uint8_t)~len; *o++ = (uint8_t)(~len >> 8); memcpy(o, raw + off, len); o += len;
    }
    png__be32(o, (b << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; png__be32(ihdr, (uint32_t)w); png__be32(ihdr + 4, (uint32_t)h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png__chunk(f, "IHDR", ihdr, 13); png__chunk(f, "IDAT", z, (uint32_t)(o - z)); png__chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return 0;
}

/* LakeSnes 512x480 BGRX frame -> 256x224 RGB (visible area, every other pixel/line). */
static void png_from_snes(uint8_t *rgb, const uint8_t *bgrx) {
    for (int y = 0; y < 224; y++) {
        const uint8_t *s = bgrx + (size_t)(16 + y * 2) * 2048;
        for (int x = 0; x < 256; x++, s += 8) { *rgb++ = s[2]; *rgb++ = s[1]; *rgb++ = s[0]; }
    }
}

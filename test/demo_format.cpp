// Demo: adding a new pixel format takes ONE descriptor entry.
// Compiles PNGdec.cpp with -DPNG_DEMO_FMT, which enables a single extra
// block in png.inl: one conversion function + one table entry for a
// pseudo format "inverted 8-bit grayscale" (PNG_PIXEL_GRAY8_INVERTED).
// This file feeds a synthetic scanline through the generic PNGRGB565()
// dispatcher and checks it against the standard gray converter run on
// inverted input.
#include "PNGdec.cpp" // PNG_STATIC functions become visible in this TU
#include <cstdio>
#include <cstring>

int main()
{
    uint8_t src[8] = {0, 32, 64, 96, 128, 160, 192, 255};
    uint8_t inv[8];
    uint16_t out[8], ref[8];
    PNGDRAW d, d2;

    memset(&d, 0, sizeof(d));
    d.iPixelType = PNG_PIXEL_GRAY8_INVERTED; // the demo format
    d.iBpp = 8;
    d.iWidth = 8;
    d.pPixels = src;
    PNGRGB565(&d, out, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

    for (int i = 0; i < 8; i++) inv[i] = (uint8_t)(255 - src[i]);
    d2 = d;
    d2.iPixelType = PNG_PIXEL_GRAYSCALE; // reference: gray8 of inverted input
    d2.pPixels = inv;
    PNGRGB565(&d2, ref, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

    int fails = 0;
    for (int i = 0; i < 8; i++) {
        bool ok = (out[i] == ref[i]);
        printf("in=%3d -> demo=0x%04x ref=0x%04x %s\n", src[i], out[i], ref[i], ok ? "OK" : "MISMATCH");
        if (!ok) fails++;
    }
    if (fails) { printf("DEMO FAILED\n"); return 1; }
    printf("demo format dispatched correctly via its single descriptor entry\n");
    return 0;
}

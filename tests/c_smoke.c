/*
 * c_smoke.c - the new stride API is declared static inline in PNGdec.h and
 * must remain usable from a plain C translation unit (the library also ships
 * a C API). This only exercises the non-decoding helpers plus a synthetic
 * draw callback; no file IO needed.
 */
#include "../src/PNGdec.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    PNGStrideConfig cfg;
    uint8_t buf[64];
    PNGDRAW draw;
    uint8_t line[4] = {1, 2, 3, 4};
    int rc;

    if (PNGStride_align(21, 32) != 32 || PNGStride_align(32, 32) != 32 ||
        PNGStride_align(7, 8) != 8) {
        printf("FAIL: PNGStride_align\n");
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    memset(buf, 0xCD, sizeof(buf));
    cfg.pBuffer = buf;
    cfg.iStride = 8;
    cfg.iHeight = 2;
    cfg.bFillPad = 1;
    cfg.ucPad = 0xEE;

    rc = PNGStride_validate(&cfg, 4, 2, 4);
    if (rc != PNG_SUCCESS || PNGStride_requiredSize(&cfg) != 16) {
        printf("FAIL: validate/requiredSize\n");
        return 1;
    }

    memset(&draw, 0, sizeof(draw));
    draw.pUser = &cfg;
    draw.pPixels = line;
    draw.iPitch = 4;
    draw.iWidth = 4;
    draw.y = 0;
    if (PNGStride_draw(&draw) != 1 ||
        memcmp(buf, line, 4) != 0 || buf[4] != 0xEE || buf[7] != 0xEE) {
        printf("FAIL: PNGStride_draw fill\n");
        return 1;
    }

    /* flipped second row must land at row 0 */
    cfg.bFlipV = 1;
    draw.y = 1;
    if (PNGStride_draw(&draw) != 1 || memcmp(buf, line, 4) != 0) {
        printf("FAIL: PNGStride_draw flip\n");
        return 1;
    }

    /* too-small stride must be rejected by the callback itself */
    cfg.bFlipV = 0;
    draw.iPitch = 4;
    cfg.iStride = 3;
    draw.y = 0;
    if (PNGStride_draw(&draw) != 0 || cfg.iError != PNG_INVALID_PARAMETER) {
        printf("FAIL: small stride not rejected\n");
        return 1;
    }

    printf("c_smoke: all C API checks passed\n");
    return 0;
}

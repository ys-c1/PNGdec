//
// stride_test.cpp
//
// One-shot verification of the GPU-friendly stride output added in PNGdec.h.
// For each of the four generated test PNGs it checks:
//   1. compact (setBuffer) decode still produces the reference pixels
//   2. a hand-written line-by-line copy callback is byte-identical to it
//   3. PNGStride_draw writes identical active pixels at a padded stride
//   4. vertical flip stores the same rows in reverse order
//   5. end-of-row padding is filled (and never overwritten)
//   6. a stride smaller than the source row width is rejected
//   7. decode timing: compact vs stride vs per-line copy
//   8. memory increment of the feature
//
// Standard library only; links the in-repo PNGdec objects.
//
#include "../src/PNGdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_nFail = 0;
static int g_nPass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_nPass++; } \
    else { g_nFail++; printf("    FAIL: %s\n", msg); } \
} while (0)

static uint8_t *g_pManual = NULL; // destination of the reference callback

// Reference line callback: exactly what a caller would do today to get
// compact rows out of the decoder one line at a time.
static int ManualCopyDraw(PNGDRAW *pDraw)
{
    memcpy(g_pManual + pDraw->y * pDraw->iPitch, pDraw->pPixels,
           (size_t)pDraw->iPitch);
    return 1;
}

static uint8_t *LoadFile(const char *path, int *pSize)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *pSize = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = (uint8_t *)malloc((size_t)*pSize);
    if (fread(p, 1, (size_t)*pSize, f) != (size_t)*pSize) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    return p;
}

static const char *PixelTypeName(int t)
{
    switch (t) {
        case PNG_PIXEL_GRAYSCALE:       return "grayscale";
        case PNG_PIXEL_TRUECOLOR:       return "RGB";
        case PNG_PIXEL_INDEXED:         return "indexed";
        case PNG_PIXEL_GRAY_ALPHA:      return "gray+alpha";
        case PNG_PIXEL_TRUECOLOR_ALPHA: return "RGBA";
        default:                        return "?";
    }
}

static long NowUs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

// Re-open + decode through a given path, repeated iIters times.
static long TimeDecode(uint8_t *data, int size, int iIters,
                       PNG_DRAW_CALLBACK *cb, void *pUser, uint8_t *dest)
{
    long t0 = NowUs();
    for (int i = 0; i < iIters; i++) {
        PNG png;
        if (png.openRAM(data, size, cb) != PNG_SUCCESS)
            return -1;
        if (cb == NULL)
            png.setBuffer(dest);
        if (png.decode(pUser, 0) != PNG_SUCCESS)
            return -1;
    }
    return NowUs() - t0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    const int kIters = 3000;
    const int kAlign = 32;

    struct Case {
        const char *name;
        int expectLineBytes;
        int expectAlpha;
    } cases[] = {
        {"nonaligned_rgb.png", 7 * 3,  0},
        {"aligned_rgb.png",    8 * 3,  0},
        {"alpha_rgba.png",     7 * 4,  1},
        {"palette_idx.png",    7,      1},
    };

    printf("== PNGStride feature test (sizeof(PNGStrideConfig)=%u) ==\n\n",
           (unsigned)sizeof(PNGStrideConfig));

    for (int ci = 0; ci < (int)(sizeof(cases) / sizeof(cases[0])); ci++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, cases[ci].name);
        int size = 0;
        uint8_t *data = LoadFile(path, &size);
        CHECK(data != NULL, "load png");
        if (data == NULL)
            continue;

        PNG png;
        int rc = png.openRAM(data, size, NULL);
        CHECK(rc == PNG_SUCCESS, "openRAM");
        int w = png.getWidth(), h = png.getHeight();
        int lineBytes = png.getBufferSize() / h;
        printf("--- %s  %dx%d  %s  bpp=%d  hasAlpha(open)=%d  pitch=%d ---\n",
               cases[ci].name, w, h,
               PixelTypeName(png.getPixelType()), png.getBpp(),
               png.hasAlpha(), lineBytes);
        CHECK(lineBytes == cases[ci].expectLineBytes, "expected pitch");

        // ---- 1. compact reference decode (existing behaviour) ----
        uint8_t *compact = (uint8_t *)malloc((size_t)png.getBufferSize());
        memset(compact, 0xAA, (size_t)png.getBufferSize());
        png.setBuffer(compact);
        rc = png.decode(NULL, 0);
        CHECK(rc == PNG_SUCCESS && png.getLastError() == PNG_SUCCESS,
              "compact decode succeeds");
        // For indexed images the tRNS chunk is parsed during decode(), so
        // hasAlpha() only becomes true afterwards (pre-existing behaviour).
        CHECK(png.hasAlpha() == cases[ci].expectAlpha, "expected alpha flag");
        if (png.getPixelType() == PNG_PIXEL_INDEXED &&
            cases[ci].expectAlpha) {
            uint8_t *pal = png.getPalette();
            int alphaOk = (pal[768] == 0xFF && pal[769] == 0x40 &&
                           pal[770] == 0xFF && pal[771] == 0xFF);
            CHECK(alphaOk, "tRNS palette alpha bytes decoded (ff 40 ff ff)");
        }

        // ---- 2. line-by-line manual copy, same compact layout ----
        uint8_t *manual = (uint8_t *)malloc((size_t)png.getBufferSize());
        memset(manual, 0x55, (size_t)png.getBufferSize());
        g_pManual = manual;
        PNG png2;
        rc = png2.openRAM(data, size, ManualCopyDraw);
        CHECK(rc == PNG_SUCCESS, "reopen for manual copy");
        rc = png2.decode(NULL, 0);
        CHECK(rc == PNG_SUCCESS, "manual-copy decode succeeds");
        CHECK(memcmp(manual, compact, (size_t)png.getBufferSize()) == 0,
              "per-line copy is byte-identical to compact output");

        // ---- 3. padded stride output, no flip ----
        int stride = PNGStride_align(lineBytes, kAlign);
        int destSize = stride * h;
        int padPerRow = stride - lineBytes;
        uint8_t *padded = (uint8_t *)malloc((size_t)destSize);
        memset(padded, 0xEE, (size_t)destSize); // poison: padding must change
        PNGStrideConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.pBuffer = padded;
        cfg.iStride = stride;
        cfg.iHeight = h;
        cfg.bFillPad = 1;
        cfg.ucPad = 0xCD;
        CHECK(PNGStride_validate(&cfg, w, h, lineBytes) == PNG_SUCCESS,
              "valid stride config accepted");
        CHECK(PNGStride_requiredSize(&cfg) == destSize,
              "requiredSize() matches stride*height");
        PNG png3;
        rc = png3.openRAM(data, size, PNGStride_draw);
        CHECK(rc == PNG_SUCCESS, "reopen for stride decode");
        rc = png3.decode(&cfg, 0);
        CHECK(rc == PNG_SUCCESS && cfg.iError == PNG_SUCCESS,
              "stride decode succeeds");
        int activeOk = 1, padOk = 1;
        for (int y = 0; y < h; y++) {
            if (memcmp(padded + y * stride, compact + y * lineBytes,
                       (size_t)lineBytes) != 0)
                activeOk = 0;
            for (int p = 0; p < padPerRow; p++) {
                if (padded[y * stride + lineBytes + p] != 0xCD)
                    padOk = 0;
            }
        }
        CHECK(activeOk, "stride active pixels equal compact output, all rows");
        CHECK(padOk, "all end-of-row padding bytes filled with ucPad");

        // no-fill mode: padding must be left exactly as the caller left it
        if (padPerRow > 0) {
            PNGStrideConfig cfgN;
            memset(&cfgN, 0, sizeof(cfgN));
            uint8_t *nofill = (uint8_t *)malloc((size_t)destSize);
            memset(nofill, 0xEE, (size_t)destSize);
            cfgN.pBuffer = nofill;
            cfgN.iStride = stride;
            cfgN.iHeight = h;
            cfgN.bFillPad = 0;
            PNG pngN;
            pngN.openRAM(data, size, PNGStride_draw);
            rc = pngN.decode(&cfgN, 0);
            int nofillOk = (rc == PNG_SUCCESS);
            for (int y = 0; y < h && nofillOk; y++) {
                if (memcmp(nofill + y * stride, compact + y * lineBytes,
                           (size_t)lineBytes) != 0)
                    nofillOk = 0;
                for (int p = 0; p < padPerRow; p++)
                    if (nofill[y * stride + lineBytes + p] != 0xEE)
                        nofillOk = 0;
            }
            CHECK(nofillOk, "bFillPad=0 leaves padding untouched, pixels OK");
            free(nofill);
        }

        // zero-padding edge: stride exactly equal to line bytes
        PNGStrideConfig cfg0;
        memset(&cfg0, 0, sizeof(cfg0));
        uint8_t *tight = (uint8_t *)malloc((size_t)(lineBytes * h));
        cfg0.pBuffer = tight;
        cfg0.iStride = lineBytes;
        cfg0.iHeight = h;
        PNG png4;
        png4.openRAM(data, size, PNGStride_draw);
        rc = png4.decode(&cfg0, 0);
        CHECK(rc == PNG_SUCCESS &&
              memcmp(tight, compact, (size_t)(lineBytes * h)) == 0,
              "stride==pitch is identical to compact output");
        free(tight);

        // ---- 4. vertical flip into an identically padded buffer ----
        uint8_t *flipped = (uint8_t *)malloc((size_t)destSize);
        memset(flipped, 0xEE, (size_t)destSize);
        PNGStrideConfig cfgF;
        memset(&cfgF, 0, sizeof(cfgF));
        cfgF.pBuffer = flipped;
        cfgF.iStride = stride;
        cfgF.iHeight = h;
        cfgF.bFlipV = 1;
        cfgF.bFillPad = 1;
        cfgF.ucPad = 0xCD;
        PNG png5;
        png5.openRAM(data, size, PNGStride_draw);
        rc = png5.decode(&cfgF, 0);
        CHECK(rc == PNG_SUCCESS, "flipped stride decode succeeds");
        int flipOk = 1, flipPadOk = 1;
        for (int y = 0; y < h; y++) {
            if (memcmp(flipped + (h - 1 - y) * stride,
                       compact + y * lineBytes, (size_t)lineBytes) != 0)
                flipOk = 0;
            for (int p = 0; p < padPerRow; p++) {
                if (flipped[y * stride + lineBytes + p] != 0xCD)
                    flipPadOk = 0;
            }
        }
        CHECK(flipOk, "flip: compact row y lands at destination row h-1-y");
        CHECK(flipPadOk, "flip: padding still filled");

        // ---- 5. error: stride smaller than the row width ----
        PNGStrideConfig cfgBad;
        memset(&cfgBad, 0, sizeof(cfgBad));
        cfgBad.pBuffer = padded;
        cfgBad.iStride = lineBytes - 1;
        cfgBad.iHeight = h;
        CHECK(PNGStride_validate(&cfgBad, w, h, lineBytes) ==
                  PNG_INVALID_PARAMETER,
              "validate rejects stride < row width");
        CHECK(PNGStride_validate(NULL, w, h, lineBytes) ==
                  PNG_INVALID_PARAMETER,
              "validate rejects NULL config");
        PNGStrideConfig cfgShort;
        memset(&cfgShort, 0, sizeof(cfgShort));
        cfgShort.pBuffer = padded;
        cfgShort.iStride = lineBytes - 1;
        cfgShort.iHeight = h;
        PNG png6;
        png6.openRAM(data, size, PNGStride_draw);
        rc = png6.decode(&cfgShort, 0);
        CHECK(rc == PNG_QUIT_EARLY &&
              cfgShort.iError == PNG_INVALID_PARAMETER,
              "decode stops early with invalid stride, cause recorded");

        // ---- layout summary ----
        printf("    layout: lineBytes=%d stride=%d (align %d) pad/row=%d "
               "compact=%dB gpuBuf=%dB delta=%+dB\n",
               lineBytes, stride, kAlign, padPerRow,
               lineBytes * h, destSize, destSize - lineBytes * h);
        printf("    row0 first 12 bytes:");
        for (int i = 0; i < 12 && i < stride; i++)
            printf(" %02x", padded[i]);
        if (padPerRow > 0) {
            printf(" | pad:");
            for (int p = 0; p < 4 && p < padPerRow; p++)
                printf(" %02x", padded[lineBytes + p]);
        }
        printf("\n");

        // ---- 6. timing (all paths reopen every iteration) ----
        long tCompact = TimeDecode(data, size, kIters, NULL, NULL, compact);
        long tStride  = TimeDecode(data, size, kIters, PNGStride_draw,
                                   &cfg, padded);
        long tManual  = TimeDecode(data, size, kIters, ManualCopyDraw,
                                   NULL, manual);
        printf("    time over %d decodes (us): compact=%ld  stride=%ld  "
               "line-copy=%ld   per call: %.2f / %.2f / %.2f us\n",
               kIters, tCompact, tStride, tManual,
               tCompact / (double)kIters, tStride / (double)kIters,
               tManual / (double)kIters);

        free(data);
        free(compact);
        free(manual);
        free(padded);
        free(flipped);
        printf("\n");
    }

    printf("== SUMMARY: %d passed, %d failed ==\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}

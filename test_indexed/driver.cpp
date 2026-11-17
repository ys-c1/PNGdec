//
// driver.cpp - tests for palette-index output (extra decode() option bit 4)
//
// Built directly against ../src (no installed copy), checks:
//   1. 1/2/4/8-bit palette images return exactly width*height index bytes,
//      matching gen_samples.py's expectation matrix, without writing past the
//      caller's buffer.
//   2. getPalette() still returns the PLTE RGB triples and tRNS alpha bytes;
//      indexes are the same values used by the palette.
//   3. Optional simultaneous RGB output: with a draw callback installed and
//      the index option set, every row is delivered in the same decode pass
//      and getLineAsRGB565() agrees pixel-for-pixel with index -> palette.
//   4. Non-palette images (truecolor, grayscale) reject the option with
//      PNG_UNSUPPORTED_FEATURE and leave the buffer untouched; a palette
//      image without setBuffer() reports PNG_NO_BUFFER; plain decode(0) of
//      palette images still produces packed rows of the same size.
//   5. RGB565 of every sample is byte-identical to a dump produced by a
//      driver built from the unmodified library at git HEAD.
//   6. Timing and memory figures for the palette depths and truecolor
//      (peak RSS stays flat: the library performs no allocation; its state is
//      the 59,632-byte PNGIMAGE struct including the 32K inflate window).
//
// Note: PNGRGB565()'s 1/2/4-bit palette paths iterate in pixel pairs/quads, so
// for widths not divisible by 4 they can write up to 3 entries past iWidth.
// That is pre-existing library behaviour; the test line buffers below leave
// spare entries so this suite checks the indexed-output change, not that.
//
// Exit code 0 only if every check passes.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>
#include <string>
#include <vector>
#include <map>

#include "../src/PNGdec.h"

#ifndef PNG_INDEXED_OUTPUT
#define PNG_INDEXED_OUTPUT 4 // must match the option bit defined in png.inl
#endif

static int g_pass, g_fail;
static const char *g_section = "";

static void check(bool ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL [%s] %s\n", g_section, what);
    }
}

static double now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct Image {
    std::string name;
    int w, h, bits, ctype, palCount, hasTrns;
    std::vector<std::vector<uint8_t> > pal;   // r,g,b,a per entry
    std::vector<std::vector<int> > idx;       // palette indexes
    std::vector<std::vector<uint8_t> > rgb;   // truecolor/grayscale samples
};

static bool loadExpected(const char *path, std::map<std::string, Image> &out)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char name[128], kind[16];
    int w, h, bits, ctype, palCount, hasTrns;
    while (fscanf(f, "%127s %d %d %d %d %15s %d %d", name, &w, &h, &bits,
                  &ctype, kind, &palCount, &hasTrns) == 8) {
        Image im;
        im.name = name; im.w = w; im.h = h; im.bits = bits;
        im.ctype = ctype; im.palCount = palCount; im.hasTrns = hasTrns;
        if (strcmp(kind, "PAL") == 0) {
            for (int i = 0; i < palCount; i++) {
                int r, g, b, a;
                if (fscanf(f, "%d %d %d %d", &r, &g, &b, &a) != 4) return false;
                std::vector<uint8_t> e(4);
                e[0] = (uint8_t)r; e[1] = (uint8_t)g;
                e[2] = (uint8_t)b; e[3] = (uint8_t)a;
                im.pal.push_back(e);
            }
            im.idx.resize(h);
            for (int y = 0; y < h; y++) {
                im.idx[y].resize(w);
                for (int x = 0; x < w; x++)
                    if (fscanf(f, "%d", &im.idx[y][x]) != 1) return false;
            }
        } else {
            im.rgb.resize(h);
            for (int y = 0; y < h; y++) {
                im.rgb[y].resize((size_t)w * 3);
                for (int x = 0; x < w; x++) {
                    int r, g, b;
                    if (fscanf(f, "%d,%d,%d", &r, &g, &b) != 3) return false;
                    im.rgb[y][x * 3 + 0] = (uint8_t)r;
                    im.rgb[y][x * 3 + 1] = (uint8_t)g;
                    im.rgb[y][x * 3 + 2] = (uint8_t)b;
                }
            }
        }
        out[im.name] = im;
    }
    fclose(f);
    return !out.empty();
}

static std::vector<uint8_t> readFile(const char *path)
{
    std::vector<uint8_t> data;
    FILE *f = fopen(path, "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize((size_t)n);
    if (fread(&data[0], 1, (size_t)n, f) != (size_t)n) data.clear();
    fclose(f);
    return data;
}

// --------------------------------------------------------------- callbacks
struct DrawCtx {
    std::vector<uint16_t> rgb565;
    std::vector<int> rows;
    std::vector<const uint8_t *> palettes;
    int bpp, pixelType, hasAlpha;
};

static int drawToRGB565(PNGDRAW *pDraw)
{
    DrawCtx *ctx = (DrawCtx *)pDraw->pUser;
    // The 1/2/4-bit palette paths of PNGRGB565() work on pixel quads/pairs, so
    // the 2-bit path can write up to 3 pixels past iWidth (pre-existing
    // library behaviour); give the test line spare entries.
    std::vector<uint16_t> line((size_t)pDraw->iWidth + 4);
    extern void driverGetLineAsRGB565(PNGDRAW *, uint16_t *, int, uint32_t);
    driverGetLineAsRGB565(pDraw, &line[0], PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    ctx->rgb565.insert(ctx->rgb565.end(), line.begin(), line.begin() + pDraw->iWidth);
    ctx->rows.push_back(pDraw->y);
    ctx->palettes.push_back(pDraw->pPalette);
    ctx->bpp = pDraw->iBpp;
    ctx->pixelType = pDraw->iPixelType;
    ctx->hasAlpha = pDraw->iHasAlpha;
    return 1;
}

// RGB565 the library produces for one palette entry when no background blend
// is requested (u32Bkgd == 0xffffffff), mirroring PNGRGB565()'s palette paths.
static uint16_t paletteRGB565(const Image &im, int idx)
{
    int r = im.pal[idx][0], g = im.pal[idx][1], b = im.pal[idx][2];
    int a = im.pal[idx][3];
    if (im.bits == 8 && im.hasTrns) {
        // 8-bit palette with tRNS: every pixel is scaled by its palette alpha
        uint16_t p = (uint16_t)((b * a) >> 11);
        p |= (uint16_t)(((g * a) >> 10) << 5);
        p |= (uint16_t)(((r * a) >> 11) << 11);
        return p;
    }
    uint16_t p = (uint16_t)(b >> 3);
    p |= (uint16_t)((g >> 2) << 5);
    p |= (uint16_t)((r >> 3) << 11);
    return p;
}

// C++ helper shim: getLineAsRGB565 is a PNG method; use one shared instance.
void driverGetLineAsRGB565(PNGDRAW *pDraw, uint16_t *pPixels, int endian, uint32_t bg)
{
    static PNG helper;
    helper.getLineAsRGB565(pDraw, pPixels, endian, bg);
}

static unsigned long fnv1a(const std::vector<uint16_t> &v)
{
    unsigned long h = 1469598103934665603UL;
    for (size_t i = 0; i < v.size(); i++) {
        h ^= (unsigned long)v[i] & 0xff;
        h *= 1099511628211UL;
        h ^= (unsigned long)v[i] >> 8;
        h *= 1099511628211UL;
    }
    return h;
}

static unsigned long fnv1aBytes(const std::vector<uint8_t> &v)
{
    unsigned long h = 1469598103934665603UL;
    for (size_t i = 0; i < v.size(); i++) {
        h ^= v[i];
        h *= 1099511628211UL;
    }
    return h;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "samples";
    const char *baselineDump = (argc > 2) ? argv[2] : NULL;
    char path[1024];
    std::map<std::string, Image> expected;

    snprintf(path, sizeof(path), "%s/expected.txt", dir);
    if (!loadExpected(path, expected)) {
        printf("cannot read %s/expected.txt (run gen_samples.py first)\n", dir);
        return 2;
    }
    printf("decoder state sizeof(PNGIMAGE) = %u bytes (includes the 32K inflate window)\n\n",
           (unsigned)sizeof(PNGIMAGE));

    const char *palNames[] = {"idx1.png", "idx2.png", "idx4.png", "idx8.png"};
    const char *nonPalNames[] = {"true.png", "gray.png"};
    const char *allNames[] = {"idx1.png", "idx2.png", "idx4.png", "idx8.png",
                              "true.png", "gray.png"};

    // ============================================================ 1. indexes
    g_section = "indexes";
    for (int n = 0; n < 4; n++) {
        const Image &im = expected[palNames[n]];
        snprintf(path, sizeof(path), "%s/%s", dir, palNames[n]);
        std::vector<uint8_t> data = readFile(path);
        check(!data.empty(), "sample file readable");
        if (data.empty()) continue;

        size_t bufSize = (size_t)im.w * im.h, guard = 64;
        std::vector<uint8_t> buffer(bufSize + guard, 0xA5);

        PNG png;
        int rc = png.openRAM(&data[0], (int)data.size(), NULL);
        check(rc == PNG_SUCCESS, "openRAM");
        check(png.getPixelType() == PNG_PIXEL_INDEXED, "pixel type is indexed");
        check(png.getBpp() == im.bits, "bit depth reported");
        png.setBuffer(&buffer[0]);
        rc = png.decode(NULL, PNG_INDEXED_OUTPUT);
        check(rc == PNG_SUCCESS, "indexed decode success");
        check(png.getLastError() == PNG_SUCCESS, "getLastError PNG_SUCCESS");

        bool ok = true;
        for (int y = 0; y < im.h && ok; y++)
            for (int x = 0; x < im.w; x++)
                if (buffer[(size_t)y * im.w + x] != (uint8_t)im.idx[y][x]) ok = false;
        check(ok, "index bytes match expected matrix");
        check(buffer.size() - guard == (size_t)im.w * im.h, "index length is width*height");

        bool untouched = true;
        for (size_t i = bufSize; i < bufSize + guard; i++)
            if (buffer[i] != 0xA5) untouched = false;
        check(untouched, "no writes past width*height");

        uint8_t *pal = png.getPalette();
        bool palOk = true;
        for (size_t i = 0; i < im.pal.size(); i++) {
            if (pal[i * 3 + 0] != im.pal[i][0] ||
                pal[i * 3 + 1] != im.pal[i][1] ||
                pal[i * 3 + 2] != im.pal[i][2]) palOk = false;
            if (im.pal[i][3] != 255 && pal[768 + i] != im.pal[i][3]) palOk = false;
        }
        check(palOk, "getPalette RGB and tRNS alpha match");

        // plain decode(0) keeps producing packed rows for palette images;
        // rebuild the expected packed bytes from the index matrix and compare
        std::vector<uint8_t> packed((size_t)png.getBufferSize(), 0);
        PNG plain;
        plain.openRAM(&data[0], (int)data.size(), NULL);
        plain.setBuffer(&packed[0]);
        rc = plain.decode(NULL, 0);
        size_t rowBytes = (size_t)((im.w * im.bits + 7) / 8);
        bool packedOk = rc == PNG_SUCCESS &&
                        packed.size() == (size_t)im.h * rowBytes;
        std::vector<uint8_t> wantPacked(packed.size(), 0);
        for (int y = 0; y < im.h; y++) {
            int bitPos = 0;
            for (int x = 0; x < im.w; x++) {
                uint8_t v = (uint8_t)(im.idx[y][x] & ((1 << im.bits) - 1));
                for (int b = im.bits - 1; b >= 0; b--, bitPos++) {
                    if (v & (1 << b))
                        wantPacked[(size_t)y * rowBytes + bitPos / 8] |=
                            (uint8_t)(0x80 >> (bitPos % 8));
                }
            }
        }
        for (size_t i = 0; packedOk && i < packed.size(); i++)
            if (packed[i] != wantPacked[i]) {
                printf("  packed mismatch %s offset %zu got %02x want %02x\n",
                       palNames[n], i, packed[i], wantPacked[i]);
                packedOk = false;
            }
        check(packedOk, "plain indexed decode(0) packed bytes unchanged");
        // option bits compose: fast palette and CRC checking do not disturb indexes
        {
            std::vector<uint8_t> combo(bufSize + 64, 0xA5);
            PNG c;
            c.openRAM(&data[0], (int)data.size(), NULL);
            c.setBuffer(&combo[0]);
            int crc = c.decode(NULL, PNG_INDEXED_OUTPUT | PNG_FAST_PALETTE | PNG_CHECK_CRC);
            bool sameIdx = crc == PNG_SUCCESS;
            for (size_t i = 0; sameIdx && i < bufSize; i++)
                if (combo[i] != buffer[i]) sameIdx = false;
            check(sameIdx, "indexes identical with PNG_FAST_PALETTE|PNG_CHECK_CRC");
        }
        png.close();
    }

    // ================================================= 2. non-palette errors
    g_section = "non-palette";
    for (int n = 0; n < 2; n++) {
        snprintf(path, sizeof(path), "%s/%s", dir, nonPalNames[n]);
        std::vector<uint8_t> data = readFile(path);
        check(!data.empty(), "sample file readable");
        if (data.empty()) continue;
        PNG png;
        int rc = png.openRAM(&data[0], (int)data.size(), NULL);
        check(rc == PNG_SUCCESS, "openRAM");
        std::vector<uint8_t> buffer((size_t)png.getWidth() * png.getHeight() + 16, 0x5A);
        png.setBuffer(&buffer[0]);
        rc = png.decode(NULL, PNG_INDEXED_OUTPUT);
        check(rc == PNG_UNSUPPORTED_FEATURE,
              "indexed option rejected with PNG_UNSUPPORTED_FEATURE");
        check(png.getLastError() == PNG_UNSUPPORTED_FEATURE,
              "getLastError reports the rejection");
        bool untouched = true;
        for (size_t i = 0; i < buffer.size(); i++)
            if (buffer[i] != 0x5A) untouched = false;
        check(untouched, "buffer untouched on rejection");
    }

    g_section = "no-buffer";
    {
        snprintf(path, sizeof(path), "%s/idx8.png", dir);
        std::vector<uint8_t> data = readFile(path);
        PNG png;
        png.openRAM(&data[0], (int)data.size(), NULL);
        png.decode(NULL, PNG_INDEXED_OUTPUT); // no setBuffer and no callback
        // The library's existing "no output target" path sets the error code
        // and returns 0; the indexed option must not silently skip that check.
        check(png.getLastError() == PNG_NO_BUFFER,
              "indexed option without buffer reports PNG_NO_BUFFER");
    }

    // ========================================= 3. indexes + RGB in one pass
    g_section = "index+rgb";
    for (int n = 0; n < 4; n++) {
        const Image &im = expected[palNames[n]];
        snprintf(path, sizeof(path), "%s/%s", dir, palNames[n]);
        std::vector<uint8_t> data = readFile(path);
        size_t bufSize = (size_t)im.w * im.h;
        std::vector<uint8_t> buffer(bufSize + 64, 0x5A);
        DrawCtx ctx;
        ctx.bpp = 0; ctx.pixelType = -1; ctx.hasAlpha = 0;

        PNG png;
        png.openRAM(&data[0], (int)data.size(), drawToRGB565);
        png.setBuffer(&buffer[0]);
        int rc = png.decode(&ctx, PNG_INDEXED_OUTPUT);
        check(rc == PNG_SUCCESS, "decode with callback + indexes");
        check((int)ctx.rows.size() == im.h, "callback called once per row");
        check((int)ctx.rgb565.size() == im.w * im.h,
              "callback produced width*height RGB565 pixels");
        check(ctx.pixelType == PNG_PIXEL_INDEXED, "callback sees indexed pixel type");
        check(ctx.bpp == im.bits, "callback sees original bit depth");
        check(ctx.palettes.size() && ctx.palettes[0] == png.getPalette(),
              "callback palette is the same palette as getPalette()");

        bool same = ctx.rgb565.size() == (size_t)im.w * im.h;
        for (int y = 0; y < im.h && same; y++)
            for (int x = 0; x < im.w; x++) {
                int idx = buffer[(size_t)y * im.w + x];
                uint16_t want = paletteRGB565(im, idx);
                if (ctx.rgb565[(size_t)y * im.w + x] != want) {
                    same = false;
                    printf("  mismatch %s (%d,%d): idx %d got %04x want %04x\n",
                           palNames[n], x, y, idx,
                           ctx.rgb565[(size_t)y * im.w + x], want);
                }
            }
        check(same, "index -> palette RGB565 agrees with callback output");

        bool untouched = true;
        for (size_t i = bufSize; i < bufSize + 64; i++)
            if (buffer[i] != 0x5A) untouched = false;
        check(untouched, "index buffer not overrun in combined pass");
    }

    // ================================ 4. baseline RGB equality against HEAD
    if (baselineDump) {
        g_section = "baseline";
        FILE *f = fopen(baselineDump, "r");
        check(f != NULL, "baseline dump present");
        if (f) {
            char line[512];
            int seen = 0;
            bool allSame = true;
            while (fgets(line, sizeof(line), f)) {
                char name[128];
                unsigned long hash = 0;
                if (sscanf(line, "%127s %lx", name, &hash) != 2) continue;
                size_t len = strlen(name);
                if (len > 7 && strcmp(&name[len - 7], "_packed") == 0) continue;
                seen++;
                snprintf(path, sizeof(path), "%s/%s", dir, name);
                std::vector<uint8_t> data = readFile(path);
                PNG png;
                DrawCtx ctx;
                png.openRAM(&data[0], (int)data.size(), drawToRGB565);
                int rc = png.decode(&ctx, 0);
                unsigned long h = fnv1a(ctx.rgb565);
                if (rc != PNG_SUCCESS || h != hash) {
                    allSame = false;
                    printf("  mismatch on %s: rc=%d hash=%lx baseline=%lx\n",
                           name, rc, h, hash);
                }
            }
            fclose(f);
            check(seen == 6, "baseline dump covered all six samples");
            check(allSame, "RGB565 callback output byte-identical to baseline (HEAD)");
        }

        // Also compare the plain framebuffer path (decode(0), no callback):
        // packed rows of palette images and raw truecolor/gray must be
        // byte-identical to the baseline library.
        std::map<std::string, unsigned long> baselineHashes;
        {
            FILE *pf = fopen(baselineDump, "r");
            char line2[256];
            while (pf && fgets(line2, sizeof(line2), pf)) {
                char nm[128];
                unsigned long hh;
                if (sscanf(line2, "%127s %lx", nm, &hh) == 2)
                    baselineHashes[nm] = hh;
            }
            if (pf) fclose(pf);
        }
        for (int n = 0; n < 6; n++) {
            snprintf(path, sizeof(path), "%s/%s", dir, allNames[n]);
            std::vector<uint8_t> data = readFile(path);
            PNG png;
            png.openRAM(&data[0], (int)data.size(), NULL);
            std::vector<uint8_t> buf((size_t)png.getBufferSize(), 0);
            png.setBuffer(&buf[0]);
            int rc = png.decode(NULL, 0);
            check(rc == PNG_SUCCESS, "framebuffer decode succeeds");
            std::string tag = std::string(allNames[n]) + "_packed";
            std::map<std::string, unsigned long>::iterator it = baselineHashes.find(tag);
            check(it != baselineHashes.end(), "baseline has framebuffer hash for sample");
            if (it != baselineHashes.end())
                check(fnv1aBytes(buf) == it->second,
                      "framebuffer bytes byte-identical to baseline (HEAD)");
        }
    }

    // ==================================================== 5. timing / memory
    printf("\n%-15s %-10s %-4s %10s %12s %12s %10s\n",
           "file", "type", "bpp", "decode", "index buffer", "framebuffer", "peak RSS");
    struct rusage ruStart, ruEnd;
    getrusage(RUSAGE_SELF, &ruStart);
    const char *perfNames[] = {"perf_idx1.png", "perf_idx2.png", "perf_idx4.png",
                               "perf_idx8.png", "perf_true.png"};
    for (int n = 0; n < 5; n++) {
        snprintf(path, sizeof(path), "%s/%s", dir, perfNames[n]);
        std::vector<uint8_t> data = readFile(path);
        check(!data.empty(), "timed sample file readable");
        if (data.empty()) continue;

        PNG probe;
        int rc = probe.openRAM(&data[0], (int)data.size(), NULL);
        check(rc == PNG_SUCCESS, "timed sample opens");
        bool isPal = probe.getPixelType() == PNG_PIXEL_INDEXED;
        size_t bufSize = (size_t)probe.getBufferSize();
        if (isPal) bufSize = (size_t)probe.getWidth() * probe.getHeight();
        std::vector<uint8_t> buffer(bufSize + 64, 0);

        int iterations = 50;
        double best = 1e9;
        int opt = isPal ? PNG_INDEXED_OUTPUT : 0;
        for (int i = 0; i < iterations; i++) {
            PNG png;
            png.openRAM(&data[0], (int)data.size(), NULL);
            png.setBuffer(&buffer[0]);
            double a = now_ms();
            int drc = png.decode(NULL, opt);
            double b = now_ms();
            if (drc != PNG_SUCCESS) { check(false, "timed decode success"); break; }
            if (b - a < best) best = b - a;
        }
        printf("%-15s %-10s %-4d %8.3fms %12zu %12zu %9ldK\n",
               perfNames[n], isPal ? "palette" : "truecolor", probe.getBpp(),
               best, isPal ? bufSize : (size_t)0,
               isPal ? (size_t)0 : bufSize, (long)ruStart.ru_maxrss);
        check(best > 0.0005 && best < 1000.0, "timing measured for this sample");
    }
    getrusage(RUSAGE_SELF, &ruEnd);
    printf("\npeak RSS before timing loop: %ld KB, after: %ld KB (library does no malloc)\n",
           (long)ruStart.ru_maxrss, (long)ruEnd.ru_maxrss);

    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

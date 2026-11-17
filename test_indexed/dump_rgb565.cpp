//
// dump_rgb565.cpp - prints "name hash" for each sample
//
// Built against the unmodified library (git HEAD) by run.sh. Two lines per
// sample so driver.cpp can prove the modified tree's RGB output is unchanged:
//   <name>         FNV-1a over RGB565 produced by getLineAsRGB565() per row
//   <name>_packed  FNV-1a over the decode(0) framebuffer (packed/raw bytes)
//
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>

#include "PNGdec.h"

static std::vector<uint16_t> g_rgb;
static PNG g_png;

static int cb(PNGDRAW *pDraw)
{
    // The 1/2/4-bit palette paths of PNGRGB565() work on pixel quads/pairs, so
    // the 2-bit path can write up to 3 pixels past iWidth (pre-existing
    // library behaviour); give the test line spare entries.
    std::vector<uint16_t> line((size_t)pDraw->iWidth + 4);
    g_png.getLineAsRGB565(pDraw, &line[0], PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    g_rgb.insert(g_rgb.end(), line.begin(), line.begin() + pDraw->iWidth);
    return 1;
}

static std::vector<uint8_t> slurp(const char *path)
{
    std::vector<uint8_t> d;
    FILE *f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d.resize((size_t)n);
    if (fread(&d[0], 1, (size_t)n, f) != (size_t)n) d.clear();
    fclose(f);
    return d;
}

static unsigned long hashBytes(const uint8_t *p, size_t n)
{
    unsigned long h = 1469598103934665603UL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211UL;
    }
    return h;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "samples";
    const char *names[] = {"idx1.png", "idx2.png", "idx4.png", "idx8.png",
                           "true.png", "gray.png"};
    char path[1024];
    for (int i = 0; i < 6; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        std::vector<uint8_t> data = slurp(path);
        if (data.empty()) return 1;

        g_rgb.clear();
        if (g_png.openRAM(&data[0], (int)data.size(), cb) != PNG_SUCCESS) return 1;
        if (g_png.decode(NULL, 0) != PNG_SUCCESS) return 1;
        g_png.close();
        std::vector<uint8_t> raw(g_rgb.size() * 2);
        for (size_t k = 0; k < g_rgb.size(); k++) {
            raw[k * 2] = (uint8_t)(g_rgb[k] & 0xff);
            raw[k * 2 + 1] = (uint8_t)(g_rgb[k] >> 8);
        }
        printf("%s %lx\n", names[i], hashBytes(&raw[0], raw.size()));

        // framebuffer path: decode into one buffer, no callback
        std::vector<uint8_t> fb((size_t)g_png.getBufferSize(), 0);
        if (g_png.openRAM(&data[0], (int)data.size(), NULL) != PNG_SUCCESS) return 1;
        g_png.setBuffer(&fb[0]);
        if (g_png.decode(NULL, 0) != PNG_SUCCESS) return 1;
        printf("%s_packed %lx\n", names[i], hashBytes(&fb[0], fb.size()));
    }
    return 0;
}

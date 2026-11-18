// Test-only harness (not part of the library).
// Modes:
//   dump_decode dump <file.png> <out.raw>
//       Decode the PNG 8 times, once per config combination
//       (endianness x background x fast-palette option), converting every
//       line with getLineAsRGB565(). Writes a deterministic binary stream:
//       for each config: 4-byte config header + width*height uint16 pixels.
//   dump_decode bench <png_dir> <reps>
//       Decode every PNG listed in <png_dir>/manifest.txt <reps> times
//       (fixed config: LE, bkgd=0x00123456, PNG_FAST_PALETTE) and print
//       total wall-clock nanoseconds.
#include <PNGdec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

static PNG png;
static FILE *g_out;
static int g_endian;
static uint32_t g_bkgd;
static uint16_t g_line[8192];

static int DrawCallback(PNGDRAW *pDraw)
{
    png.getLineAsRGB565(pDraw, g_line, g_endian, g_bkgd);
    if (g_out)
        fwrite(g_line, sizeof(uint16_t), pDraw->iWidth, g_out);
    return 1;
}

static uint8_t *slurp(const char *path, int *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "read error\n"); exit(1); }
    fclose(f);
    *size = (int)n;
    return buf;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "dump")) {
        int size;
        uint8_t *data = slurp(argv[2], &size);
        g_out = fopen(argv[3], "wb");
        if (!g_out) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
        static const uint32_t bkgds[2] = {0xffffffffu, 0x00123456u};
        for (int e = 0; e < 2; e++)
        for (int b = 0; b < 2; b++)
        for (int o = 0; o < 2; o++) {
            int options = o ? PNG_FAST_PALETTE : 0;
            uint8_t hdr[4] = {(uint8_t)e, (uint8_t)b, (uint8_t)o, 0};
            fwrite(hdr, 1, 4, g_out);
            g_endian = e;
            g_bkgd = bkgds[b];
            int rc = png.openRAM(data, size, DrawCallback);
            if (rc != PNG_SUCCESS) { fprintf(stderr, "openRAM failed %d\n", rc); return 1; }
            rc = png.decode(NULL, options);
            if (rc != PNG_SUCCESS) { fprintf(stderr, "decode failed %d\n", rc); return 1; }
            png.close();
        }
        fclose(g_out);
        free(data);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "bench")) {
        int reps = atoi(argv[3]);
        char path[1024];
        snprintf(path, sizeof(path), "%s/manifest.txt", argv[2]);
        FILE *m = fopen(path, "r");
        if (!m) { fprintf(stderr, "no manifest\n"); return 1; }
        std::vector<uint8_t*> imgs;
        std::vector<int> sizes;
        char line[512];
        while (fgets(line, sizeof(line), m)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            snprintf(path, sizeof(path), "%s/%s", argv[2], line);
            int size;
            imgs.push_back(slurp(path, &size));
            sizes.push_back(size);
        }
        fclose(m);
        g_out = NULL;
        g_endian = PNG_RGB565_LITTLE_ENDIAN;
        g_bkgd = 0x00123456u;
        // warmup
        for (size_t i = 0; i < imgs.size(); i++) {
            png.openRAM(imgs[i], sizes[i], DrawCallback);
            png.decode(NULL, PNG_FAST_PALETTE);
            png.close();
        }
        uint64_t t0 = now_ns();
        for (int r = 0; r < reps; r++)
            for (size_t i = 0; i < imgs.size(); i++) {
                png.openRAM(imgs[i], sizes[i], DrawCallback);
                png.decode(NULL, PNG_FAST_PALETTE);
                png.close();
            }
        uint64_t t1 = now_ns();
        printf("%llu\n", (unsigned long long)(t1 - t0));
        return 0;
    }
    fprintf(stderr, "usage: dump_decode dump <in.png> <out.raw> | bench <dir> <reps>\n");
    return 1;
}

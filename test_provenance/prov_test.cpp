//
// prov_test.cpp
//
// Debug-provenance test harness for PNGdec.
// Single translation unit (same pattern as the bundled MacOS/PNG_Test),
// built from the library's own sources - no third-party code.
//
// Build with -DPNG_PROVENANCE=1 for the tracing build, without it for the
// "feature compiled out" build.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifndef __LINUX__
#define __LINUX__
#endif
#define ZLIB_DEBUG
#define INFLATE_STRICT
#include "../src/zutil.c"
#include "../src/inflate.c"
#include "../src/inffast.c"
#include "../src/inftrees.c"
#include "../src/crc32.c"
#undef DO1
#undef DO8
#include "../src/adler32.c"
#include "../src/PNGdec.cpp"

static int g_fail = 0;
static int g_checks = 0;

#if defined(__GNUC__)
#define PROV_UNUSED __attribute__((unused))
#else
#define PROV_UNUSED
#endif

#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static uint8_t *read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *out_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)*out_len);
    if (fread(buf, 1, (size_t)*out_len, f) != (size_t)*out_len) exit(2);
    fclose(f);
    return buf;
}

static uint8_t *read_oracle(const char *dir, const char *name, const char *suffix, long *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s%s", dir, name, suffix);
    return read_file(path, len);
}

typedef struct sample_tag {
    char name[64];
    int w, h, colortype, bpp, interlaced;
} SAMPLE;

static long micros_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static const char *filter_name(int f) PROV_UNUSED;
static const char *filter_name(int f)
{
    static const char *names[5] = {"None", "Sub", "Up", "Average", "Paeth"};
    return (f >= 0 && f < 5) ? names[f] : "?";
}

#if PNG_PROVENANCE
//
// Write the pixel-level provenance index map as a binary PPM (P6).
// Raster positions never covered by the decoder stay black.
//
static void write_provenance_ppm(const char *path, int w, int h,
                                 PNGTRACEPIXEL *recs, uint32_t nrec,
                                 int interlaced)
{
    typedef struct { int x, y; } PT;
    static const int sx[7] = {8, 8, 4, 4, 2, 2, 1};
    static const int sy[7] = {8, 8, 8, 4, 4, 2, 2};
    static const int ox[7] = {0, 4, 0, 2, 0, 1, 0};
    static const int oy[7] = {0, 0, 4, 0, 2, 0, 1};
    // filter hue table (R,G,B)
    static const uint8_t fcol[5][3] = {
        {80, 80, 80}, {220, 60, 60}, {60, 180, 60},
        {60, 90, 230}, {220, 170, 40}
    };
    uint8_t *img = (uint8_t *)calloc((size_t)w * h * 3, 1);
    PT *coords = (PT *)malloc((size_t)w * h * sizeof(PT));
    uint32_t idx = 0;
    if (interlaced) {
        for (int p = 0; p < 7; p++) {
            for (int fy = oy[p]; fy < h; fy += sy[p])
                for (int fx = ox[p]; fx < w; fx += sx[p])
                    if (idx < (uint32_t)w * h) coords[idx++] = (PT){fx, fy};
        }
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                coords[idx++] = (PT){x, y};
    }
    for (uint32_t i = 0; i < nrec && i < idx; i++) {
        int off = (coords[i].y * w + coords[i].x) * 3;
        int f = recs[i].ucFilter;
        if (f < 0 || f >= 5) f = 0;
        img[off + 0] = fcol[f][0];
        img[off + 1] = fcol[f][1];
        img[off + 2] = fcol[f][2];
        if (recs[i].ucTransparent) { // mark transparent pixels white
            img[off + 0] = img[off + 1] = img[off + 2] = 255;
        }
    }
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n# PNGdec provenance: filter color, white=transparent, "
               "black=not emitted\n%d %d\n255\n", w, h);
    fwrite(img, 1, (size_t)w * h * 3, f);
    fclose(f);
    free(img);
    free(coords);
}

//
// Adam7 pass index map as a binary PPM (P6): one distinct color per pass.
//
static void write_pass_ppm(const char *path, int w, int h,
                           PNGTRACEPIXEL *recs, uint32_t nrec, int interlaced)
{
    typedef struct { int x, y; } PT;
    static const int sx[7] = {8, 8, 4, 4, 2, 2, 1};
    static const int sy[7] = {8, 8, 8, 4, 4, 2, 2};
    static const int ox[7] = {0, 4, 0, 2, 0, 1, 0};
    static const int oy[7] = {0, 0, 4, 0, 2, 0, 1};
    static const uint8_t pcol[8][3] = {
        {40, 40, 40}, {230, 50, 50}, {230, 150, 50}, {230, 230, 50},
        {50, 200, 50}, {50, 200, 220}, {70, 90, 230}, {180, 70, 220}
    };
    uint8_t *img = (uint8_t *)calloc((size_t)w * h * 3, 1);
    PT *coords = (PT *)malloc((size_t)w * h * sizeof(PT));
    uint32_t idx = 0;
    if (interlaced) {
        for (int p = 0; p < 7; p++) {
            for (int fy = oy[p]; fy < h; fy += sy[p])
                for (int fx = ox[p]; fx < w; fx += sx[p])
                    if (idx < (uint32_t)w * h) coords[idx++] = (PT){fx, fy};
        }
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                coords[idx++] = (PT){x, y};
    }
    for (uint32_t i = 0; i < nrec && i < idx; i++) {
        int off = (coords[i].y * w + coords[i].x) * 3;
        int p = recs[i].ucPass;
        if (p < 0 || p > 7) p = 0;
        img[off + 0] = pcol[p][0];
        img[off + 1] = pcol[p][1];
        img[off + 2] = pcol[p][2];
    }
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n# PNGdec Adam7 pass index (1..7), black=not emitted\n"
               "%d %d\n255\n", w, h);
    fwrite(img, 1, (size_t)w * h * 3, f);
    fclose(f);
    free(img);
    free(coords);
}
#endif

static int draw_accept(PNGDRAW *) PROV_UNUSED;
static int draw_accept(PNGDRAW *)
{
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: prov_test <sample_dir> <out_dir>\n");
        return 2;
    }
    const char *sdir = argv[1];
    const char *odir = argv[2];
    char path[512];

    SAMPLE samples[16];
    int nsamp = 0;
    snprintf(path, sizeof(path), "%s/manifest.txt", sdir);
    FILE *mf = fopen(path, "rb");
    if (!mf) { printf("missing manifest\n"); return 2; }
    while (fscanf(mf, " %63[^,],%d,%d,%d,%d,%d", samples[nsamp].name,
                  &samples[nsamp].w, &samples[nsamp].h,
                  &samples[nsamp].colortype, &samples[nsamp].bpp,
                  &samples[nsamp].interlaced) == 6) {
        nsamp++;
    }
    fclose(mf);

    PNG png;
    for (int si = 0; si < nsamp; si++) {
        SAMPLE *s = &samples[si];
        int is_timing = strcmp(s->name, "timing_rgba") == 0;
        printf("== sample %s (%dx%d type=%d bpp=%d interlaced=%d)\n",
               s->name, s->w, s->h, s->colortype, s->bpp, s->interlaced);
        long flen;
        snprintf(path, sizeof(path), "%s/%s.png", sdir, s->name);
        uint8_t *pngbuf = read_file(path, &flen);
        long rawlen;
        uint8_t *raw = read_oracle(sdir, s->name, ".raw", &rawlen);
        long trlen;
        uint8_t *tr = read_oracle(sdir, s->name, ".tr", &trlen);

        int pitch = (int)(rawlen / s->h);
        (void)pitch;
        // calloc: row padding / unused bits stay deterministic (the decoder
        // only writes valid pixel positions, matching the zeroed oracle)
        uint8_t *fb = (uint8_t *)calloc(1, (size_t)rawlen);

        // ---- decode without any tracing; framebuffer must match oracle ----
        CHECK(png.openRAM(pngbuf, (int)flen, NULL) == PNG_SUCCESS, "openRAM");
        png.setBuffer(fb);
        CHECK(png.decode(NULL, 0) == PNG_SUCCESS, "plain decode succeeds");
        CHECK(memcmp(fb, raw, (size_t)rawlen) == 0,
              "framebuffer byte-identical to oracle");
        png.close();

#if PNG_PROVENANCE
        if (!is_timing) {
            // ---- pixel-granularity trace ----
            uint32_t cap = (uint32_t)(s->w * s->h);
            PNGTRACEPIXEL *pix =
                (PNGTRACEPIXEL *)malloc(cap * sizeof(PNGTRACEPIXEL));
            CHECK(PNG_TraceBegin(PNG_TRACE_PIXEL, pix, cap) == 1,
                  "trace begin (pixel)");
            CHECK(png.openRAM(pngbuf, (int)flen, NULL) == PNG_SUCCESS,
                  "re-open for trace");
            png.setBuffer(fb);
            CHECK(png.decode(NULL, 0) == PNG_SUCCESS, "traced decode");
            PNGTRACESTATS st;
            PNG_TraceEnd(&st);
            CHECK(memcmp(fb, raw, (size_t)rawlen) == 0,
                  "output unchanged with tracing ON");
            CHECK(st.u32TotalPixels == (uint32_t)trlen,
                  "stats total pixel count");
            CHECK(st.u32DroppedRecords == 0, "no dropped records");

            // transparent flags match the oracle emission order
            uint32_t tr_expect = 0;
            for (long i = 0; i < trlen; i++) tr_expect += tr[i];
            CHECK(st.u32TransparentPixels == tr_expect,
                  "transparent pixel count");
            int tr_ok = 1;
            for (long i = 0; i < trlen; i++) {
                if (pix[i].ucTransparent != tr[i]) { tr_ok = 0; break; }
            }
            CHECK(tr_ok, "per-pixel transparent flags");

            // every record must carry a valid filter and the expected pass id
            int field_ok = 1;
            for (long i = 0; i < trlen; i++) {
                if (pix[i].ucFilter > 4) { field_ok = 0; break; }
                if (s->interlaced) {
                    if (pix[i].ucPass < 1 || pix[i].ucPass > 7) {
                        field_ok = 0; break;
                    }
                } else if (pix[i].ucPass != 0) {
                    field_ok = 0; break;
                }
            }
            CHECK(field_ok, "filter/pass fields in valid range");

            // cross-check against the *.prov scanline oracle
            snprintf(path, sizeof(path), "%s/%s.prov", sdir, s->name);
            FILE *pf = fopen(path, "rb");
            CHECK(pf != NULL, "prov oracle opens");
            if (pf) {
                int filt, passno, srcrow, npix, line_ok = 1, rec_i = 0;
                while (fscanf(pf, "%d %d %d %d", &filt, &passno, &srcrow,
                              &npix) == 4) {
                    for (int k = 0; k < npix; k++) {
                        if (rec_i >= (int)trlen) { line_ok = 0; break; }
                        PNGTRACEPIXEL *r = &pix[rec_i++];
                        if (r->ucFilter != filt || r->ucPass != passno ||
                            r->usSrcRow != srcrow) {
                            line_ok = 0;
                        }
                    }
                }
                fclose(pf);
                CHECK(line_ok && rec_i == (int)trlen,
                      "filter/pass/srcrow match prov oracle per pixel");
            }

            // filter counts: every filter present somewhere in the oracle
            int seen[5] = {0};
            for (long i = 0; i < trlen; i++) seen[pix[i].ucFilter] = 1;
            if (!s->interlaced) {
                int all5 = 1;
                for (int f = 0; f < 5; f++) if (!seen[f]) all5 = 0;
                CHECK(all5, "non-interlaced sample exercises all 5 filters");
            }
            // filter histogram totals equal pixel count
            uint32_t fsum = 0;
            for (int f = 0; f < 5; f++) fsum += st.u32FilterCount[f];
            CHECK(fsum == st.u32TotalPixels, "filter histogram sums to total");
            uint32_t psum = 0;
            for (int p = 0; p <= 7; p++) psum += st.u32PassCount[p];
            CHECK(psum == st.u32TotalPixels, "pass histogram sums to total");
            if (s->interlaced) {
                int multi = 0;
                for (int p = 1; p <= 7; p++)
                    if (st.u32PassCount[p]) multi++;
                CHECK(multi >= 2, "interlaced pixels span multiple passes");
                CHECK(st.u32PassCount[0] == 0,
                      "interlaced has no pass-0 records");
            } else {
                CHECK(st.u32PassCount[0] == st.u32TotalPixels,
                      "non-interlaced all pass 0");
            }

            // ---- line-granularity trace ----
            uint32_t lcap = (uint32_t)(s->h * 2);
            PNGTRACELINE *lines =
                (PNGTRACELINE *)malloc(lcap * sizeof(PNGTRACELINE));
            PNGTRACESTATS st2;
            CHECK(PNG_TraceBegin(PNG_TRACE_LINE, lines, lcap) == 1,
                  "trace begin (line)");
            CHECK(png.openRAM(pngbuf, (int)flen, NULL) == PNG_SUCCESS,
                  "re-open for line trace");
            png.setBuffer(fb);
            CHECK(png.decode(NULL, 0) == PNG_SUCCESS, "line traced decode");
            PNG_TraceEnd(&st2);
            CHECK(memcmp(fb, raw, (size_t)rawlen) == 0,
                  "output unchanged with line tracing ON");
            CHECK(st2.u32TotalPixels == (uint32_t)trlen,
                  "line-mode stats still per-pixel totals");
            // expected number of lines from the prov oracle
            snprintf(path, sizeof(path), "%s/%s.prov", sdir, s->name);
            pf = fopen(path, "rb");
            int expect_lines = 0, lfilt, lpass, lrow, lnp;
            while (pf && fscanf(pf, "%d %d %d %d", &lfilt, &lpass, &lrow,
                                &lnp) == 4)
                expect_lines++;
            if (pf) fclose(pf);
            CHECK(st2.u32DroppedRecords == 0,
                  "line buffer large enough, no drops");
            // count used records by re-running with a capacity of one
            PNGTRACELINE smallbuf[1];
            CHECK(PNG_TraceBegin(PNG_TRACE_LINE, smallbuf, 1) == 1,
                  "tiny trace begin");
            png.openRAM(pngbuf, (int)flen, NULL);
            png.setBuffer(fb);
            png.decode(NULL, 0);
            PNGTRACESTATS st3;
            PNG_TraceEnd(&st3);
            CHECK(st3.u32DroppedRecords == (uint32_t)(expect_lines - 1),
                  "overflow reports dropped line records");
            CHECK(PNG_TraceBegin(PNG_TRACE_PIXEL + 9, pix, cap) == 0,
                  "bad granularity rejected");
            CHECK(PNG_TraceBegin(PNG_TRACE_PIXEL, NULL, cap) == 0,
                  "null buffer rejected");

            // provenance index map (PPM) for the four core categories
            if (strcmp(s->name, "filters_gray8") == 0 ||
                strcmp(s->name, "interlace_rgb") == 0 ||
                strcmp(s->name, "palette8_trns") == 0 ||
                strcmp(s->name, "alpha_rgba") == 0 ||
                strcmp(s->name, "interlace_pal2") == 0) {
                snprintf(path, sizeof(path), "%s/%s_provenance.ppm", odir,
                         s->name);
                write_provenance_ppm(path, s->w, s->h, pix, (uint32_t)trlen,
                                     s->interlaced);
                if (s->interlaced) {
                    snprintf(path, sizeof(path), "%s/%s_passes.ppm", odir,
                             s->name);
                    write_pass_ppm(path, s->w, s->h, pix, (uint32_t)trlen,
                                   s->interlaced);
                }
            }

            // stats table file
            snprintf(path, sizeof(path), "%s/%s_stats.txt", odir, s->name);
            FILE *sf = fopen(path, "wb");
            fprintf(sf, "sample=%s %dx%d type=%d bpp=%d interlaced=%d\n",
                    s->name, s->w, s->h, s->colortype, s->bpp,
                    s->interlaced);
            fprintf(sf, "total_pixels=%u transparent=%u dropped=%u\n",
                    st.u32TotalPixels, st.u32TransparentPixels,
                    st.u32DroppedRecords);
            for (int f = 0; f < 5; f++)
                fprintf(sf, "filter[%s]=%u\n", filter_name(f),
                        st.u32FilterCount[f]);
            for (int p = 0; p <= 7; p++)
                if (st.u32PassCount[p])
                    fprintf(sf, "pass[%d]=%u\n", p, st.u32PassCount[p]);
            fclose(sf);
            free(pix);
            free(lines);
        }

        // ---- timing + memory overhead on the larger sample ----
        if (is_timing) {
            const int ITERS = 200, TRIALS = 3;
            long plain_us = (1L << 60), line_us = (1L << 60);
            long traced_us = (1L << 60);
            uint32_t cap = (uint32_t)(s->w * s->h);
            PNGTRACEPIXEL *pix =
                (PNGTRACEPIXEL *)malloc(cap * sizeof(PNGTRACEPIXEL));
            for (int tr = 0; tr < TRIALS; tr++) {
                long t0 = micros_now();
                for (int it = 0; it < ITERS; it++) {
                    png.openRAM(pngbuf, (int)flen, NULL);
                    png.setBuffer(fb);
                    png.decode(NULL, 0);
                    png.close();
                }
                long dt = micros_now() - t0;
                if (dt < plain_us) plain_us = dt;

                t0 = micros_now();
                for (int it = 0; it < ITERS; it++) {
                    PNG_TraceBegin(PNG_TRACE_PIXEL, pix, cap);
                    png.openRAM(pngbuf, (int)flen, NULL);
                    png.setBuffer(fb);
                    png.decode(NULL, 0);
                    png.close();
                    PNG_TraceEnd(NULL);
                }
                dt = micros_now() - t0;
                if (dt < traced_us) traced_us = dt;

                t0 = micros_now();
                for (int it = 0; it < ITERS; it++) {
                    PNG_TraceBegin(PNG_TRACE_LINE, pix, cap);
                    png.openRAM(pngbuf, (int)flen, NULL);
                    png.setBuffer(fb);
                    png.decode(NULL, 0);
                    png.close();
                    PNG_TraceEnd(NULL);
                }
                dt = micros_now() - t0;
                if (dt < line_us) line_us = dt;
            }
            printf("  TIMING best of %d trials, %d iters each: plain=%ldus "
                   "line_trace=%ldus pixel_trace=%ldus\n",
                   TRIALS, ITERS, plain_us, line_us, traced_us);
            printf("  TIMING per decode:    plain=%.1fus line=%.1fus "
                   "pixel=%.1fus (line x%.2f, pixel x%.2f)\n",
                   plain_us / (double)ITERS, line_us / (double)ITERS,
                   traced_us / (double)ITERS,
                   line_us / (double)plain_us,
                   traced_us / (double)plain_us);
            printf("  MEMORY: trace record sizes line=%zu pixel=%zu; "
                   "full-frame pixel table=%u bytes for %dx%d\n",
                   sizeof(PNGTRACELINE), sizeof(PNGTRACEPIXEL),
                   cap * (uint32_t)sizeof(PNGTRACEPIXEL), s->w, s->h);
            free(pix);
        }

        // ---- interlaced + line callback must be rejected cleanly ----
        if (s->interlaced) {
            CHECK(png.openRAM(pngbuf, (int)flen, draw_accept) == PNG_SUCCESS,
                  "interlaced opens in callback mode");
            png.setBuffer(NULL);
            int drc = png.decode(NULL, 0);
            // the decoder reports fatal errors through getLastError()
            CHECK(drc == PNG_UNSUPPORTED_FEATURE ||
                  png.getLastError() == PNG_UNSUPPORTED_FEATURE,
                  "interlaced callback decode rejected without framebuffer");
            png.close();
        }
#else
        printf("  (provenance compiled out: PNG_PROVENANCE=0)\n");
        // verify the feature truly adds nothing: repeated decodes stay equal
        for (int rep = 0; rep < 3; rep++) {
            png.openRAM(pngbuf, (int)flen, NULL);
            png.setBuffer(fb);
            png.decode(NULL, 0);
            png.close();
            CHECK(memcmp(fb, raw, (size_t)rawlen) == 0,
                  "oracle match with provenance compiled out");
        }
        if (is_timing) {
            const int ITERS = 200;
            long t0 = micros_now();
            for (int it = 0; it < ITERS; it++) {
                png.openRAM(pngbuf, (int)flen, NULL);
                png.setBuffer(fb);
                png.decode(NULL, 0);
                png.close();
            }
            long plain_us = micros_now() - t0;
            printf("  TIMING over %d iters: plain=%ldus (%.1fus/decode)\n",
                   ITERS, plain_us, plain_us / (double)ITERS);
            printf("  MEMORY: no trace state is linked into this build\n");
        }
#endif
        free(pngbuf);
        free(raw);
        free(tr);
        free(fb);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

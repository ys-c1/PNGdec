//
// cache_bench.cpp - header cache benchmark and correctness checks
//
// Build/run through run.sh. Includes the library sources directly, exactly
// like the repository's own MacOS/PNG_Test harness; no build system changes.
//
// Scenarios:
//   baseline  every frame is a unique buffer (100% miss) - old behavior
//   S1        same file decoded frame after frame
//   S2       two files alternating
//   S3       file content changed in place (fingerprint invalidation)
//   S4       cache full (eviction), small vs. fitting capacity, LRU/FIFO
//   threads  several threads sharing one read-only cache
//   zero      capacity 0 and other invalid parameters must be rejected
//
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include <string>
#include <pthread.h>
#include <sys/resource.h>

#define ZLIB_DEBUG
#define verbose 0
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
#include "legacy_shim.h"

// Cache control functions added to png.inl (C linkage, appended API only;
// no existing public signature changed). Legacy builds get no-op stand-ins.
#ifndef PNG_CACHE_LEGACY
extern "C" {
int  PNG_cache_configure(int iMaxEntries, int iPolicy, int iThreads);
void PNG_cache_reset(void);
int  PNG_cache_stats(int *pHits, int *pMisses, int *pParses,
                     int *pEvictions, int *pEntries, int *pCapacity);
}
#endif
#ifndef PNG_CACHE_LRU
#define PNG_CACHE_LRU 0
#define PNG_CACHE_FIFO 1
#endif

static const int N = 4000; // frames per scenario

static double Micros(void)
{
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return (double)res.tv_sec * 1e6 + (double)res.tv_nsec / 1000.0;
}

static std::vector<uint8_t> LoadFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v(n);
    if (fread(v.data(), 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f);
    return v;
}

static __thread int g_lines; // per-thread line counter (keeps the TSan run race-free)
static int DrawCB(PNGDRAW *pDraw)
{
    g_lines += pDraw->iWidth > 0 ? 1 : 0;
    return 1;
}

// One full open+decode frame using the line-callback path.
static int FrameCB(PNG &png, uint8_t *data, int size)
{
    int rc = png.openRAM(data, size, DrawCB);
    if (rc != PNG_SUCCESS) return rc;
    rc = png.decode(NULL, 0);
    png.close();
    return rc;
}

// One frame decoded into a caller buffer (for pixel-exact comparisons).
static int FrameBuf(PNG &png, uint8_t *data, int size, std::vector<uint8_t> &out)
{
    int rc = png.openRAM(data, size, NULL);
    if (rc != PNG_SUCCESS) return rc;
    out.resize(png.getBufferSize());
    png.setBuffer(out.data());
    rc = png.decode(NULL, 0);
    png.close();
    return rc;
}

// Locate a mutable payload byte inside the tEXt chunk.
static int TextPayloadOffset(const std::vector<uint8_t> &buf)
{
    const char *needle = "Comment\x00";
    for (size_t i = 0; i + 8 < buf.size(); i++) {
        if (memcmp(&buf[i], needle, 8) == 0)
            return (int)i + 8;
    }
    return -1;
}

// Offset of a chunk's first data byte.
static int ChunkDataOffset(const std::vector<uint8_t> &buf, uint32_t type)
{
    size_t off = 8;
    while (off + 8 <= buf.size()) {
        uint32_t len = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
                       ((uint32_t)buf[off + 2] << 8) | buf[off + 3];
        uint32_t t = ((uint32_t)buf[off + 4] << 24) | ((uint32_t)buf[off + 5] << 16) |
                     ((uint32_t)buf[off + 6] << 8) | buf[off + 7];
        if (off + 8 + len + 4 > buf.size()) return -1;
        if (t == type) return (int)(off + 8);
        off += 8 + len + 4;
    }
    return -1;
}

// Locate the CRC byte range of a chunk of the given 4CC; returns the offset
// of the first CRC byte, or -1 if absent.
static int ChunkCrcOffset(const std::vector<uint8_t> &buf, uint32_t type)
{
    size_t off = 8;
    while (off + 8 <= buf.size()) {
        uint32_t len = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
                       ((uint32_t)buf[off + 2] << 8) | buf[off + 3];
        uint32_t t = ((uint32_t)buf[off + 4] << 24) | ((uint32_t)buf[off + 5] << 16) |
                     ((uint32_t)buf[off + 6] << 8) | buf[off + 7];
        if (off + 8 + len + 4 > buf.size()) return -1;
        if (t == type) return (int)(off + 8 + len);
        off += 8 + len + 4;
    }
    return -1;
}

struct Stats { int hits, misses, parses, evictions, entries, capacity; };
static Stats GetStats(void)
{
    Stats s;
    PNG_cache_stats(&s.hits, &s.misses, &s.parses, &s.evictions,
                    &s.entries, &s.capacity);
    return s;
}

static void Report(const char *name, int frames, double us, const Stats &s)
{
    int lookups = s.hits + s.misses;
    double hitpct = lookups ? 100.0 * s.hits / lookups : 0.0;
    printf("%-28s frames=%-5d lookups=%-5d parses=%-5d hits=%-5d "
           "misses=%-5d evict=%-4d entries=%d/%d hit=%6.2f%% "
           "time=%8.2fms (%7.3fus/frame)\n",
           name, frames, lookups, s.parses, s.hits, s.misses,
           s.evictions, s.entries, s.capacity, hitpct,
           us / 1000.0, us / frames);
}

static void Check(int cond, const char *what)
{
    if (!cond) {
        printf("ASSERT FAILED: %s\n", what);
        exit(1);
    }
    printf("  [ok] %s\n", what);
}

//
// Timing-only loops, identical for the cached and legacy (pre-change) builds.
// No cache-stat assertions, so it runs against either library implementation.
//
static void RunPerf(const std::string &dir)
{
    auto a = LoadFile((dir + "/a_rgb.png").c_str());
    auto b = LoadFile((dir + "/b_pal8.png").c_str());
    int plteOff = ChunkDataOffset(b, 0x504c5445U /*'PLTE'*/);
    if (plteOff <= 0) { printf("missing PLTE byte\n"); exit(2); }
    PNG png;
    double t0, t1;

    // legacy-vs-new baseline: distinct cached-field content per frame, so the
    // cached build cannot get a hit either - pure parse+decode cost both sides
    PNG_cache_configure(16, PNG_CACHE_LRU, 1);
    auto scratch = b;
    t0 = Micros();
    for (int i = 0; i < N; i++) {
        scratch[plteOff] = (uint8_t)(i & 0xff);
        scratch[plteOff + 1] = (uint8_t)((i >> 8) & 0x0f);
        if (FrameCB(png, scratch.data(), (int)scratch.size()) != PNG_SUCCESS) exit(2);
    }
    t1 = Micros();
    printf("PERF %-26s %8.3f us/frame (%d frames)\n",
           "all-miss/legacy-equiv", (t1 - t0) / N, N);

    // S1: same file repeatedly
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    t0 = Micros();
    for (int i = 0; i < N; i++)
        if (FrameCB(png, b.data(), (int)b.size()) != PNG_SUCCESS) exit(2);
    t1 = Micros();
    printf("PERF %-26s %8.3f us/frame (%d frames)\n",
           "S1 same file", (t1 - t0) / N, N);

    // S2: two files alternating
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    t0 = Micros();
    for (int i = 0; i < N; i++) {
        auto &cur = (i & 1) ? b : a;
        if (FrameCB(png, cur.data(), (int)cur.size()) != PNG_SUCCESS) exit(2);
    }
    t1 = Micros();
    printf("PERF %-26s %8.3f us/frame (%d frames)\n",
           "S2 alternating 2 files", (t1 - t0) / N, N);

    // S3: one in-place PLTE change halfway through
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    auto work = b;
    t0 = Micros();
    for (int i = 0; i < N; i++) {
        if (i == N / 2) work[plteOff] ^= 0x5a;
        if (FrameCB(png, work.data(), (int)work.size()) != PNG_SUCCESS) exit(2);
    }
    t1 = Micros();
    printf("PERF %-26s %8.3f us/frame (%d frames)\n",
           "S3 changed in place", (t1 - t0) / N, N);

    // open-only: header phase in isolation
    const int M = 50000;
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    auto mscratch = b;
    t0 = Micros();
    for (int i = 0; i < M; i++) {
        mscratch[plteOff] = (uint8_t)i;
        if (png.openRAM(mscratch.data(), (int)mscratch.size(), DrawCB) != PNG_SUCCESS) exit(2);
        png.close();
    }
    t1 = Micros();
    printf("PERF %-26s %8.3f us/op (%d ops)\n",
           "open() full parse", (t1 - t0) / M, M);
    PNG_cache_reset();
    t0 = Micros();
    for (int i = 0; i < M; i++) {
        if (png.openRAM(b.data(), (int)b.size(), DrawCB) != PNG_SUCCESS) exit(2);
        png.close();
    }
    t1 = Micros();
    printf("PERF %-26s %8.3f us/op (%d ops)\n",
           "open() repeated", (t1 - t0) / M, M);

    // header-phase scaling across sample sizes (open only, no decode)
    const char *sizes[2] = {"b_pal8.png", "e_pal8_big.png"};
    for (int k = 0; k < 2; k++) {
        auto f = LoadFile((dir + "/" + sizes[k]).c_str());
        char label[64];
        PNG_cache_reset();
        t0 = Micros();
        for (int i = 0; i < M; i++) {
            if (png.openRAM(f.data(), (int)f.size(), DrawCB) != PNG_SUCCESS) exit(2);
            png.close();
        }
        t1 = Micros();
        snprintf(label, sizeof(label), "open() parse %4dB", (int)f.size());
        printf("PERF %-26s %8.3f us/op (%d ops)\n", label, (t1 - t0) / M, M);
        PNG_cache_reset();
        t0 = Micros();
        for (int i = 0; i < M; i++) {
            if (png.openRAM(f.data(), (int)f.size(), DrawCB) != PNG_SUCCESS) exit(2);
            png.close();
        }
        t1 = Micros();
        Stats ss = GetStats();
        snprintf(label, sizeof(label), "open() cached %3dB", (int)f.size());
        printf("PERF %-26s %8.3f us/op (hits %d/%d)\n",
               label, (t1 - t0) / M, ss.hits, ss.hits + ss.misses);
    }
}

// open-vs-decode cost split on a real, larger file passed by path (optional).
static void RunRealImage(const char *path)
{
    auto f = LoadFile(path);
    PNG png;
    const int K = 2000;
    double t0, t1;

    PNG_cache_reset();
    t0 = Micros();
    for (int i = 0; i < K; i++) {
        if (png.openRAM(f.data(), (int)f.size(), DrawCB) != PNG_SUCCESS) exit(2);
        png.close();
    }
    t1 = Micros();
    double openParse = (t1 - t0) / K;

    PNG_cache_reset();
    png.openRAM(f.data(), (int)f.size(), DrawCB); // prime the entry
    png.close();
    t0 = Micros();
    for (int i = 0; i < K; i++) {
        if (png.openRAM(f.data(), (int)f.size(), DrawCB) != PNG_SUCCESS) exit(2);
        png.close();
    }
    t1 = Micros();
    double openCached = (t1 - t0) / K;
    Stats rs = GetStats();

    t0 = Micros();
    for (int i = 0; i < K; i++) {
        if (png.openRAM(f.data(), (int)f.size(), DrawCB) != PNG_SUCCESS) exit(2);
        if (png.decode(NULL, 0) != PNG_SUCCESS) exit(2);
        png.close();
    }
    t1 = Micros();
    double fullFrame = (t1 - t0) / K;
    printf("REAL %-26s %8.3f us/op  (file %d bytes, open hits %d/%d)\n",
           "real open() parse", openParse, (int)f.size(), 0, 0);
    printf("REAL %-26s %8.3f us/op  (hits %d/%d)\n",
           "real open() cached", openCached, rs.hits, rs.hits + rs.misses);
    printf("REAL %-26s %8.3f us/frame (open+decode)\n",
           "real full frame", fullFrame);
    printf("REAL header-phase share of full frame: parse %.1f%%, cached %.1f%%\n",
           100.0 * openParse / fullFrame, 100.0 * openCached / fullFrame);
}

static void RunAll(const std::string &dir)
{
    auto a = LoadFile((dir + "/a_rgb.png").c_str());
    auto b = LoadFile((dir + "/b_pal8.png").c_str());
    auto c = LoadFile((dir + "/c_gray.png").c_str());
    auto d = LoadFile((dir + "/d_pal4.png").c_str());
    int textOff = TextPayloadOffset(a);
    Check(textOff > 0, "tEXt payload byte located for non-invalidation tests");
    int plteOff = ChunkDataOffset(b, 0x504c5445U /*'PLTE'*/);
    Check(plteOff > 0, "PLTE payload byte located for fingerprint tests");

    PNG png;
    double t0, t1;

    // ---- correctness: miss frame and hit frame decode pixel-identical ----
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    std::vector<uint8_t> px1, px2;
    Check(FrameBuf(png, b.data(), (int)b.size(), px1) == PNG_SUCCESS, "palette PNG decodes (miss)");
    Check(FrameBuf(png, b.data(), (int)b.size(), px2) == PNG_SUCCESS, "palette PNG decodes (hit)");
    Check(px1 == px2, "cached and uncached pixel output are identical");
    Check(png.openRAM(b.data(), (int)b.size(), DrawCB) == PNG_SUCCESS, "reopen palette PNG");
    uint8_t palBefore[1024];
    memcpy(palBefore, png.getPalette(), 1024);
    png.close();
    Check(FrameCB(png, b.data(), (int)b.size()) == PNG_SUCCESS, "callback decode path ok");
    Check(png.openRAM(b.data(), (int)b.size(), DrawCB) == PNG_SUCCESS, "reopen for palette compare");
    Check(memcmp(palBefore, png.getPalette(), 1024) == 0, "palette (PLTE+tRNS) restored from cache");
    // openFLASH takes the same cached path
    Check(png.openFLASH(a.data(), (int)a.size(), DrawCB) == PNG_SUCCESS, "openFLASH shares the cache");
    g_lines = 0;
    Check(png.decode(NULL, 0) == PNG_SUCCESS && g_lines == 12, "FLASH decode produces all 12 lines");
    png.close();
    PNG_cache_reset();

    // ---- baseline: unique header content every frame (equivalent to old behavior) ----
    // Capacity at its maximum: every unique PLTE edit still misses, so this
    // measures the pure parse+decode cost the cache is meant to remove.
    PNG_cache_configure(16, PNG_CACHE_LRU, 1);
    auto scratch = b;
    t0 = Micros();
    for (int i = 0; i < N; i++) {
        scratch[plteOff] = (uint8_t)(i & 0xff);       // unique cached-field
        scratch[plteOff + 1] = (uint8_t)((i >> 8) & 0x0f); // content each frame
        Check(FrameCB(png, scratch.data(), (int)scratch.size()) == PNG_SUCCESS,
              "baseline frame decodes");
    }
    t1 = Micros();
    Stats sb = GetStats();
    Report("baseline all-miss", N, t1 - t0, sb);
    Check(sb.parses == N && sb.hits == 0,
          "baseline: every unique frame parsed, zero cache hits");
    double baselineUs = (t1 - t0) / N;

    // ---- S1: same file, consecutive frames ----
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    g_lines = 0;
    t0 = Micros();
    for (int i = 0; i < N; i++)
        Check(FrameCB(png, b.data(), (int)b.size()) == PNG_SUCCESS, "S1 frame decodes");
    t1 = Micros();
    Stats s1 = GetStats();
    Report("S1 same file", N, t1 - t0, s1);
    Check(s1.parses == 1 && s1.hits == N - 1, "S1: exactly 1 parse, all later frames hit");
    double s1Us = (t1 - t0) / N;
    printf("  --> open/parse+decode per-frame time: baseline %.3f us -> cached %.3f us "
           "(%.2fx faster, saved %.3f us/frame)\n",
           baselineUs, s1Us, baselineUs / s1Us, baselineUs - s1Us);

    // ---- open-only microbenchmark: isolates header parse cost (no decode) ----
    const int M = 50000;
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    auto mscratch = b;
    t0 = Micros();
    for (int i = 0; i < M; i++) {
        mscratch[plteOff] = (uint8_t)i; // unique cached-field content: full parse
        Check(png.openRAM(mscratch.data(), (int)mscratch.size(), DrawCB) == PNG_SUCCESS,
              "micro baseline open");
        png.close();
    }
    t1 = Micros();
    double parseOpenUs = (t1 - t0) / M;
    printf("  open() with full header parse: %.3f us/op\n", parseOpenUs);
    PNG_cache_reset();
    t0 = Micros();
    for (int i = 0; i < M; i++) {
        Check(png.openRAM(b.data(), (int)b.size(), DrawCB) == PNG_SUCCESS,
              "micro cached open");
        png.close();
    }
    t1 = Micros();
    Stats sm = GetStats();
    double cachedOpenUs = (t1 - t0) / M;
    printf("  open() served from cache:      %.3f us/op (%.2fx faster, "
           "header parse removed on %d/%d ops)\n",
           cachedOpenUs, parseOpenUs / cachedOpenUs, sm.hits, sm.hits + sm.misses);

    // ---- S2: two files alternating ----
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    t0 = Micros();
    for (int i = 0; i < N; i++) {
        auto &cur = (i & 1) ? b : a;
        Check(FrameCB(png, cur.data(), (int)cur.size()) == PNG_SUCCESS, "S2 frame decodes");
    }
    t1 = Micros();
    Stats s2 = GetStats();
    Report("S2 alternating 2 files", N, t1 - t0, s2);
    Check(s2.parses == 2 && s2.hits == N - 2 && s2.entries == 2,
          "S2: 2 parses, both entries stay resident");

    // ---- S3: a cached field (PLTE) changed in place ----
    PNG_cache_configure(4, PNG_CACHE_LRU, 1);
    std::vector<uint8_t> work = b;
    std::vector<uint8_t> outV1, outV2;
    t0 = Micros();
    for (int i = 0; i < N; i++) {
        if (i == N / 2)
            work[plteOff] ^= 0x5a; // same buffer, new cached-field content
        Check(FrameCB(png, work.data(), (int)work.size()) == PNG_SUCCESS, "S3 frame decodes");
    }
    t1 = Micros();
    Stats s3 = GetStats();
    Report("S3 file changed in place", N, t1 - t0, s3);
    Check(s3.parses == 2, "S3: parsed once per content version, not per frame");
    Check(s3.hits == N - 2, "S3: unchanged frames keep hitting");
    Check(FrameBuf(png, a.data(), (int)a.size(), outV1) == PNG_SUCCESS, "RGB decode");
    Check(FrameBuf(png, b.data(), (int)b.size(), outV2) == PNG_SUCCESS, "palette decode");
    Check(outV1 != outV2, "different inputs decode to different pixels");
    // single-byte in-place PLTE edit invalidates the fingerprint
    PNG_cache_reset();
    Check(FrameCB(png, work.data(), (int)work.size()) == PNG_SUCCESS, "S3 pre-edit decode");
    Stats before = GetStats();
    work[plteOff] ^= 0x5a; // flip back to the original palette content
    Check(FrameCB(png, work.data(), (int)work.size()) == PNG_SUCCESS, "S3 post-edit decode");
    Stats after = GetStats();
    Check(after.misses == before.misses + 1 && after.parses == before.parses + 1,
          "S3: one cached-field byte change forces a miss and a re-parse");

    // Non-cached regions must NOT invalidate the entry:
    //  (a) an ancillary tEXt byte (not part of any cached field)
    auto txt = a;
    PNG_cache_reset();
    Check(FrameCB(png, txt.data(), (int)txt.size()) == PNG_SUCCESS, "tEXt pre-edit decode");
    Stats preTxt = GetStats();
    txt[textOff] ^= 0x5a;
    Check(FrameCB(png, txt.data(), (int)txt.size()) == PNG_SUCCESS, "tEXt post-edit decode");
    Stats postTxt = GetStats();
    Check(postTxt.misses == preTxt.misses && postTxt.parses == preTxt.parses,
          "ancillary tEXt edit keeps using the cached header (no re-parse)");
    //  (b) a post-IDAT byte (IEND CRC); the file still decodes with options=0
    int iendCrc = ChunkCrcOffset(work, 0x49454e44U /*'IEND'*/);
    Check(iendCrc > 0, "IEND CRC offset located (post-IDAT region)");
    Check(FrameCB(png, work.data(), (int)work.size()) == PNG_SUCCESS, "pre-post-IDAT-edit decode");
    Stats prePost = GetStats();
    work[iendCrc] ^= 0x5a;
    Check(FrameCB(png, work.data(), (int)work.size()) == PNG_SUCCESS, "post-IDAT-edit decode ok");
    Stats postPost = GetStats();
    Check(postPost.misses == prePost.misses && postPost.parses == prePost.parses,
          "post-IDAT edit keeps using the cached header (no re-parse)");
    work[iendCrc] ^= 0x5a; // restore

    // ---- S4: cache full / eviction ----
    // 8 distinct palette contents, capacity 2: continuous eviction, ~0% hits
    std::vector<std::vector<uint8_t> > many(8);
    for (int i = 0; i < 8; i++) {
        many[i] = b;
        many[i][plteOff] = (uint8_t)(0x10 + i);
        many[i][plteOff + 1] = (uint8_t)i;
    }
    PNG_cache_configure(2, PNG_CACHE_LRU, 1);
    t0 = Micros();
    for (int i = 0; i < 2000; i++) {
        auto &cur = many[i % 8];
        Check(FrameCB(png, cur.data(), (int)cur.size()) == PNG_SUCCESS, "S4 frame decodes");
    }
    t1 = Micros();
    Stats s4a = GetStats();
    Report("S4 cap=2, 8 files thrash", 2000, t1 - t0, s4a);
    Check(s4a.entries == 2 && s4a.evictions > 1900 && s4a.hits == 0,
          "S4: table stays at capacity, every frame evicts and misses");
    // same workload, capacity 8: everything fits after warmup
    PNG_cache_configure(8, PNG_CACHE_LRU, 1);
    t0 = Micros();
    for (int i = 0; i < 2000; i++) {
        auto &cur = many[i % 8];
        Check(FrameCB(png, cur.data(), (int)cur.size()) == PNG_SUCCESS, "S4-fit frame decodes");
    }
    t1 = Micros();
    Stats s4b = GetStats();
    Report("S4 cap=8, 8 files fit", 2000, t1 - t0, s4b);
    Check(s4b.parses == 8 && s4b.evictions == 0 && s4b.entries == 8,
          "S4: fitting capacity parses each file once, never evicts");
    // FIFO policy also honors the capacity
    PNG_cache_configure(2, PNG_CACHE_FIFO, 1);
    for (int i = 0; i < 64; i++)
        Check(FrameCB(png, many[i % 8].data(), (int)many[0].size()) == PNG_SUCCESS, "S4 FIFO frame");
    Stats s4c = GetStats();
    Check(s4c.entries == 2 && s4c.evictions > 0, "S4: FIFO evicts and keeps 2 entries");

    // ---- invalid configuration must be rejected ----
    Check(PNG_cache_configure(0, PNG_CACHE_LRU, 1) == PNG_INVALID_PARAMETER,
          "capacity 0 is rejected");
    Check(PNG_cache_configure(-1, PNG_CACHE_LRU, 1) == PNG_INVALID_PARAMETER,
          "negative capacity is rejected");
    Check(PNG_cache_configure(16, 99, 1) == PNG_INVALID_PARAMETER,
          "unknown eviction policy is rejected");
    Check(PNG_cache_configure(16, PNG_CACHE_LRU, 0) == PNG_INVALID_PARAMETER,
          "thread count 0 is rejected");
    Check(PNG_cache_configure(17, PNG_CACHE_LRU, 1) == PNG_INVALID_PARAMETER,
          "capacity above compile-time max is rejected");
    Check(PNG_cache_configure(4, PNG_CACHE_LRU, 4) == PNG_SUCCESS,
          "valid configuration accepted");
    (void)c; (void)d;

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("process peak RSS: %ld KB\n", ru.ru_maxrss);
    printf("ALL SINGLE-THREADED SCENARIOS PASSED\n");
}

struct ThreadArg { const std::vector<uint8_t> *a, *b; int frames; int id; };

static void *ThreadWorker(void *p)
{
    ThreadArg *ta = (ThreadArg *)p;
    PNG pngWorker; // one decoder instance per thread, one shared cache for all
    for (int i = 0; i < ta->frames; i++) {
        const std::vector<uint8_t> &cur = ((i + ta->id) & 1) ? *ta->b : *ta->a;
        if (FrameCB(pngWorker, const_cast<uint8_t *>(cur.data()), (int)cur.size()) != PNG_SUCCESS) {
            printf("thread %d decode failure at frame %d\n", ta->id, i);
            return (void *)1;
        }
    }
    return NULL;
}

static void RunThreads(const std::string &dir)
{
    auto a = LoadFile((dir + "/a_rgb.png").c_str());
    auto b = LoadFile((dir + "/b_pal8.png").c_str());
    for (int tcount : {2, 4, 8}) {
        PNG_cache_configure(4, PNG_CACHE_LRU, tcount);
        pthread_t th[8];
        ThreadArg args[8];
        int frames = 1000;
        double t0 = Micros();
        for (int i = 0; i < tcount; i++) {
            args[i] = {&a, &b, frames, i};
            pthread_create(&th[i], NULL, ThreadWorker, &args[i]);
        }
        int bad = 0;
        for (int i = 0; i < tcount; i++) {
            void *r = NULL;
            pthread_join(th[i], &r);
            if (r) bad++;
        }
        double t1 = Micros();
        Stats s = GetStats();
        int total = tcount * frames;
        Report("threads shared cache", total, t1 - t0, s);
        Check(bad == 0, "all worker frames decoded successfully");
        Check(s.entries == 2, "exactly the two shared files are cached");
        Check(s.parses >= 2 && s.parses <= 2 * tcount,
              "parse count bounded between 2 and 2x threads (race-safe publish)");
        Check(s.hits + s.misses == total, "every frame performed a cache lookup");
        Check(s.hits > 0, "shared read-only cache served hits across threads");
    }
    printf("ALL MULTI-THREADED SCENARIOS PASSED\n");
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "all";
    const char *dir = (argc > 2) ? argv[2] : "samples";
    if (!strcmp(mode, "threads"))
        RunThreads(dir);
    else if (!strcmp(mode, "perf"))
        RunPerf(dir);
    else if (!strcmp(mode, "real"))
        RunRealImage(dir); // here dir is the path to the real PNG file
    else
        RunAll(dir);
    return 0;
}

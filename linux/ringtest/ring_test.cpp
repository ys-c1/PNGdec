//
// ring_test.cpp - acceptance test for the ring-buffer line output mode
//
// For each of the 4 fixture types:
//   - full-image decode (baseline, setBuffer) vs ring-buffer decode
//   - both checked against an independent Python/stdlib reference (.raw)
//   - peak memory (analytic + ru_maxrss) and decode time
// Plus: ring size 1 filter failures, prefetch, lookback, interlaced.
//
#include "../../src/PNGdec.h"
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

static int g_iFailures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("    [PASS] %s\n", msg); } \
    else { printf("    [FAIL] %s\n", msg); g_iFailures++; } \
} while (0)

static double NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static uint8_t *LoadFile(const char *szPath, int *piSize) {
    FILE *f = fopen(szPath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = (uint8_t *)malloc(l ? l : 1);
    if (fread(p, 1, l, f) != (size_t)l) { fclose(f); free(p); return NULL; }
    fclose(f);
    *piSize = (int)l;
    return p;
}

// Callback that only counts the lines (used for fair timing: no extra work)
static int CountDraw(PNGDRAW *pDraw) {
    (*(int *)pDraw->pUser)++;
    return 1;
}

// Callback that only checksums the lines (no test-side image copy)
struct CrcCtx { unsigned long ulCrc; int iLines; };
static int CrcDraw(PNGDRAW *pDraw) {
    CrcCtx *c = (CrcCtx *)pDraw->pUser;
    c->ulCrc = crc32(c->ulCrc, pDraw->pPixels, pDraw->iPitch);
    c->iLines++;
    return 1;
}

// Callback that copies each line into a test-side image and exercises the
// lookback API: line y-1 must still be in the ring and match what we were
// given before; line y-N must have been recycled already.
struct CopyCtx {
    PNG *pPNG;
    uint8_t *pImage;
    int iPitch, iLines, iRingN;
    int iLookbackErr;
};
static int CopyDraw(PNGDRAW *pDraw) {
    CopyCtx *c = (CopyCtx *)pDraw->pUser;
    memcpy(&c->pImage[pDraw->y * c->iPitch], pDraw->pPixels, c->iPitch);
    c->iLines++;
    if (pDraw->y > 0) {
        uint8_t *pPrev = c->pPNG->getRingLine(pDraw->y - 1);
        if (!pPrev || memcmp(pPrev, &c->pImage[(pDraw->y - 1) * c->iPitch], c->iPitch) != 0)
            c->iLookbackErr++;
    }
    if (c->pPNG->getRingLine(pDraw->y - c->iRingN) != NULL)
        c->iLookbackErr++; // should have been recycled
    return 1;
}

struct Result {
    int rc, iLines, iLookbackErr;
    double dMs;
    unsigned long ulCrc;
};

// Decode with the full-image buffer (baseline), keeps the image in ppImage
static Result RunFull(PNG &png, uint8_t *pData, int iSize, uint8_t **ppImage, int iReps) {
    Result r = {};
    png.openRAM(pData, iSize, NULL);
    int iBufSize = png.getBufferSize();
    *ppImage = (uint8_t *)malloc(iBufSize);
    png.setBuffer(*ppImage);
    r.dMs = 1e30;
    for (int i = 0; i < iReps; i++) {
        double t0 = NowMs();
        r.rc = png.decode(NULL, 0);
        double t1 = NowMs();
        if (t1 - t0 < r.dMs) r.dMs = t1 - t0;
    }
    r.ulCrc = crc32(0, *ppImage, iBufSize);
    r.iLines = png.getHeight();
    png.close();
    return r;
}

// Decode with the ring buffer, count lines only (pure streaming, fair timing)
static Result RunRingCrc(PNG &png, uint8_t *pData, int iSize, int iN, int iP, int iReps) {
    Result r = {};
    int iCount;
    png.openRAM(pData, iSize, CountDraw);
    uint8_t *pRing = (uint8_t *)malloc(png.getRingBufferSize(iN));
    png.setRingBuffer(pRing, iN);
    png.setRingPrefetch(iP);
    r.dMs = 1e30;
    for (int i = 0; i < iReps; i++) {
        iCount = 0;
        double t0 = NowMs();
        r.rc = png.decode(&iCount, 0);
        double t1 = NowMs();
        if (t1 - t0 < r.dMs) r.dMs = t1 - t0;
    }
    r.iLines = iCount;
    free(pRing);
    png.close();
    return r;
}

// Measure the peak RSS of a single decode in a fresh child process so the
// two modes don't pollute each other's high-water mark.
// iRingMode: 0 = full-image, 1 = ring, 2 = baseline (open only, no decode)
static long ChildPeakRSSkb(uint8_t *pData, int iSize, int iRingMode, int iN, int iP) {
    pid_t pid = fork();
    if (pid == 0) { // child
        static PNG png;
        if (iRingMode == 1) {
            int iCount = 0;
            png.openRAM(pData, iSize, CountDraw);
            uint8_t *pRing = (uint8_t *)malloc(png.getRingBufferSize(iN));
            png.setRingBuffer(pRing, iN);
            png.setRingPrefetch(iP);
            png.decode(&iCount, 0);
            free(pRing);
        } else if (iRingMode == 0) {
            png.openRAM(pData, iSize, NULL);
            uint8_t *pBuf = (uint8_t *)malloc(png.getBufferSize());
            png.setBuffer(pBuf);
            png.decode(NULL, 0);
            free(pBuf);
        } else {
            png.openRAM(pData, iSize, NULL); // baseline: no decode buffers
        }
        png.close();
        _exit(0);
    }
    int iStatus;
    struct rusage ru;
    wait4(pid, &iStatus, 0, &ru);
    return ru.ru_maxrss;
}

// Decode with the ring buffer, copying lines out for pixel comparison
static Result RunRingCopy(PNG &png, uint8_t *pData, int iSize, int iN, int iP, uint8_t **ppImage) {
    Result r = {};
    CopyCtx ctx = {};
    png.openRAM(pData, iSize, CopyDraw);
    *ppImage = (uint8_t *)malloc(png.getBufferSize());
    ctx.pPNG = &png;
    ctx.pImage = *ppImage;
    ctx.iPitch = png.getBufferSize() / png.getHeight();
    ctx.iRingN = iN;
    uint8_t *pRing = (uint8_t *)malloc(png.getRingBufferSize(iN));
    png.setRingBuffer(pRing, iN);
    png.setRingPrefetch(iP);
    double t0 = NowMs();
    r.rc = png.decode(&ctx, 0);
    r.dMs = NowMs() - t0;
    r.iLines = ctx.iLines;
    r.iLookbackErr = ctx.iLookbackErr;
    free(pRing);
    png.close();
    return r;
}

static void TestFixture(const char *szDir, const char *szName) {
    char szPath[256];
    int iSize;
    snprintf(szPath, sizeof(szPath), "%s/%s.png", szDir, szName);
    uint8_t *pData = LoadFile(szPath, &iSize);
    if (!pData) { printf("  cannot load %s\n", szPath); g_iFailures++; return; }

    static PNG png;
    const int N = 4, P = 0, REPS = 3;

    // 1) peak RSS of each mode, measured in fresh child processes
    long lRssBase = ChildPeakRSSkb(pData, iSize, 2, N, P);
    long lRssRing = ChildPeakRSSkb(pData, iSize, 1, N, P);
    long lRssFull = ChildPeakRSSkb(pData, iSize, 0, N, P);

    // 2) ring decode, timed (count-only callback)
    Result rRing = RunRingCrc(png, pData, iSize, N, P, REPS);

    // 3) baseline full-image decode, timed
    uint8_t *pFull = NULL;
    Result rFull = RunFull(png, pData, iSize, &pFull, REPS);

    // 4) ring decode with line copy-out for pixel-exact comparison
    uint8_t *pCopy = NULL;
    Result rCopy = RunRingCopy(png, pData, iSize, N, P, &pCopy);

    // 5) ring decode with prefetch P=3, N=6
    uint8_t *pPrefetch = NULL;
    Result rPre = RunRingCopy(png, pData, iSize, 6, 3, &pPrefetch);

    // 5) independent reference (.raw produced by ref_decode.py)
    snprintf(szPath, sizeof(szPath), "%s/%s.raw", szDir, szName);
    int iRawSize = 0;
    uint8_t *pRaw = LoadFile(szPath, &iRawSize);

    int iW = 0, iH = 0, iBpp = 0, iType = 0, iFull = 0, iRing = 0;
    {
        static PNG pngInfo;
        pngInfo.openRAM(pData, iSize, NULL);
        iW = pngInfo.getWidth(); iH = pngInfo.getHeight();
        iBpp = pngInfo.getBpp(); iType = pngInfo.getPixelType();
        iFull = pngInfo.getBufferSize();
        iRing = pngInfo.getRingBufferSize(N);
        pngInfo.close();
    }
    long lCtx = (long)sizeof(PNG);
    double dBufPct = 100.0 * iRing / iFull;
    double dTotPct = 100.0 * (lCtx + iRing) / (lCtx + iFull);

    // legacy 2-line callback mode (no ring configured) must be unaffected
    {
        static PNG pngL;
        CrcCtx lc = {0, 0};
        pngL.openRAM(pData, iSize, CrcDraw);
        int rcL = pngL.decode(&lc, 0);
        CHECK(rcL == PNG_SUCCESS && lc.iLines == iH && lc.ulCrc == rFull.ulCrc,
              "legacy 2-line callback mode unchanged");
        pngL.close();
    }

    printf("  %s (%dx%d, %d bpp, type %d)\n", szName, iW, iH, iBpp, iType);
    CHECK(rFull.rc == PNG_SUCCESS, "baseline decode ok");
    CHECK(rRing.rc == PNG_SUCCESS && rCopy.rc == PNG_SUCCESS && rPre.rc == PNG_SUCCESS,
          "ring decode ok (N=4/P=0 and N=6/P=3)");
    CHECK(rRing.iLines == iH && rCopy.iLines == iH && rPre.iLines == iH,
          "all lines delivered");
    CHECK(crc32(0, pCopy, iFull) == rFull.ulCrc, "crc32(ring) == crc32(full-image)");
    CHECK(memcmp(pCopy, pFull, iFull) == 0, "ring pixels identical to full-image (memcmp)");
    CHECK(memcmp(pPrefetch, pFull, iFull) == 0, "prefetch P=3 pixels identical to full-image");
    CHECK(rCopy.iLookbackErr == 0 && rPre.iLookbackErr == 0,
          "lookback API: y-1 available & correct, y-N recycled");
    if (pRaw) {
        CHECK(iRawSize == iFull && memcmp(pRaw, pFull, iFull) == 0,
              "baseline pixels identical to independent reference (.raw)");
        free(pRaw);
    } else {
        printf("    [SKIP] no .raw reference found\n");
    }
    printf("    time: full %.2f ms, ring %.2f ms (best of %d)\n", rFull.dMs, rRing.dMs, REPS);
    printf("    memory: full buf %d B, ring buf %d B (%.2f%%), decoder ctx %ld B both\n",
           iFull, iRing, dBufPct, lCtx);
    printf("    memory: total %ld B (full) vs %ld B (ring) = %.2f%%  [must be < 10%%]\n",
           lCtx + iFull, lCtx + iRing, dTotPct);
    printf("    process peak RSS (fresh child): base %ld KB, ring %+ld KB, full %+ld KB over base\n",
           lRssBase, lRssRing - lRssBase, lRssFull - lRssBase);
    CHECK(dBufPct < 10.0 && dTotPct < 10.0, "peak memory < 10% of full-image scheme");

    free(pCopy);
    free(pPrefetch);
    free(pFull);
    free(pData);
}

static void TestRingSize1(const char *szDir) {
    char szPath[256];
    int iSize;
    static PNG png;

    printf("  ring size = 1:\n");
    // a) file using Up/Avg/Paeth filters must fail with PNG_RING_TOO_SMALL
    snprintf(szPath, sizeof(szPath), "%s/gray8_1024x768.png", szDir);
    uint8_t *pData = LoadFile(szPath, &iSize);
    CrcCtx ctx;
    png.openRAM(pData, iSize, CrcDraw);
    uint8_t *pRing = (uint8_t *)malloc(png.getRingBufferSize(1));
    png.setRingBuffer(pRing, 1);
    ctx.ulCrc = 0; ctx.iLines = 0;
    int rc = png.decode(&ctx, 0);
    printf("    gray8 (uses Up/Avg/Paeth): decode rc = %d (PNG_RING_TOO_SMALL = %d)\n",
           rc, PNG_RING_TOO_SMALL);
    CHECK(rc == PNG_RING_TOO_SMALL && png.getLastError() == PNG_RING_TOO_SMALL,
          "ring size 1 + Up/Avg/Paeth filter -> PNG_RING_TOO_SMALL");
    png.close();
    free(pRing);
    free(pData);

    // b) file using only None/Sub filters must decode fine with ring size 1
    snprintf(szPath, sizeof(szPath), "%s/subonly_128x128.png", szDir);
    pData = LoadFile(szPath, &iSize);
    int iBufSize;
    {
        static PNG pngInfo;
        pngInfo.openRAM(pData, iSize, NULL);
        iBufSize = pngInfo.getBufferSize();
        pngInfo.close();
    }
    uint8_t *pFull = NULL;
    Result rFull = RunFull(png, pData, iSize, &pFull, 1);
    uint8_t *pCopy = NULL;
    Result rCopy = RunRingCopy(png, pData, iSize, 1, 0, &pCopy);
    CHECK(rFull.rc == PNG_SUCCESS && rCopy.rc == PNG_SUCCESS,
          "ring size 1 + None/Sub-only filters -> decode ok");
    CHECK(rCopy.iLines == 128 && memcmp(pCopy, pFull, iBufSize) == 0,
          "ring size 1 pixels identical to full-image");
    free(pCopy);
    free(pFull);
    free(pData);

    // c) invalid config: N < P+1 must be rejected
    snprintf(szPath, sizeof(szPath), "%s/subonly_128x128.png", szDir);
    pData = LoadFile(szPath, &iSize);
    png.openRAM(pData, iSize, CrcDraw);
    pRing = (uint8_t *)malloc(png.getRingBufferSize(2));
    png.setRingBuffer(pRing, 2);
    png.setRingPrefetch(2); // N=2 < P+1=3
    ctx.ulCrc = 0; ctx.iLines = 0;
    rc = png.decode(&ctx, 0);
    CHECK(rc == PNG_INVALID_PARAMETER, "N < P+1 rejected with PNG_INVALID_PARAMETER");
    png.close();
    free(pRing);
    free(pData);
}

static void TestInterlaced(const char *szDir) {
    char szPath[256];
    int iSize;
    static PNG png;

    printf("  interlaced (Adam7):\n");
    snprintf(szPath, sizeof(szPath), "%s/interlaced_128x128.png", szDir);
    uint8_t *pData = LoadFile(szPath, &iSize);
    if (!pData) { printf("    [FAIL] cannot load interlaced fixture\n"); g_iFailures++; return; }
    int rcOpen = png.openRAM(pData, iSize, NULL);
    printf("    full-image mode: open rc = %d (PNG_UNSUPPORTED_FEATURE = %d)\n",
           rcOpen, PNG_UNSUPPORTED_FEATURE);
    CHECK(rcOpen == PNG_UNSUPPORTED_FEATURE && png.isInterlaced() == 1,
          "interlaced rejected at open (full-image mode)");
    png.close();

    int rcOpen2 = png.openRAM(pData, iSize, CrcDraw);
    printf("    ring mode:       open rc = %d (PNG_UNSUPPORTED_FEATURE = %d)\n",
           rcOpen2, PNG_UNSUPPORTED_FEATURE);
    CHECK(rcOpen2 == PNG_UNSUPPORTED_FEATURE,
          "interlaced rejected at open (ring mode)");
    png.close();
    free(pData);
}

int main(int argc, char **argv) {
    const char *szDir = (argc > 1) ? argv[1] : "fixtures";
    printf("== ring-buffer line output mode: acceptance test ==\n");
    printf("decoder context sizeof(PNG) = %zu bytes (shared by both modes)\n", sizeof(PNG));

    const char *names[] = {
        "gray8_1024x768", "rgb8_1024x768", "pal8_1024x768", "rgba8_1024x768"
    };
    for (int i = 0; i < 4; i++)
        TestFixture(szDir, names[i]);

    TestRingSize1(szDir);
    TestInterlaced(szDir);

    printf("== %s (%d failure%s) ==\n", g_iFailures ? "FAILURES" : "ALL PASS",
           g_iFailures, g_iFailures == 1 ? "" : "s");
    return g_iFailures ? 1 : 0;
}

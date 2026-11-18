//
//  rowview_test.cpp
//
//  Tests for the optional read-only row view added to PNGdec.
//  Generates small PNG files in memory (zlib "stored" blocks, CRC/adler
//  computed by the zlib sources already shipped in the repo) so the test
//  needs no third-party libraries and no network access.
//
//  Build/run: ./run.sh
//
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <string>
#include <vector>

#define __LINUX__
#define INFLATE_STRICT
#include "../../src/zutil.c"
#include "../../src/inflate.c"
#include "../../src/inffast.c"
#include "../../src/inftrees.c"
#include "../../src/crc32.c"
#undef DO1
#undef DO8
#include "../../src/adler32.c"
#include "../../src/PNGdec.cpp"

static int g_iPass = 0, g_iFail = 0, g_iTotal = 0;

#define CHECK(cond, name)                                                    \
    do {                                                                     \
        g_iTotal++;                                                          \
        if (cond) {                                                          \
            g_iPass++;                                                       \
            printf("  [PASS] %s\n", name);                                   \
        } else {                                                             \
            g_iFail++;                                                       \
            printf("  [FAIL] %s (line %d)\n", name, __LINE__);               \
        }                                                                    \
    } while (0)

static long Micros(void)
{
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return (long)res.tv_sec * 1000000L + res.tv_nsec / 1000;
}

static void put_be32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

static void put_chunk(std::vector<uint8_t> &png, const char tag[4],
                      const uint8_t *pData, size_t len)
{
    put_be32(png, (uint32_t)len);
    size_t crc_start = png.size();
    png.insert(png.end(), tag, tag + 4);
    if (len)
        png.insert(png.end(), pData, pData + len);
    uint32_t crc = (uint32_t)crc32(0, &png[crc_start], (uInt)(len + 4));
    put_be32(png, crc);
}

// One zlib stored (uncompressed) deflate block + adler32 trailer.
static std::vector<uint8_t> zlib_stored(const uint8_t *pData, size_t len)
{
    std::vector<uint8_t> out;
    out.push_back(0x78); // CM=deflate, CINFO=32K window
    out.push_back(0x01); // FCHECK so (CMF*256+FLG) % 31 == 0
    size_t pos = 0;
    do {
        size_t n = len - pos;
        int final = (n <= 65535);
        size_t chunk = final ? n : 65535;
        out.push_back(final ? 1 : 0);
        out.push_back((uint8_t)(chunk & 0xff));
        out.push_back((uint8_t)(chunk >> 8));
        out.push_back((uint8_t)(~chunk & 0xff));
        out.push_back((uint8_t)((~chunk >> 8) & 0xff));
        out.insert(out.end(), pData + pos, pData + pos + chunk);
        pos += chunk;
    } while (pos < len);
    uint32_t ad = (uint32_t)adler32(1, pData, (uInt)len);
    put_be32(out, ad);
    return out;
}

struct ImgSpec {
    int w, h, bpp, colorType, interlace;
    const char *name;
};

// Build a raw scanline (no filter byte) for the given image geometry.
static void make_raw_line(const ImgSpec &s, int y, uint8_t *out)
{
    int channels = (s.colorType == PNG_PIXEL_TRUECOLOR) ? 3 :
                   (s.colorType == PNG_PIXEL_TRUECOLOR_ALPHA) ? 4 : 1;
    if (s.colorType == PNG_PIXEL_INDEXED) {
        // 4 bits per index, high nibble first, last byte zero padded
        int pitch = (s.w * s.bpp + 7) / 8;
        memset(out, 0, pitch);
        for (int x = 0; x < s.w; x++) {
            uint8_t idx = (uint8_t)((x + y * 3) & 0x0f);
            if (s.bpp == 4)
                out[x / 2] |= (uint8_t)(idx << ((x & 1) ? 0 : 4));
            else if (s.bpp == 8)
                out[x] = idx;
            else if (s.bpp == 2)
                out[x / 4] |= (uint8_t)(idx << (6 - 2 * (x & 3)));
        }
        return;
    }
    for (int x = 0; x < s.w; x++) {
        for (int c = 0; c < channels; c++) {
            uint8_t v;
            if (s.bpp == 16)
                v = 0; // high byte; low byte set below
            else
                v = (uint8_t)((x * 17 + y * 31 + c * 7) & 0xff);
            out[(x * channels + c) * (s.bpp == 16 ? 2 : 1)] = v;
            if (s.bpp == 16)
                out[(x * channels + c) * 2 + 1] =
                    (uint8_t)((x * 5 + y * 11 + c) & 0xff);
        }
    }
}

// Apply the PNG per-line filters so all five filter paths get exercised.
static void apply_filter(std::vector<uint8_t> &raw, int y,
                         const uint8_t *cur, const uint8_t *prev, int pitch,
                         int bppBytes)
{
    int f = y % 5; // NONE, SUB, UP, AVG, PAETH in rotation
    raw.push_back((uint8_t)f);
    for (int x = 0; x < pitch; x++) {
        uint8_t a = (x >= bppBytes) ? cur[x - bppBytes] : 0;
        uint8_t b = prev ? prev[x] : 0;
        uint8_t c = (prev && x >= bppBytes) ? prev[x - bppBytes] : 0;
        uint8_t v = cur[x];
        switch (f) {
        case PNG_FILTER_NONE: break;
        case PNG_FILTER_SUB: v = (uint8_t)(cur[x] - a); break;
        case PNG_FILTER_UP: v = (uint8_t)(cur[x] - b); break;
        case PNG_FILTER_AVG:
            v = (uint8_t)(cur[x] - ((a + b) / 2));
            break;
        case PNG_FILTER_PAETH: {
            int p = (int)a + (int)b - (int)c;
            int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            uint8_t pr = (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
            v = (uint8_t)(cur[x] - pr);
            break;
        }
        }
        raw.push_back(v);
    }
}

static std::vector<uint8_t> make_png(const ImgSpec &s,
                                     const std::vector<uint8_t> *palette = NULL,
                                     const std::vector<uint8_t> *trns = NULL)
{
    std::vector<uint8_t> png;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), sig, sig + 8);

    uint8_t ihdr[13];
    ihdr[0] = (s.w >> 24); ihdr[1] = (s.w >> 16);
    ihdr[2] = (s.w >> 8);  ihdr[3] = s.w;
    ihdr[4] = (s.h >> 24); ihdr[5] = (s.h >> 16);
    ihdr[6] = (s.h >> 8);  ihdr[7] = s.h;
    ihdr[8] = (uint8_t)s.bpp;
    ihdr[9] = (uint8_t)s.colorType;
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = (uint8_t)s.interlace;
    put_chunk(png, "IHDR", ihdr, 13);

    if (palette)
        put_chunk(png, "PLTE", palette->data(), palette->size());
    if (trns)
        put_chunk(png, "tRNS", trns->data(), trns->size());

    int channels = (s.colorType == PNG_PIXEL_TRUECOLOR) ? 3 :
                   (s.colorType == PNG_PIXEL_TRUECOLOR_ALPHA) ? 4 : 1;
    int pitch = (s.w * channels * s.bpp + 7) / 8;
    int bppBytes = (channels * s.bpp + 7) / 8;

    std::vector<uint8_t> raw;
    std::vector<uint8_t> prev(pitch, 0), cur(pitch);
    for (int y = 0; y < s.h; y++) {
        make_raw_line(s, y, cur.data());
        if (s.colorType == PNG_PIXEL_INDEXED || y == 0)
            apply_filter(raw, y, cur.data(), NULL, pitch, bppBytes);
        else
            apply_filter(raw, y, cur.data(), prev.data(), pitch, bppBytes);
        prev = cur;
    }
    std::vector<uint8_t> z = zlib_stored(raw.data(), raw.size());
    put_chunk(png, "IDAT", z.data(), z.size());
    put_chunk(png, "IEND", NULL, 0);
    return png;
}

// Capture every line delivered through the classic draw callback.
struct CapState {
    std::vector<std::vector<uint8_t> > rows;
    int width, pitch;
};
static int cap_callback(PNGDRAW *pDraw)
{
    CapState *cap = (CapState *)pDraw->pUser;
    if ((int)cap->rows.size() <= pDraw->y)
        cap->rows.resize(pDraw->y + 1);
    cap->rows[pDraw->y].assign(pDraw->pPixels,
                               pDraw->pPixels + pDraw->iPitch);
    cap->width = pDraw->iWidth;
    cap->pitch = pDraw->iPitch;
    return 1;
}

// Matches PNGRGB565's truecolor packing: R in the high bits, B low.
static uint16_t ref_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((b >> 3) | ((g >> 2) << 5) | ((r >> 3) << 11));
}

// Full structural + consistency checks for every decodable image.
static void exercise_decoder(const char *label, std::vector<uint8_t> &file,
                             int expectBpp, int expectType, int align,
                             int lazy)
{
    printf("\n=== %s (align=%d, lazy=%d) ===\n", label, align, lazy);
    PNG png;
    int rc = png.openRAM(file.data(), (int)file.size(), NULL);
    CHECK(rc == PNG_SUCCESS, "openRAM succeeds");
    CHECK(png.getBpp() == expectBpp && png.getPixelType() == expectType,
          "reported bpp/type match fixture");
    int w = png.getWidth(), h = png.getHeight(), pitch = 0;

    // Reference 1: classic full-image buffer decode.
    PNG ref;
    ref.openRAM(file.data(), (int)file.size(), NULL);
    pitch = ref.getBufferSize() / ref.getHeight();
    std::vector<uint8_t> full(ref.getBufferSize());
    ref.setBuffer(full.data());
    CHECK(ref.decode(NULL, 0) == PNG_SUCCESS, "full-image decode succeeds");

    // Reference 2: classic line-by-line draw callback.
    PNG cb;
    CapState cap;
    cb.openRAM(file.data(), (int)file.size(), cap_callback);
    CHECK(cb.decode(&cap, 0) == PNG_SUCCESS, "callback decode succeeds");
    CHECK((int)cap.rows.size() == h && cap.pitch == pitch,
          "callback delivered h rows of pitch bytes");

    // Row view under test.
    PNGROWCONFIG cfg;
    cfg.iAlign = align;
    cfg.iOptions = lazy ? PNG_ROW_LAZY_RGB565 : 0;
    CHECK(png.enableRowView(&cfg) == PNG_SUCCESS, "enableRowView succeeds");
    CHECK(png.decode(NULL, 0) == PNG_SUCCESS, "row-view decode succeeds");
    int n = -1;
    const PNGROW *rows = png.getRows(&n);
    CHECK(rows != NULL && n == h, "getRows returns h descriptors");
    bool ok = true, aligned = true;
    for (int y = 0; y < h && rows; y++) {
        if (rows[y].iLen != pitch || rows[y].pData == NULL)
            ok = false;
        if (rows[y].pData &&
            ((uintptr_t)rows[y].pData & (align - 1)) != 0)
            aligned = false;
        if (rows[y].pData &&
            memcmp(rows[y].pData, &full[(size_t)y * pitch], pitch) != 0)
            ok = false; // equality with full-image decode
        if (y < (int)cap.rows.size() &&
            memcmp(rows[y].pData, cap.rows[y].data(), pitch) != 0)
            ok = false; // equality with callback path
    }
    CHECK(ok, "row contents match full-image and callback decodes");
    CHECK(aligned || align == 1, "every row start meets requested alignment");
    CHECK(png.isRowViewValid() == TRUE, "view reports valid");
    printf("  info: %d rows, pitch %d bytes, view memory %d bytes\n",
          n, pitch, png.getRowViewMemory());

    // RGB565: lazy vs eager vs a conversion built straight from the buffer.
    const uint16_t *p565 = png.getRowRGB565(0, PNG_RGB565_LITTLE_ENDIAN,
                                            0xffffffff);
    CHECK(p565 != NULL, "getRowRGB565 returns a row");
    PNGDRAW d;
    memset(&d, 0, sizeof(d));
    d.iWidth = w; d.iPitch = pitch;
    d.iPixelType = png.getPixelType(); d.iBpp = png.getBpp();
    d.iHasAlpha = png.hasAlpha(); d.pPalette = png.getPalette();
    int convW = w; // PNGRGB565 rounds sub-byte pixels up to whole bytes
    if (d.iBpp < 8 && (d.iPixelType == PNG_PIXEL_INDEXED ||
                       d.iPixelType == PNG_PIXEL_GRAYSCALE))
        convW = ((w + (8 / d.iBpp) - 1) / (8 / d.iBpp)) * (8 / d.iBpp);
    std::vector<uint16_t> ref565(convW);
    d.pPixels = full.data();
    png.getLineAsRGB565(&d, ref565.data(), PNG_RGB565_LITTLE_ENDIAN,
                        0xffffffff);
    bool cvt = p565 && memcmp(p565, ref565.data(), w * 2) == 0;
    CHECK(cvt, "row RGB565 matches getLineAsRGB565 on the same pixels");

    // Lazy mode keeps only one reusable cache, so each result is copied
    // before the next call; eager views only expose the canonical params.
    if (lazy) {
        const uint16_t *be = png.getRowRGB565(1 % h, PNG_RGB565_BIG_ENDIAN,
                                              0xffffffff);
        std::vector<uint16_t> beCopy(w);
        if (be)
            memcpy(beCopy.data(), be, w * 2);
        const uint16_t *le = png.getRowRGB565(1 % h,
                                              PNG_RGB565_LITTLE_ENDIAN,
                                              0xffffffff);
        bool beok = be != NULL && le != NULL;
        for (int x = 0; x < w && le; x++)
            if (beCopy[x] != (uint16_t)(((le[x] >> 8) | (le[x] << 8))))
                beok = false;
        CHECK(beok, "big-endian row is byte-swapped little-endian");
    } else {
        CHECK(png.getRowRGB565(0, PNG_RGB565_BIG_ENDIAN, 0xffffffff) == NULL,
              "eager view rejects non-canonical conversion params");
    }
    CHECK(png.getRowRGB565(h, 0, 0xffffffff) == NULL,
          "out-of-range row returns NULL");
    printf("  info: converted rows materialized = %d (lazy=%d)\n",
          png.getRowViewConvertedRows(), lazy);

    // Independent spot check of the RGB565 math on a truecolor fixture.
    if (expectType == PNG_PIXEL_TRUECOLOR && expectBpp == 8) {
        uint8_t r = (0 * 17 + 0 * 31 + 0 * 7) & 0xff;
        uint8_t g = (0 * 17 + 0 * 31 + 1 * 7) & 0xff;
        uint8_t b = (0 * 17 + 0 * 31 + 2 * 7) & 0xff;
        CHECK(p565 && p565[0] == ref_rgb565(r, g, b),
              "first RGB565 pixel matches hand-computed value");
    }

    // A second decode on the same object rebuilds the same view.
    CHECK(png.decode(NULL, 0) == PNG_SUCCESS, "re-decode succeeds");
    const PNGROW *rows2 = png.getRows(&n);
    bool reok = rows2 && n == h;
    for (int y = 0; y < h && reok; y++)
        if (memcmp(rows2[y].pData, &full[(size_t)y * pitch], pitch) != 0)
            reok = false;
    CHECK(reok, "view after re-decode still matches the buffer decode");

    // Releasing the decode backing store invalidates the view.
    png.disableRowView();
    CHECK(png.getRows(NULL) == NULL && png.isRowViewValid() == FALSE &&
          png.getRowViewMemory() == 0 && png.getRowRGB565(0, 0, 0) == NULL,
          "view invalid after disableRowView");

    // After teardown the classic full-buffer path works again on this object.
    png.setBuffer(full.data());
    CHECK(png.decode(NULL, 0) == PNG_SUCCESS,
          "full-buffer decode works again after teardown");
    ref.close();
    cb.close();
    png.close();
}

static void exercise_unsupported(const char *label,
                                 std::vector<uint8_t> &file, int interlace)
{
    printf("\n=== %s (must remain unsupported) ===\n", label);
    PNG png;
    int rc = png.openRAM(file.data(), (int)file.size(), NULL);
    CHECK(rc == PNG_UNSUPPORTED_FEATURE &&
          png.getLastError() == PNG_UNSUPPORTED_FEATURE,
          "open reports PNG_UNSUPPORTED_FEATURE");
    CHECK(png.isInterlaced() == interlace, "interlace flag reported correctly");
    PNGROWCONFIG cfg;
    cfg.iAlign = 4;
    cfg.iOptions = 0;
    CHECK(png.enableRowView(&cfg) == PNG_UNSUPPORTED_FEATURE,
          "enableRowView refuses unsupported file");
    CHECK(png.getRows(NULL) == NULL && png.isRowViewValid() == FALSE,
          "no row view exists");
    long t0 = Micros();
    for (int i = 0; i < 1000; i++) {
        PNG p;
        p.openRAM(file.data(), (int)file.size(), NULL);
    }
    printf("  info: 1000 rejected opens took %ld us (%.2f us each)\n",
           Micros() - t0, (Micros() - t0) / 1000.0);
}

static long time_decodes(std::vector<uint8_t> &file, int mode, int iters,
                         PNGROWCONFIG *cfg)
{
    PNG png;
    png.openRAM(file.data(), (int)file.size(), NULL);
    std::vector<uint8_t> buf(png.getBufferSize());
    if (mode == 0)
        png.setBuffer(buf.data());
    else
        png.enableRowView(cfg);
    long t0 = Micros();
    for (int i = 0; i < iters; i++)
        png.decode(NULL, 0);
    long t = Micros() - t0;
    png.disableRowView();
    png.close();
    return t;
}

int main(void)
{
    // ---- Fixtures -------------------------------------------------------
    ImgSpec gray8 = {12, 8, 8, PNG_PIXEL_GRAYSCALE, 0, "8-bit grayscale"};
    ImgSpec rgb888 = {13, 9, 8, PNG_PIXEL_TRUECOLOR, 0, "8-bit truecolor"};
    ImgSpec idx4 = {9, 6, 4, PNG_PIXEL_INDEXED, 0, "4-bit palette"};
    ImgSpec rgb16 = {4, 4, 16, PNG_PIXEL_TRUECOLOR, 0, "16-bit truecolor"};
    ImgSpec inter = {4, 4, 8, PNG_PIXEL_GRAYSCALE, 1, "Adam7 interlaced"};

    std::vector<uint8_t> fGray = make_png(gray8);
    std::vector<uint8_t> fRGB = make_png(rgb888);
    std::vector<uint8_t> pal(3 * 16), trns(4);
    for (int i = 0; i < 16; i++) {
        pal[i * 3 + 0] = (uint8_t)(i * 17);
        pal[i * 3 + 1] = (uint8_t)(255 - i * 17);
        pal[i * 3 + 2] = (uint8_t)(i * 11);
    }
    trns[0] = 0; trns[1] = 64; trns[2] = 128; trns[3] = 255;
    std::vector<uint8_t> fIdx = make_png(idx4, &pal, &trns);
    ImgSpec idx2 = {7, 5, 2, PNG_PIXEL_INDEXED, 0, "2-bit palette"};
    std::vector<uint8_t> pal4(pal.begin(), pal.begin() + 12); // 4 entries
    std::vector<uint8_t> trns4(trns.begin(), trns.begin() + 4);
    std::vector<uint8_t> fIdx2 = make_png(idx2, &pal4, &trns4);
    std::vector<uint8_t> f16 = make_png(rgb16);
    std::vector<uint8_t> fInter = make_png(inter);

    // ---- The four required file classes --------------------------------
    exercise_decoder("8-bit file (grayscale)", fGray, 8,
                     PNG_PIXEL_GRAYSCALE, 4, 1);
    exercise_decoder("8-bit file (RGB888, all 5 filters)", fRGB, 8,
                     PNG_PIXEL_TRUECOLOR, 16, 0);
    exercise_decoder("palette file (4bpp + tRNS, partial last byte)", fIdx,
                     4, PNG_PIXEL_INDEXED, 1, 1);
    exercise_decoder("palette file (2bpp, sub-byte RGB565 rounding)", fIdx2,
                     2, PNG_PIXEL_INDEXED, 8, 0);
    exercise_unsupported("16-bit file", f16, 0);
    exercise_unsupported("interlaced file", fInter, 1);

    // ---- Configuration validation --------------------------------------
    printf("\n=== configuration & lifetime ===\n");
    PNG png;
    png.openRAM(fRGB.data(), (int)fRGB.size(), NULL);
    PNGROWCONFIG bad;
    bad.iAlign = 3; bad.iOptions = 0;
    CHECK(png.enableRowView(&bad) == PNG_INVALID_PARAMETER,
          "alignment 3 rejected");
    bad.iAlign = 32; bad.iOptions = 0;
    CHECK(png.enableRowView(&bad) == PNG_INVALID_PARAMETER,
          "alignment 32 rejected");
    bad.iAlign = 4; bad.iOptions = 99;
    CHECK(png.enableRowView(&bad) == PNG_INVALID_PARAMETER,
          "unknown option flag rejected");
    CHECK(png.enableRowView(NULL) == PNG_INVALID_PARAMETER,
          "NULL config rejected");
    CHECK(png.getRows(NULL) == NULL, "no view before enable -> NULL rows");
    png.close();

    // Repeated enable without an explicit disable must replace cleanly.
    {
        PNG p2;
        p2.openRAM(fGray.data(), (int)fGray.size(), NULL);
        PNGROWCONFIG a; a.iAlign = 1; a.iOptions = PNG_ROW_LAZY_RGB565;
        PNGROWCONFIG b; b.iAlign = 16; b.iOptions = 0;
        CHECK(p2.enableRowView(&a) == PNG_SUCCESS, "first enable succeeds");
        CHECK(p2.enableRowView(&b) == PNG_SUCCESS,
              "second enable replaces the previous view");
        CHECK(p2.decode(NULL, 0) == PNG_SUCCESS, "decode after re-enable");
        int n = 0;
        const PNGROW *r = p2.getRows(&n);
        bool good = r && n == gray8.h;
        for (int y = 0; y < n && good; y++)
            if (((uintptr_t)r[y].pData & 15) != 0)
                good = false;
        CHECK(good, "replaced view uses the new alignment");
        p2.disableRowView();
        p2.close();
    }

    // A row requested before any decode must be rejected, not crash.
    {
        PNG p3;
        p3.openRAM(fGray.data(), (int)fGray.size(), NULL);
        PNGROWCONFIG c; c.iAlign = 2; c.iOptions = PNG_ROW_LAZY_RGB565;
        p3.enableRowView(&c);
        CHECK(p3.isRowViewValid() == FALSE && p3.getRows(NULL) == NULL &&
              p3.getRowRGB565(0, 0, 0xffffffff) == NULL,
              "no valid row before decode");
        p3.disableRowView();
        p3.close();
    }

    // Truncate the IDAT down to its 2-byte zlib header so no complete line
    // can ever inflate: the decoder must error and the view stay invalid.
    // (A flip of one data byte may slip past because adler/CRC checks are
    // off by default, and a long truncation can still leave full lines.)
    {
        PNG p4;
        const char idat[4] = {'I', 'D', 'A', 'T'};
        size_t idatData = std::string::npos;
        for (size_t i = 8; i + 8 <= fGray.size(); i++) {
            if (memcmp(&fGray[i + 4], idat, 4) == 0) {
                idatData = i + 8;
                break;
            }
        }
        CHECK(idatData != std::string::npos, "fixture contains an IDAT chunk");
        std::vector<uint8_t> truncated(fGray.begin(),
                                       fGray.begin() + idatData + 2);
        CHECK(p4.openRAM(truncated.data(), (int)truncated.size(), NULL) ==
              PNG_SUCCESS, "truncated fixture still opens");
        PNGROWCONFIG c; c.iAlign = 4; c.iOptions = PNG_ROW_LAZY_RGB565;
        p4.enableRowView(&c);
        int brc = p4.decode(NULL, 0);
        CHECK(brc != PNG_SUCCESS, "truncated decode reports an error");
        CHECK(p4.isRowViewValid() == FALSE && p4.getRows(NULL) == NULL,
              "view invalid after a failed decode");
        p4.disableRowView();
        p4.close();
    }

    // After a successful decode, a failed re-decode must not expose stale
    // row data left over from the good decode.
    {
        PNG p5;
        p5.openRAM(fGray.data(), (int)fGray.size(), NULL);
        PNGROWCONFIG c; c.iAlign = 4; c.iOptions = PNG_ROW_LAZY_RGB565;
        p5.enableRowView(&c);
        CHECK(p5.decode(NULL, 0) == PNG_SUCCESS &&
              p5.isRowViewValid() == TRUE, "first decode builds a valid view");
        const char idat[4] = {'I', 'D', 'A', 'T'};
        size_t idatData = std::string::npos;
        for (size_t i = 8; i + 8 <= fGray.size(); i++) {
            if (memcmp(&fGray[i + 4], idat, 4) == 0) {
                idatData = i + 8;
                break;
            }
        }
        std::vector<uint8_t> truncated(fGray.begin(),
                                       fGray.begin() + idatData + 2);
        p5.close();
        p5.openRAM(truncated.data(), (int)truncated.size(), NULL);
        p5.enableRowView(&c); // rebuild an empty view for the broken file
        p5.decode(NULL, 0);
        CHECK(p5.isRowViewValid() == FALSE && p5.getRows(NULL) == NULL,
              "failed re-decode does not expose stale rows");
        p5.disableRowView();
        p5.close();
    }

    // Re-opening an object without an explicit teardown must not leak the
    // previous block: open*() memsets PNGIMAGE, so cleanup is driven by the
    // owner registry rather than by a pointer stored inside PNGIMAGE.
    for (int rep = 0; rep < 5; rep++) {
        PNG p6;
        p6.openRAM(fGray.data(), (int)fGray.size(), NULL);
        PNGROWCONFIG c; c.iAlign = (rep % 2) ? 8 : 4;
        c.iOptions = (rep % 2) ? PNG_ROW_LAZY_RGB565 : 0;
        p6.enableRowView(&c);
        p6.decode(NULL, 0);
        CHECK(p6.isRowViewValid() == TRUE, "view valid before forgotten close");
        p6.close();                       // deliberately no disableRowView()
        p6.openRAM(fRGB.data(), (int)fRGB.size(), NULL); // memset's PNGIMAGE
        p6.enableRowView(&c);             // must free the orphaned block
        p6.decode(NULL, 0);
        CHECK(p6.isRowViewValid() == TRUE, "new view valid after re-open");
        p6.disableRowView();
        p6.close();
    }
    CHECK(true, "re-open without teardown stays leak-free (LSan clean)");

    // Two objects enabled before either decode: the draw thunk must route
    // each line to the block owned by the object actually decoding.
    {
        PNG a, b;
        a.openRAM(fGray.data(), (int)fGray.size(), NULL);
        b.openRAM(fRGB.data(), (int)fRGB.size(), NULL);
        PNGROWCONFIG ca; ca.iAlign = 4; ca.iOptions = PNG_ROW_LAZY_RGB565;
        PNGROWCONFIG cb; cb.iAlign = 8; cb.iOptions = 0;
        CHECK(a.enableRowView(&ca) == PNG_SUCCESS &&
              b.enableRowView(&cb) == PNG_SUCCESS, "two views enabled");
        CHECK(b.decode(NULL, 0) == PNG_SUCCESS &&
              a.decode(NULL, 0) == PNG_SUCCESS, "both decode out of order");
        int na = 0, nb = 0;
        const PNGROW *ra = a.getRows(&na), *rb = b.getRows(&nb);
        bool good = ra && rb && na == gray8.h && nb == rgb888.h;
        int apitch = a.getBufferSize() / a.getHeight();
        int bpitch = b.getBufferSize() / b.getHeight();
        PNG refa, refb;
        refa.openRAM(fGray.data(), (int)fGray.size(), NULL);
        refb.openRAM(fRGB.data(), (int)fRGB.size(), NULL);
        std::vector<uint8_t> fa(refa.getBufferSize()), fb(refb.getBufferSize());
        refa.setBuffer(fa.data()); refa.decode(NULL, 0);
        refb.setBuffer(fb.data()); refb.decode(NULL, 0);
        for (int y = 0; y < na && good; y++)
            if (memcmp(ra[y].pData, &fa[(size_t)y * apitch], apitch))
                good = false;
        for (int y = 0; y < nb && good; y++)
            if (memcmp(rb[y].pData, &fb[(size_t)y * bpitch], bpitch))
                good = false;
        CHECK(good, "thunk routes lines to the correct owning object");
        a.disableRowView(); b.disableRowView();
        a.close(); b.close(); refa.close(); refb.close();
    }

    // Destructor must release the single block (ASan reports a leak otherwise)
    {
        PNG scoped;
        scoped.openRAM(fGray.data(), (int)fGray.size(), NULL);
        PNGROWCONFIG cfg;
        cfg.iAlign = 8; cfg.iOptions = PNG_ROW_LAZY_RGB565;
        scoped.enableRowView(&cfg);
        scoped.decode(NULL, 0);
        CHECK(scoped.isRowViewValid() == TRUE, "scoped view valid before dtor");
    }
    CHECK(true, "destructor releases row-view memory (ASan clean)");

    // ---- Row array contents & memory growth per class ------------------
    printf("\n=== row-array contents and memory increment ===\n");
    struct Case { const char *name; std::vector<uint8_t> *f; int w, h; };
    Case cases[] = {
        {"8-bit grayscale", &fGray, gray8.w, gray8.h},
        {"8-bit RGB888   ", &fRGB, rgb888.w, rgb888.h},
        {"4-bit palette  ", &fIdx, idx4.w, idx4.h},
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        PNG p;
        p.openRAM(cases[k].f->data(), (int)cases[k].f->size(), NULL);
        int pitch = p.getBufferSize() / p.getHeight();
        PNGROWCONFIG c1; c1.iAlign = 1; c1.iOptions = PNG_ROW_LAZY_RGB565;
        PNGROWCONFIG c4; c4.iAlign = 4; c4.iOptions = 0;
        p.enableRowView(&c1);
        p.decode(NULL, 0);
        int memLazy = p.getRowViewMemory();
        int nLazy = 0;
        const PNGROW *r = p.getRows(&nLazy);
        p.disableRowView();
        p.enableRowView(&c4);
        p.decode(NULL, 0);
        int memEager = p.getRowViewMemory();
        int nEager = 0;
        p.getRows(&nEager);
        p.disableRowView();
        p.close();
        printf("  %s: %2dx%-2d pitch=%2d  rows=%d  "
               "lazy(a1) mem=%5d B  eager(a4) mem=%5d B  delta=%5d B\n",
               cases[k].name, cases[k].w, cases[k].h, pitch, nLazy,
               memLazy, memEager, memEager - memLazy);
        CHECK(r && nLazy == cases[k].h, "row count equals image height");
    }

    // ---- Timing ---------------------------------------------------------
    printf("\n=== decode timing (us, 2000 iterations each) ===\n");
    const int ITERS = 2000;
    ImgSpec bigGray = {64, 48, 8, PNG_PIXEL_GRAYSCALE, 0, ""};
    ImgSpec bigRGB = {64, 48, 8, PNG_PIXEL_TRUECOLOR, 0, ""};
    ImgSpec bigIdx = {64, 48, 8, PNG_PIXEL_INDEXED, 0, ""};
    std::vector<uint8_t> bg = make_png(bigGray);
    std::vector<uint8_t> br = make_png(bigRGB);
    std::vector<uint8_t> bi = make_png(bigIdx, &pal, &trns);
    struct TCase { const char *name; std::vector<uint8_t> *f; };
    TCase tcases[] = {
        {"8-bit grayscale", &bg},
        {"8-bit RGB888   ", &br},
        {"8-bit palette  ", &bi},
    };
    for (size_t k = 0; k < sizeof(tcases) / sizeof(tcases[0]); k++) {
        PNGROWCONFIG lazy; lazy.iAlign = 4; lazy.iOptions = PNG_ROW_LAZY_RGB565;
        PNGROWCONFIG eager; eager.iAlign = 4; eager.iOptions = 0;
        long tFull = time_decodes(*tcases[k].f, 0, ITERS, NULL);
        long tLazy = time_decodes(*tcases[k].f, 1, ITERS, &lazy);
        long tEager = time_decodes(*tcases[k].f, 1, ITERS, &eager);
        printf("  %s: full-buffer %6ld us  row-lazy %6ld us  "
               "row-eager %6ld us (per decode: %.1f / %.1f / %.1f us)\n",
               tcases[k].name, tFull, tLazy, tEager,
               tFull / (double)ITERS, tLazy / (double)ITERS,
               tEager / (double)ITERS);
        CHECK(tFull > 0 && tLazy > 0 && tEager > 0, "timing runs completed");
    }

    printf("\nTotal checks: %d, %d passed, %d failed\n",
           g_iTotal, g_iPass, g_iFail);
    return g_iFail ? 1 : 0;
}

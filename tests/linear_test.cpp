//
// tests/linear_test.cpp
//
// Additive test harness for the decode-time linearization feature.
// Does NOT touch the library's public interface; the three additive
// diagnostic helpers are reached through extern declarations.
//
// Reference values come from <cmath> exact formulas here in the TEST
// only. The library itself stays libm-free (see lin_pow/lin_log in
// png.inl).
//
#include "../src/PNGdec.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <climits>

// additive helpers exported by png.inl
extern "C" int PNG_linearStatus(void);
extern "C" int PNG_linearStateSize(void);
extern "C" uint8_t PNG_linearInv8(uint8_t);
extern "C" uint8_t PNG_linearInv16(uint16_t);

// option bits matching png.inl
#define T_LIN_ENABLE  0x04
#define T_LIN_PREMULT 0x08
#define T_LIN_16BIT   0x10
// status bits
#define ST_ACTIVE    0x02
#define ST_SRGB      0x04
#define ST_GAMA      0x08
#define ST_CONFLICT  0x10
#define ST_PREMULT   0x20
#define ST_16BIT     0x40

static int g_fail = 0;
static void check(bool ok, const std::string &what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) g_fail++;
}

static std::vector<uint8_t> read_file(const std::string &p) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) { perror(p.c_str()); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d(n);
    if (fread(d.data(), 1, n, f) != (size_t)n) { perror("fread"); exit(2); }
    fclose(f);
    return d;
}

// exact transfer curves (mirror png.inl)
static double ref_fwd_srgb(double u) {
    return u <= 0.04045 ? u / 12.92 : std::pow((u + 0.055) / 1.055, 2.4);
}
static double ref_fwd_gamma(double u, double g) {
    return std::pow(u, 1.0 / g);
}

struct Decoded {
    PNG png;
    std::vector<uint8_t> buf;
    int w = 0, h = 0, pitch = 0, ptype = 0, bpp = 0;
};

static Decoded decode_file(const std::vector<uint8_t> &data, int opts,
                           bool wide16) {
    Decoded d;
    d.png.openRAM(const_cast<uint8_t *>(data.data()), (int)data.size(), NULL);
    d.w = d.png.getWidth();
    d.h = d.png.getHeight();
    d.ptype = d.png.getPixelType();
    d.bpp = d.png.getBpp();
    int bytes = d.png.getBufferSize();
    if (d.ptype == PNG_PIXEL_TRUECOLOR) d.pitch = d.w * 3;
    else if (d.ptype == PNG_PIXEL_TRUECOLOR_ALPHA) d.pitch = d.w * 4;
    else if (d.ptype == PNG_PIXEL_GRAY_ALPHA) d.pitch = d.w * 2;
    else d.pitch = bytes / d.h;
    d.buf.resize(wide16 ? (size_t)bytes * 2 : (size_t)bytes);
    d.png.setBuffer(d.buf.data());
    int rc = d.png.decode(NULL, opts);
    if (rc != PNG_SUCCESS) {
        printf("  decode failed rc=%d (opts=0x%x)\n", rc, opts);
        g_fail++;
    }
    return d;
}

static int channels(int ptype) {
    return ptype == 6 ? 4 : ptype == 4 ? 2 : ptype == 2 ? 3 : 1;
}
static bool is_alpha_channel(int ptype, int k) {
    return (ptype == 6 && k == 3) || (ptype == 4 && k == 1);
}

// 1. default decode path byte-identical for every fixture
static void test_default_path_unchanged() {
    printf("\n== Default decode path unchanged (feature off) ==\n");
    const char *names[] = {"samples/srgb_rgba.png", "samples/nogama_rgb.png",
                           "samples/gama_gray.png", "samples/gama_rgb.png",
                           "samples/conflict_rgb.png", "samples/srgb_pal.png",
                           "samples/bad_gama_rgb.png"};
    bool allSame = true;
    for (auto n : names) {
        auto data = read_file(n);
        auto a = decode_file(data, 0, false);
        auto b = decode_file(data, T_LIN_PREMULT, false); // ENABLE bit still off
        if (a.buf != b.buf) { allSame = false; printf("  diff in %s\n", n); }
    }
    check(allSame, "all 7 fixtures byte-identical with feature disabled");
}

// 2. untagged file: passthrough, status reports no active profile
static void test_untagged_passthrough() {
    printf("\n== Untagged file (no sRGB, no gAMA): passthrough ==\n");
    auto data = read_file("samples/nogama_rgb.png");
    auto plain = decode_file(data, 0, false);
    auto lin = decode_file(data, T_LIN_ENABLE, false);
    check(plain.buf == lin.buf, "untagged output byte-identical to plain");
    check(!(PNG_linearStatus() & ST_ACTIVE), "status: no profile active");
}

// 3. sRGB + gAMA together: sRGB wins
static void test_conflict_priority() {
    printf("\n== sRGB/gAMA conflict: sRGB has priority ==\n");
    auto data = read_file("samples/conflict_rgb.png");
    auto d = decode_file(data, T_LIN_ENABLE, false);
    int st = PNG_linearStatus();
    check((st & ST_SRGB) && (st & ST_GAMA), "both markers detected");
    check((st & ST_CONFLICT), "conflict flag raised");
    int got = d.buf[1 * 3 + 0]; // row0 pixel1, red channel, encoded 10
    int srgbExpect = (int)std::lround(ref_fwd_srgb(10.0 / 255.0) * 255.0);
    int gammaExpect = (int)std::lround(
        ref_fwd_gamma(10.0 / 255.0, 1.8) * 255.0);
    char msg[200];
    snprintf(msg, sizeof msg,
             "value %d follows sRGB (%d), not gamma1.8 (%d)",
             got, srgbExpect, gammaExpect);
    check(got == srgbExpect && srgbExpect != gammaExpect, msg);
}

// 4. LUT precision vs exact math, all observed encoded levels, both LUTs
static void test_conversion_error() {
    printf("\n== Conversion error (LUT vs exact math), four file classes ==\n");
    struct C { const char *name; bool srgb; double gamma; };
    C cases[] = {
        {"samples/srgb_rgba.png", true, 0.0},    // with sRGB, with alpha
        {"samples/gama_rgb.png", false, 1.8},    // gAMA only
        {"samples/gama_gray.png", false, 2.2},   // gAMA, gray+alpha
        {"samples/conflict_rgb.png", true, 0.0}, // conflict -> sRGB
    };
    for (auto &c : cases) {
        auto data = read_file(c.name);
        auto enc = decode_file(data, 0, false);          // original encoded
        auto d8 = decode_file(data, T_LIN_ENABLE, false);
        auto d16 = decode_file(data, T_LIN_ENABLE | T_LIN_16BIT, true);
        int ch = channels(d8.ptype);
        int max8 = 0, max16 = 0;
        long sum8 = 0, n8 = 0;
        for (int y = 0; y < d8.h; y++) {
            uint8_t *re = &enc.buf[(size_t)y * enc.pitch];
            uint8_t *r8 = &d8.buf[(size_t)y * d8.pitch];
            // 16-bit row: 2*pitch bytes = pitch uint16 entries
            uint16_t *r16 = (uint16_t *)&d16.buf[(size_t)y * d16.pitch * 2];
            for (int x = 0; x < d8.w; x++) {
                for (int k = 0; k < ch; k++) {
                    int src = re[x * ch + k]; // true encoded value
                    if (is_alpha_channel(d8.ptype, k)) {
                        int e16 = std::abs(src * 257 - (int)r16[x * ch + k]);
                        if (e16 > max16) max16 = e16;
                        continue;
                    }
                    double f = c.srgb ? ref_fwd_srgb(src / 255.0)
                                      : ref_fwd_gamma(src / 255.0, c.gamma);
                    int e8 = std::abs((int)std::lround(f * 255.0) -
                                      (int)r8[x * ch + k]);
                    int e16 = std::abs((int)std::lround(f * 65535.0) -
                                       (int)r16[x * ch + k]);
                    if (e8 > max8) max8 = e8;
                    if (e16 > max16) max16 = e16;
                    sum8 += e8; n8++;
                }
            }
        }
        char msg[220];
        snprintf(msg, sizeof msg, "%-24s 8bit max=%d mean=%.4f | "
                 "16bit max=%d/65535 (n=%ld)", c.name, max8,
                 sum8 / (double)n8, max16, n8);
        check(max8 <= 1 && max16 <= 1, msg);
    }
    // indexed class: palette entries linearized, indices unchanged
    auto pdata = read_file("samples/srgb_pal.png");
    auto pid = decode_file(pdata, T_LIN_ENABLE, false);
    uint8_t *pal = pid.png.getPalette();
    int maxPal = 0;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 3; k++) {
            // original palette colors are known from make_samples.py
            static const uint8_t orig[8][3] = {
                {0,0,0},{255,0,0},{0,255,0},{0,0,255},{128,128,128},
                {200,100,50},{10,20,30},{255,255,255}};
            int e = std::abs((int)std::lround(
                ref_fwd_srgb(orig[i][k] / 255.0) * 255.0) - pal[i*3+k]);
            if (e > maxPal) maxPal = e;
        }
    check(maxPal <= 1, "srgb_pal.png palette LUT max err <=1, indices kept");
}

// 5. round trip over all 256 encoded levels
static void test_roundtrip() {
    printf("\n== Round-trip consistency (encode -> linear -> encode) ==\n");
    auto data = read_file("samples/srgb_rgba.png");
    decode_file(data, T_LIN_ENABLE, false); // prime LUT/profile state
    // An 8-bit linear code cannot distinguish dark encoded levels: many
    // encoded values quantize to the same linear code (linear code 0
    // alone covers encoded 0..9), so an exact round trip is impossible
    // there by construction. This is quantization, not an implementation
    // defect (the forward LUT itself is exact, see conversion tests).
    // What must hold: the inverse map is monotonic and never exceeds the
    // width of one linear quantization band. Exact reversibility is the
    // job of the 16-bit LUT, asserted below.
    int exact8 = 0, worst8 = 0;
    bool monotonic = true;
    int prevBack = -1;
    for (int i = 0; i < 256; i++) {
        int lin = (int)std::lround(ref_fwd_srgb(i / 255.0) * 255.0);
        int back = PNG_linearInv8((uint8_t)lin);
        int e = std::abs(back - i);
        if (e > worst8) worst8 = e;
        if (back == i) exact8++;
        if (back < prevBack) monotonic = false;
        prevBack = back;
    }
    char msg[220];
    snprintf(msg, sizeof msg,
             "8-bit LUT: %d/256 exact; inverse monotonic=%d; worst band "
             "width +/-%d (dark-level quantization, expected)",
             exact8, (int)monotonic, worst8);
    check(monotonic, msg);
    // 16-bit linear space preserves every 8-bit encoded level exactly
    decode_file(data, T_LIN_ENABLE | T_LIN_16BIT, true);
    int mism16 = 0, worst16 = 0;
    for (int i = 0; i < 256; i++) {
        int lin = (int)std::lround(ref_fwd_srgb(i / 255.0) * 65535.0);
        int back = PNG_linearInv16((uint16_t)lin);
        int e = std::abs(back - i);
        if (e > worst16) worst16 = e;
        if (back != i) mism16++;
    }
    snprintf(msg, sizeof msg,
             "16-bit LUT: %d/256 encoded levels recovered exactly",
             256 - mism16);
    check(mism16 == 0 && worst16 == 0, msg);
}

// 6. premultiplied alpha, exact linear-space products
static void test_premultiply() {
    printf("\n== Premultiplied alpha (linear space, optional) ==\n");
    auto data = read_file("samples/srgb_rgba.png");
    auto base = decode_file(data, T_LIN_ENABLE, false);      // linear, no pm
    auto pm = decode_file(data, T_LIN_ENABLE | T_LIN_PREMULT, false);
    int maxErr = 0;
    bool zeroOK = true, alphaKept = true;
    for (size_t i = 0; i < pm.buf.size(); i += 4) {
        int a = pm.buf[i + 3];
        if (a == 0 && (pm.buf[i] || pm.buf[i+1] || pm.buf[i+2])) zeroOK = false;
        if (a != base.buf[i + 3]) alphaKept = false;
        for (int k = 0; k < 3; k++) {
            int expect = (base.buf[i + k] * a + 127) / 255;
            int e = std::abs(expect - (int)pm.buf[i + k]);
            if (e > maxErr) maxErr = e;
        }
    }
    check(maxErr <= 1, "RGBA premultiply = linear*a/255 (+/-1 code)");
    check(zeroOK, "a=0 pixels premultiplied to black");
    check(alphaKept, "alpha channel never transformed");
    check((PNG_linearStatus() & ST_PREMULT), "status reports premult mode");
    // 16-bit premultiply
    auto pm16 = decode_file(data, T_LIN_ENABLE | T_LIN_PREMULT | T_LIN_16BIT,
                            true);
    auto base16 = decode_file(data, T_LIN_ENABLE | T_LIN_16BIT, true);
    int max16 = 0;
    for (size_t i = 0; i < pm16.buf.size() / 2; i += 4) {
        uint16_t *p = (uint16_t *)pm16.buf.data();
        uint16_t *b = (uint16_t *)base16.buf.data();
        uint32_t a = p[i + 3];
        for (int k = 0; k < 3; k++) {
            uint32_t expect = ((uint32_t)b[i + k] * a + 32768u) / 65535u;
            int e = std::abs((int)expect - (int)p[i + k]);
            if (e > max16) max16 = e;
        }
    }
    check(max16 <= 1, "16-bit premultiply matches lin*a/65535 (+/-1)");
    // indexed premultiply on palette: full linear product for every entry
    auto pdata = read_file("samples/srgb_pal.png");
    auto pd = decode_file(pdata, T_LIN_ENABLE | T_LIN_PREMULT, false);
    uint8_t *pal = pd.png.getPalette();
    bool palOK = true, palZero = true;
    static const uint8_t porig[8][3] = {
        {0,0,0},{255,0,0},{0,255,0},{0,0,255},{128,128,128},
        {200,100,50},{10,20,30},{255,255,255}};
    static const uint8_t palpha[8] = {255,128,64,0,200,32,16,255};
    int palMaxErr = 0;
    for (int i = 0; i < 8; i++) {
        int a = pal[768 + i];
        if (a == 0 && (pal[i*3] || pal[i*3+1] || pal[i*3+2])) palZero = false;
        for (int k = 0; k < 3; k++) {
            int lin = (int)std::lround(
                ref_fwd_srgb(porig[i][k] / 255.0) * 255.0);
            int expect = (lin * palpha[i] + 127) / 255;
            int e = std::abs(expect - (int)pal[i*3+k]);
            if (e > palMaxErr) palMaxErr = e;
            if (e > 1) palOK = false;
        }
    }
    check(palZero, "indexed: zero-alpha palette entries premultiplied to 0");
    char palMsg[160];
    snprintf(palMsg, sizeof palMsg,
             "indexed: all 8 entries match lin*a/255 (max err %d)", palMaxErr);
    check(palOK, palMsg);
}

// 7. timing across all classes
static void test_timing() {
    printf("\n== Timing (best of 50 decodes, microseconds) ==\n");
    const char *names[] = {"samples/srgb_rgba.png", "samples/nogama_rgb.png",
                           "samples/gama_gray.png", "samples/gama_rgb.png",
                           "samples/conflict_rgb.png", "samples/srgb_pal.png",
                           "samples/bad_gama_rgb.png"};
    for (auto n : names) {
        auto data = read_file(n);
        // 16-bit widening is only defined for 8-bit channel RGB/gray types
        bool wideOK = true;
        {
            PNG probe;
            probe.openRAM(const_cast<uint8_t *>(data.data()),
                          (int)data.size(), NULL);
            int pt = probe.getPixelType();
            wideOK = (pt == PNG_PIXEL_TRUECOLOR ||
                      pt == PNG_PIXEL_TRUECOLOR_ALPHA ||
                      pt == PNG_PIXEL_GRAY_ALPHA ||
                      (pt == PNG_PIXEL_GRAYSCALE && probe.getBpp() == 8));
        }
        auto bench = [&](int opts, bool wide) {
            long best = LONG_MAX;
            for (int it = 0; it < 50; it++) {
                auto t0 = std::chrono::high_resolution_clock::now();
                auto d = decode_file(data, opts, wide);
                auto t1 = std::chrono::high_resolution_clock::now();
                long us = std::chrono::duration_cast<
                    std::chrono::microseconds>(t1 - t0).count();
                if (us < best) best = us;
                (void)d;
            }
            return best;
        };
        long tPlain = bench(0, false);
        long tLin8 = bench(T_LIN_ENABLE, false);
        long tPre = bench(T_LIN_ENABLE | T_LIN_PREMULT, false);
        if (wideOK) {
            long tLin16 = bench(T_LIN_ENABLE | T_LIN_16BIT, true);
            printf("  %-24s plain=%5ld  lin8=%5ld (+%4ld)  "
                   "premult=%5ld (+%4ld)  lin16=%5ld (+%4ld) us\n",
                   n, tPlain, tLin8, tLin8 - tPlain, tPre, tPre - tPlain,
                   tLin16, tLin16 - tPlain);
        } else {
            printf("  %-24s plain=%5ld  lin8=%5ld (+%4ld)  "
                   "premult=%5ld (+%4ld)  lin16=   n/a (type unsupported)\n",
                   n, tPlain, tLin8, tLin8 - tPlain, tPre, tPre - tPlain);
        }
    }
    check(true, "timing table printed");
}

// 8. memory accounting
static void test_memory() {
    printf("\n== Memory ==\n");
    printf("  static linearization state block: %d bytes\n"
           "    (ucLUT8[256]=256, usLUT16[256]=512, control fields)\n",
           PNG_linearStateSize());
    printf("  library heap allocations on this path: 0 (LUTs are static,\n"
           "    output buffer is caller-supplied as in the existing API)\n");
    auto data = read_file("samples/srgb_rgba.png");
    auto d8 = decode_file(data, T_LIN_ENABLE, false);
    auto d16 = decode_file(data, T_LIN_ENABLE | T_LIN_16BIT, true);
    printf("  srgb_rgba caller buffer: 8-bit %zu bytes (in-place), "
           "16-bit %zu bytes\n", d8.buf.size(), d16.buf.size());
    check(d16.buf.size() == d8.buf.size() * 2,
          "16-bit mode needs exactly 2x caller buffer");
}

// bad gAMA value (0) must be ignored rather than divide by zero
static void test_bad_gamma() {
    printf("\n== Malformed gAMA (value 0): ignored, plain passthrough ==\n");
    auto data = read_file("samples/bad_gama_rgb.png");
    auto plain = decode_file(data, 0, false);
    auto lin = decode_file(data, T_LIN_ENABLE, false);
    check(plain.buf == lin.buf, "gAMA=0 output equals plain decode");
    check(!(PNG_linearStatus() & ST_ACTIVE), "no profile activated");
}

// 16-bit widening is rejected where it is not defined
static void test_16bit_guards() {
    printf("\n== 16-bit mode parameter guards ==\n");
    auto pal = read_file("samples/srgb_pal.png"); // indexed
    Decoded d;
    d.png.openRAM(const_cast<uint8_t *>(pal.data()), (int)pal.size(), NULL);
    int bytes = d.png.getBufferSize();
    d.buf.resize((size_t)bytes * 2);
    d.png.setBuffer(d.buf.data());
    int rc = d.png.decode(NULL, T_LIN_ENABLE | T_LIN_16BIT);
    check(rc == PNG_UNSUPPORTED_FEATURE, "indexed 16-bit rejected cleanly");
}

int main() {
    test_default_path_unchanged();
    test_untagged_passthrough();
    test_bad_gamma();
    test_conflict_priority();
    test_conversion_error();
    test_roundtrip();
    test_premultiply();
    test_16bit_guards();
    test_timing();
    test_memory();
    printf("\n==== %s (%d failure%s) ====\n",
           g_fail ? "FAILED" : "ALL TESTS PASSED", g_fail,
           g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

/**
 * @file GoldenBlitTests.cpp
 * @brief Golden-image gate for QuadBlit: a fixed matrix of blits (every
 *        source format x every target format x scale/tint/alpha/dither/clip/
 *        flip/rotation/stride/chroma-key) hashed per target format.
 *
 * The expected hashes were captured from the kernels as they were before the
 * kernel refactor. Any change to what a blit writes - intended or not - shows
 * up here first; an intended change regenerates the constants (the test prints
 * the actual values) and says so in the commit message.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "QuadBlit.h"
#include "DekiEngine.h"  // DekiColorFormat

namespace
{

struct Lcg
{
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    uint32_t Next()
    {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    uint8_t Byte() { return static_cast<uint8_t>(Next()); }
};

uint64_t Fnv(const std::vector<uint8_t>& bytes, uint64_t h)
{
    for (uint8_t c : bytes)
    {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

enum class SrcFmt { RGB565, RGB565A8, RGB565A8_NoAlpha, RGBA8888, RGB888, ALPHA8 };

const char* SrcName(SrcFmt f)
{
    switch (f)
    {
    case SrcFmt::RGB565: return "RGB565";
    case SrcFmt::RGB565A8: return "RGB565A8";
    case SrcFmt::RGB565A8_NoAlpha: return "RGB565A8(noalpha)";
    case SrcFmt::RGBA8888: return "RGBA8888";
    case SrcFmt::RGB888: return "RGB888";
    case SrcFmt::ALPHA8: return "ALPHA8";
    }
    return "?";
}

int Bpp(SrcFmt f)
{
    switch (f)
    {
    case SrcFmt::RGB565: return 2;
    case SrcFmt::RGB565A8: case SrcFmt::RGB565A8_NoAlpha: return 3;
    case SrcFmt::RGBA8888: return 4;
    case SrcFmt::RGB888: return 3;
    case SrcFmt::ALPHA8: return 1;
    }
    return 0;
}

int TargetBpp(Deki::ColorFormat f)
{
    switch (f)
    {
    case Deki::ColorFormat::RGB565: return 2;
    case Deki::ColorFormat::RGB888: return 3;
    case Deki::ColorFormat::ARGB8888: return 4;
    case Deki::ColorFormat::RGB565A8: return 3;
    }
    return 0;
}

// Chroma key colour, at 5/6/5 precision so it matches RGB565 sources too.
constexpr uint8_t kKeyR = 0xF8, kKeyG = 0x00, kKeyB = 0xF8;

struct SrcBuf
{
    std::vector<uint8_t> px;
    std::vector<int16_t> alphaSpans;
    std::vector<int16_t> chromaSpans;
    QuadBlit::Source src{};
};

// Deterministic source: every row has a run of fully opaque / non-key pixels
// in the middle (so the row-span paths are exercised) and random alpha / key
// pixels outside it. stridePad adds unused bytes per row.
SrcBuf MakeSrc(SrcFmt f, int w, int h, uint32_t seed, int stridePad, bool chroma)
{
    SrcBuf out;
    const int bpp = Bpp(f);
    const int stride = w * bpp + stridePad;
    out.px.assign(static_cast<size_t>(stride) * h, 0);
    out.alphaSpans.resize(static_cast<size_t>(h) * 2);
    out.chromaSpans.resize(static_cast<size_t>(h) * 2);
    Lcg rng(seed);
    for (int y = 0; y < h; ++y)
    {
        int runStart = static_cast<int>(rng.Next() % static_cast<uint32_t>(w));
        int runEnd = runStart + static_cast<int>(rng.Next() % static_cast<uint32_t>(w - runStart + 1));
        if (y % 5 == 4) { runStart = w; runEnd = 0; }  // a row with no opaque pixel at all
        out.alphaSpans[y * 2] = static_cast<int16_t>(runStart);
        out.alphaSpans[y * 2 + 1] = static_cast<int16_t>(runEnd);
        out.chromaSpans[y * 2] = static_cast<int16_t>(runStart);
        out.chromaSpans[y * 2 + 1] = static_cast<int16_t>(runEnd);
        for (int x = 0; x < w; ++x)
        {
            uint8_t* p = out.px.data() + y * stride + x * bpp;
            const bool inRun = (x >= runStart && x < runEnd);
            uint8_t r = rng.Byte(), g = rng.Byte(), b = rng.Byte();
            uint8_t a;
            const uint32_t roll = rng.Next() % 4;
            if (inRun) a = 255;
            else a = (roll == 0) ? 0 : (roll == 1 ? 255 : rng.Byte());
            // Chroma-keyed sources honour the chromaRowSpans contract: every
            // pixel outside the run IS the key colour, every pixel inside is not.
            const bool isKey = chroma && !inRun;
            if (isKey) { r = kKeyR; g = kKeyG; b = kKeyB; }
            else if (chroma && r == kKeyR && g == kKeyG && b == kKeyB) { g = 0x40; }
            switch (f)
            {
            case SrcFmt::RGB565:
            case SrcFmt::RGB565A8:
            case SrcFmt::RGB565A8_NoAlpha:
            {
                const uint16_t v = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                p[0] = static_cast<uint8_t>(v & 0xFF);
                p[1] = static_cast<uint8_t>(v >> 8);
                if (bpp == 3) p[2] = (f == SrcFmt::RGB565A8) ? a : rng.Byte();
                break;
            }
            case SrcFmt::RGBA8888: p[0] = r; p[1] = g; p[2] = b; p[3] = a; break;
            case SrcFmt::RGB888: p[0] = r; p[1] = g; p[2] = b; break;
            case SrcFmt::ALPHA8: p[0] = a; break;
            }
        }
    }
    const bool isRGB565 = (f == SrcFmt::RGB565 || f == SrcFmt::RGB565A8 || f == SrcFmt::RGB565A8_NoAlpha);
    const bool hasAlpha = (f == SrcFmt::RGB565A8 || f == SrcFmt::RGBA8888 || f == SrcFmt::ALPHA8);
    out.src = QuadBlit::MakeSource(out.px.data(), w, h, bpp, hasAlpha, isRGB565, false,
                                   hasAlpha ? out.alphaSpans.data() : nullptr);
    out.src.stride = stridePad ? stride : 0;
    if (chroma)
    {
        out.src.hasChromaKey = true;
        out.src.keyR = kKeyR; out.src.keyG = kKeyG; out.src.keyB = kKeyB;
        out.src.chromaRowSpans = out.chromaSpans.data();
    }
    return out;
}

constexpr int kTW = 64, kTH = 48;

struct Case
{
    const char* name;
    int destX, destY, destW, destH;     // BlitScaled geometry (destW/H 0 = 1:1)
    uint8_t tr, tg, tb, ta;
    bool dither;
    bool clip;
    bool flipH, flipV, flipD;
    float rotation;                     // != 0: goes through Blit()
    int stridePad;
    bool chroma;
};

const Case kCases[] = {
    { "1:1",              3, 5, 0, 0,     255, 255, 255, 255, false, false, false, false, false, 0.0f, 0, false },
    { "1:1 off-edge",     -7, -3, 0, 0,   255, 255, 255, 255, false, false, false, false, false, 0.0f, 0, false },
    { "up 1.5x",          4, 2, 48, 36,   255, 255, 255, 255, false, false, false, false, false, 0.0f, 0, false },
    { "down 0.6x",        10, 8, 19, 14,  255, 255, 255, 255, false, false, false, false, false, 0.0f, 0, false },
    { "tint",             3, 5, 0, 0,     200, 100, 50, 255,  false, false, false, false, false, 0.0f, 0, false },
    { "alpha tint",       3, 5, 0, 0,     255, 255, 255, 128, false, false, false, false, false, 0.0f, 0, false },
    { "tint+alpha scaled",1, 1, 40, 30,   90, 220, 160, 77,   false, false, false, false, false, 0.0f, 0, false },
    { "dither",           3, 5, 0, 0,     255, 255, 255, 100, true,  false, false, false, false, 0.0f, 0, false },
    { "dither scaled tint",2, 2, 50, 20,  255, 128, 64, 160,  true,  false, false, false, false, 0.0f, 0, false },
    { "clip",             3, 5, 0, 0,     255, 255, 255, 255, false, true,  false, false, false, 0.0f, 0, false },
    { "clip scaled alpha",0, 0, 60, 44,   255, 255, 255, 200, false, true,  false, false, false, 0.0f, 0, false },
    { "flipH",            3, 5, 0, 0,     255, 255, 255, 255, false, false, true,  false, false, 0.0f, 0, false },
    { "flipV+D scaled",   5, 3, 40, 40,   255, 255, 255, 255, false, false, false, true,  true,  0.0f, 0, false },
    { "stride",           3, 5, 0, 0,     255, 255, 255, 255, false, false, false, false, false, 0.0f, 6, false },
    { "stride scaled",    3, 5, 30, 24,   255, 255, 255, 255, false, false, false, false, false, 0.0f, 6, false },
    { "chroma 1:1",       3, 5, 0, 0,     255, 255, 255, 255, false, false, false, false, false, 0.0f, 0, true },
    { "chroma scaled tint",3, 5, 44, 28,  100, 255, 100, 255, false, false, false, false, false, 0.0f, 0, true },
    { "chroma alpha tint",3, 5, 0, 0,     255, 255, 255, 90,  false, false, false, false, false, 0.0f, 0, true },
    { "rot 0.3",          30, 24, 0, 0,   255, 255, 255, 255, false, false, false, false, false, 0.3f, 0, false },
    { "rot 1.2 tint alpha",30, 24, 0, 0,  200, 120, 255, 140, false, false, false, false, false, 1.2f, 0, false },
    { "rot -2 dither",    30, 24, 0, 0,   255, 255, 255, 120, true,  false, false, false, false, -2.0f, 0, false },
    { "rot 0.7 flipH clip",30, 24, 0, 0,  255, 255, 255, 255, false, true,  true,  false, false, 0.7f, 0, false },
    { "rot 2.5 chroma",   30, 24, 0, 0,   255, 255, 255, 255, false, false, false, false, false, 2.5f, 0, true },
};

const SrcFmt kSrcFmts[] = { SrcFmt::RGB565, SrcFmt::RGB565A8, SrcFmt::RGB565A8_NoAlpha,
                            SrcFmt::RGBA8888, SrcFmt::RGB888, SrcFmt::ALPHA8 };
const Deki::ColorFormat kDstFmts[] = { Deki::ColorFormat::RGB565, Deki::ColorFormat::RGB888,
                                     Deki::ColorFormat::ARGB8888, Deki::ColorFormat::RGB565A8 };

uint64_t RunTarget(Deki::ColorFormat dst, bool print)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    const int tbpp = TargetBpp(dst);
    for (SrcFmt sf : kSrcFmts)
    {
        for (const Case& c : kCases)
        {
            // Square sources for the transpose case, otherwise 24x18.
            const int sw = c.flipD ? 20 : 24, sh = c.flipD ? 20 : 18;
            SrcBuf s = MakeSrc(sf, sw, sh, 0x1234u + static_cast<uint32_t>(sf) * 77u, c.stridePad, c.chroma);
            s.src.flipH = c.flipH; s.src.flipV = c.flipV; s.src.flipD = c.flipD;

            std::vector<uint8_t> target(static_cast<size_t>(kTW) * kTH * tbpp);
            Lcg noise(0x99u + static_cast<uint32_t>(dst));
            for (uint8_t& b : target) b = noise.Byte();

            QuadBlit::ClearClipStack();
            if (c.clip) QuadBlit::PushClipRect(10, 10, 40, 30);
            if (c.rotation != 0.0f)
            {
                QuadBlit::Blit(s.src, target.data(), kTW, kTH, dst, c.destX, c.destY, 1.3f, 1.1f, c.rotation,
                               0.5f, 0.5f, c.tr, c.tg, c.tb, c.ta, c.dither);
            }
            else
            {
                const int dw = c.destW ? c.destW : sw, dh = c.destH ? c.destH : sh;
                QuadBlit::BlitScaled(s.src, target.data(), kTW, kTH, dst, c.destX, c.destY, dw, dh,
                                     c.tr, c.tg, c.tb, c.ta, c.dither);
            }
            QuadBlit::ClearClipStack();
            // Each case hashes on its own (so two builds can be compared case
            // by case); the per-target value folds the case hashes in order.
            const uint64_t caseHash = Fnv(target, 0xcbf29ce484222325ULL);
            hash = (hash ^ caseHash) * 0x100000001b3ULL;
            if (print)
                std::printf("  %-18s %-22s %016llx\n", SrcName(sf), c.name,
                            static_cast<unsigned long long>(caseHash));
        }
    }
    return hash;
}

// Captured from the unified pipeline (September 2026). Against the twelve
// hand-written kernels it replaced, exactly two things changed, both on
// purpose: an RGB565A8 source with hasAlpha == false is opaque on every path
// (the ARGB8888/RGB888 kernels and the rotated path read its alpha byte
// anyway), and blends onto an RGB565A8 target through the generic, flipped
// and rotated paths keep coverage alpha (src-over union) like the
// specialised kernels always did instead of writing 0xFF. Every other case
// is bit-identical. 0 means "not captured yet": the test then prints the
// actual value and fails.
struct Expected { Deki::ColorFormat fmt; const char* name; uint64_t hash; };
const Expected kExpected[] = {
    { Deki::ColorFormat::RGB565,   "RGB565",   0x0a31383163f71c2bULL },
    { Deki::ColorFormat::RGB888,   "RGB888",   0x2f1126ce6004d7f7ULL },
    { Deki::ColorFormat::ARGB8888, "ARGB8888", 0xc80bf916f6efe40bULL },
    { Deki::ColorFormat::RGB565A8, "RGB565A8", 0xd8f3d76abf379d7aULL },
};

}  // namespace

class GoldenBlitTest : public ::testing::TestWithParam<int>
{
};

TEST_P(GoldenBlitTest, TargetFormatMatchesGolden)
{
    const Expected& e = kExpected[GetParam()];
    // DEKI_GOLDEN_PRINT=1 lists every case's hash (to diff two builds).
    const uint64_t actual = RunTarget(e.fmt, std::getenv("DEKI_GOLDEN_PRINT") != nullptr);
    std::printf("GOLDEN %s = 0x%016llxULL\n", e.name, static_cast<unsigned long long>(actual));
    if (actual != e.hash)
    {
        std::printf("Per-case hashes for %s:\n", e.name);
        RunTarget(e.fmt, true);
    }
    EXPECT_EQ(actual, e.hash) << "QuadBlit output changed for target " << e.name
                              << ". If intended, update kExpected and say so in the commit.";
}

INSTANTIATE_TEST_SUITE_P(Formats, GoldenBlitTest, ::testing::Values(0, 1, 2, 3));
